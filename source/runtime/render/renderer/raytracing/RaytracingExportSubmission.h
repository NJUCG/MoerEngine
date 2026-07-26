#pragma once

#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

#include <functional>
#include <stdexcept>
#include <utility>

namespace Moer::Render::Raytracing {

// Couples a backend-facing raw CmdSubmit fence identity with Completion-owned
// strong references and provides the one acceptance rule shared by export
// sources and their dependent frame tails.
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

        std::function<void()> keepalive = [fence = receipt] {};
        _submit.callbacks.reserve(_submit.callbacks.size() + 1);
        _submit.signal_events.reserve(_submit.signal_events.size() + 1);
        _submit.callbacks.emplace_back(std::move(keepalive));
        _submit.Signal(receipt.Get(), value);
    }

    void AttachDependentWait(CmdSubmit& _submit) const {
        RequireActive();

        std::function<void()> keepalive = [fence = receipt] {};
        _submit.callbacks.reserve(_submit.callbacks.size() + 1);
        _submit.wait_events.reserve(_submit.wait_events.size() + 1);
        _submit.callbacks.emplace_back(std::move(keepalive));
        _submit.Wait(receipt.Get(), value);
    }

    [[nodiscard]] bool SourceAccepted() const {
        return !IsActive() || receipt->WaitSubmitted(value);
    }

    [[nodiscard]] bool FoldAcceptance(bool _dependent_accepted) const {
        const bool source_accepted = SourceAccepted();
        return source_accepted && _dependent_accepted;
    }

private:
    void RequireActive() const {
        if (!IsActive()) {
            throw std::logic_error(
                "ExportSubmissionTransaction has no active receipt"
            );
        }
    }

    FenceRef receipt{};
    uint64   value{0};
};

} // namespace Moer::Render::Raytracing
