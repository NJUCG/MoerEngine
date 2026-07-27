#pragma once

#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

#include <functional>
#include <stdexcept>
#include <utility>

namespace Moer::Render::Raytracing {

enum class EExportSubmissionOutcome : uint8 {
    NotSubmitted,
    Accepted,
    Rejected,
    Failed,
};

struct ExportFrameSubmissionDecision {
    EExportSubmissionOutcome source_outcome{
        EExportSubmissionOutcome::NotSubmitted
    };
    EExportSubmissionOutcome readback_outcome{
        EExportSubmissionOutcome::NotSubmitted
    };
    EExportSubmissionOutcome tail_outcome{
        EExportSubmissionOutcome::NotSubmitted
    };
    bool source_accepted{true};
    bool tail_accepted{false};
    bool readback_attempted{false};
    bool readback_accepted{false};
    bool encoder_dispatched{false};
    bool export_consumed{false};
    bool retry_requested{false};
    bool frame_accepted{false};
    bool independent_source_failed{false};
    bool retryable_frame_rejection{false};
    bool latch_renderer{false};
};

// Couples a backend-facing raw CmdSubmit fence identity with Completion-owned
// strong references. The same production transaction owns render-prefix,
// readback, encoder-dispatch, dependent-tail, retry and latch decisions.
class ExportSubmissionTransaction final {
public:
    ExportSubmissionTransaction() = default;

    explicit ExportSubmissionTransaction(
        FenceRef _receipt,
        uint64   _value = 1
    ) {
        Reset(std::move(_receipt), _value);
    }

    ExportSubmissionTransaction(const ExportSubmissionTransaction&) = delete;
    ExportSubmissionTransaction& operator=(const ExportSubmissionTransaction&) = delete;
    ExportSubmissionTransaction(ExportSubmissionTransaction&&) noexcept = default;
    ExportSubmissionTransaction& operator=(ExportSubmissionTransaction&&) noexcept = default;

    void Reset(FenceRef _receipt, uint64 _value = 1) {
        if (!_receipt.IsValid() || _value == 0) {
            throw std::invalid_argument(
                "ExportSubmissionTransaction requires a valid receipt and non-zero value"
            );
        }
        receipt = std::move(_receipt);
        value   = _value;
        readback_receipt    = FenceRef{};
        readback_value     = 0;
        readback_outcome   = EExportSubmissionOutcome::NotSubmitted;
        encoder_dispatched = false;
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return receipt.IsValid();
    }

    [[nodiscard]] Fence* GetReceiptFence() const noexcept {
        return receipt.Get();
    }

    [[nodiscard]] uint64 GetReceiptValue() const noexcept {
        return value;
    }

    void AttachSignal(CmdSubmit& _submit) const {
        RequireActive();
        AttachReceiptSignal(_submit, receipt, value);
    }

    void AttachDependentWait(CmdSubmit& _submit) const {
        RequireActive();
        AttachReceiptWait(_submit, receipt, value);
    }

    [[nodiscard]] bool SourceAccepted() const {
        return !IsActive() ||
               SourceOutcome() == EExportSubmissionOutcome::Accepted;
    }

    [[nodiscard]] EExportSubmissionOutcome SourceOutcome() const {
        return IsActive() ?
                   ResolveReceiptOutcome(receipt.Get(), value) :
                   EExportSubmissionOutcome::NotSubmitted;
    }

    [[nodiscard]] bool FoldAcceptance(bool _dependent_accepted) const {
        const bool source_accepted = SourceAccepted();
        return source_accepted && _dependent_accepted;
    }

    void BeginReadback(FenceRef _receipt, uint64 _value = 1) {
        RequireActive();
        if (readback_receipt.IsValid()) {
            throw std::logic_error(
                "ExportSubmissionTransaction readback is already active"
            );
        }
        if (!_receipt.IsValid() || _value == 0) {
            throw std::invalid_argument(
                "ExportSubmissionTransaction requires a valid readback receipt and non-zero value"
            );
        }
        readback_receipt = std::move(_receipt);
        readback_value   = _value;
    }

    void AttachReadbackSignal(CmdSubmit& _submit) const {
        RequireReadbackActive();
        AttachReceiptSignal(_submit, readback_receipt, readback_value);
    }

    [[nodiscard]] bool ResolveReadbackAcceptance() {
        RequireReadbackActive();
        if (readback_outcome == EExportSubmissionOutcome::NotSubmitted) {
            readback_outcome = ResolveReceiptOutcome(
                readback_receipt.Get(),
                readback_value
            );
        }
        return readback_outcome == EExportSubmissionOutcome::Accepted;
    }

    template <typename EncoderDispatch>
    [[nodiscard]] bool DispatchEncoder(EncoderDispatch&& _dispatch) {
        if (!ResolveReadbackAcceptance() || encoder_dispatched) {
            return false;
        }
        std::invoke(std::forward<EncoderDispatch>(_dispatch));
        encoder_dispatched = true;
        return true;
    }

    [[nodiscard]] Fence* GetReadbackReceiptFence() const noexcept {
        return readback_receipt.Get();
    }

    [[nodiscard]] uint64 GetReadbackReceiptValue() const noexcept {
        return readback_value;
    }

    [[nodiscard]] EExportSubmissionOutcome ReadbackOutcome() const noexcept {
        return readback_outcome;
    }

    [[nodiscard]] static EExportSubmissionOutcome ResolveReceiptOutcome(
        Fence* _receipt,
        uint64 _value
    ) {
        if (_receipt == nullptr || _value == 0) {
            return EExportSubmissionOutcome::Failed;
        }
        if (_receipt->WaitSubmitted(_value)) {
            return EExportSubmissionOutcome::Accepted;
        }
        // Every recoverable pre-native rejection must terminalize the exact
        // value through Fence::Reject(). A false wait without that marker is
        // therefore a fence/backend/device hard failure.
        return _receipt->IsRejected(_value) ?
                   EExportSubmissionOutcome::Rejected :
                   EExportSubmissionOutcome::Failed;
    }

    [[nodiscard]] ExportFrameSubmissionDecision ClassifyFrameAcceptance(
        EExportSubmissionOutcome _tail_outcome,
        bool _dependent_sources_accepted,
        bool _independent_source_failed
    ) const {
        return ResolveFrameDecision(
            IsActive(),
            SourceOutcome(),
            readback_outcome,
            _tail_outcome,
            encoder_dispatched,
            _dependent_sources_accepted,
            _independent_source_failed
        );
    }

    [[nodiscard]] static ExportFrameSubmissionDecision ResolveFrameDecision(
        bool _export_active,
        EExportSubmissionOutcome _source_outcome,
        EExportSubmissionOutcome _readback_outcome,
        EExportSubmissionOutcome _tail_outcome,
        bool _encoder_dispatched,
        bool _dependent_sources_accepted,
        bool _independent_source_failed
    ) {
        ExportFrameSubmissionDecision decision{};
        decision.source_outcome =
            _source_outcome;
        decision.readback_outcome =
            _readback_outcome;
        decision.tail_outcome =
            _tail_outcome;
        decision.source_accepted =
            !_export_active ||
            decision.source_outcome ==
                EExportSubmissionOutcome::Accepted;
        decision.tail_accepted =
            decision.tail_outcome ==
            EExportSubmissionOutcome::Accepted;
        decision.readback_attempted =
            decision.readback_outcome !=
            EExportSubmissionOutcome::NotSubmitted;
        decision.readback_accepted =
            decision.readback_outcome ==
            EExportSubmissionOutcome::Accepted;
        decision.encoder_dispatched =
            _encoder_dispatched;
        decision.export_consumed =
            decision.encoder_dispatched &&
            decision.readback_accepted;
        decision.retry_requested =
            _export_active && !decision.export_consumed;
        decision.independent_source_failed =
            _independent_source_failed ||
            decision.readback_outcome ==
                EExportSubmissionOutcome::Failed ||
            (decision.encoder_dispatched &&
             !decision.readback_accepted);
        decision.frame_accepted =
            decision.source_accepted &&
            decision.tail_accepted &&
            _dependent_sources_accepted &&
            !decision.independent_source_failed;
        decision.retryable_frame_rejection =
            _export_active &&
            decision.source_outcome ==
                EExportSubmissionOutcome::Rejected &&
            decision.tail_outcome ==
                EExportSubmissionOutcome::Rejected &&
            !decision.independent_source_failed;
        decision.latch_renderer =
            decision.independent_source_failed ||
            (!decision.frame_accepted &&
             !decision.retryable_frame_rejection);
        return decision;
    }

private:
    static void AttachReceiptSignal(
        CmdSubmit&      _submit,
        const FenceRef& _receipt,
        uint64          _value
    ) {
        std::function<void()> keepalive = [fence = _receipt] {};
        _submit.callbacks.reserve(_submit.callbacks.size() + 1);
        _submit.signal_events.reserve(_submit.signal_events.size() + 1);
        _submit.signal_rejection_keepalives.reserve(
            _submit.signal_rejection_keepalives.size() + 1
        );
        _submit.callbacks.emplace_back(std::move(keepalive));
        _submit.signal_rejection_keepalives.emplace_back(_receipt);
        _submit.Signal(_receipt.Get(), _value);
    }

    static void AttachReceiptWait(
        CmdSubmit&      _submit,
        const FenceRef& _receipt,
        uint64          _value
    ) {
        std::function<void()> keepalive = [fence = _receipt] {};
        _submit.callbacks.reserve(_submit.callbacks.size() + 1);
        _submit.wait_events.reserve(_submit.wait_events.size() + 1);
        _submit.callbacks.emplace_back(std::move(keepalive));
        _submit.Wait(_receipt.Get(), _value);
    }

    void RequireActive() const {
        if (!IsActive()) {
            throw std::logic_error(
                "ExportSubmissionTransaction has no active receipt"
            );
        }
    }

    void RequireReadbackActive() const {
        if (!readback_receipt.IsValid()) {
            throw std::logic_error(
                "ExportSubmissionTransaction has no active readback receipt"
            );
        }
    }

    FenceRef receipt{};
    uint64   value{0};
    FenceRef readback_receipt{};
    uint64   readback_value{0};
    EExportSubmissionOutcome readback_outcome{
        EExportSubmissionOutcome::NotSubmitted
    };
    bool     encoder_dispatched{false};
};

} // namespace Moer::Render::Raytracing
