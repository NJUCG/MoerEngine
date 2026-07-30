#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIGpuScope.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "rhi/RHIThreadOwnership.h"
#include "rhi/vulkan/VulkanCustomCommand.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanIOService.h"
#include "rhi/vulkan/VulkanQueue.h"
#include "rhi/vulkan/VulkanRHIResource.h"
#include "rhi/vulkan/VulkanSubmissionDiagnostics.h"
#include "renderer/raytracing/RaytracingExportSubmission.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Moer;
using namespace Moer::Render;
using Moer::Render::Raytracing::EExportSubmissionOutcome;
using Moer::Render::Raytracing::ExportSubmissionTransaction;

namespace {

constexpr size_t kElementCount = 64;
constexpr uint32 kIterations   = 24;
constexpr uint32 kHeavyCopiesPerWave = 48;
constexpr uint32 kHeavyCopyCount = kHeavyCopiesPerWave * 2;

bool IsExpectedZeroOcclusionResult(
    const OcclusionQueryResult* _result,
    bool                        _precise
) {
    if (_result == nullptr || _result->visible) {
        return false;
    }
    return _precise ?
               _result->sample_count ==
                   std::optional<std::uint64_t>{0} :
               !_result->sample_count.has_value();
}

class TranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    explicit TranslateProbeCommand(
        std::binary_semaphore* _translated,
        EQueueType             _queue = EQueueType::Graphics
    ) :
        translated(_translated),
        queue(_queue) {}

    void Execute(const VkDispatchContext&) const override {
        translated->release();
    }

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }

    std::binary_semaphore* translated;
    EQueueType             queue;
};

class OpaquePresentationStateProbeCommand final : public CustomCmd {
public:
    OpaquePresentationStateProbeCommand() :
        CustomCmd(
            CustomCmdId::CUSTOM_CMD_NONE,
            "OpaquePresentationStateProbe"
        ) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }
};

class BindlessPresentationAccessProbeCommand final
    : public VkCustomDispatchCmd {
public:
    explicit BindlessPresentationAccessProbeCommand(
        BindlessArrayRef _bindless,
        EQueueType       _queue = EQueueType::Graphics
    ) :
        queue(_queue) {
        usages.emplace_back(
            std::move(_bindless),
            ParamInfoFlags{
                .state_flags    = 0,
                .pipeline_flags = 0,
            }
        );
    }

    void Execute(const VkDispatchContext&) const override {}

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::span<const ResourceUsage>
    GetResourceUsages() const override {
        return usages;
    }

    Array<ResourceUsage> usages{};
    EQueueType           queue{EQueueType::Graphics};
};

class PipelineTranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    PipelineTranslateProbeCommand(
        EQueueType               _queue,
        std::binary_semaphore*   _translated,
        std::atomic<uint32>*     _translate_count,
        const std::atomic<bool>* _dependency_release_started,
        std::atomic<bool>*       _observed_dependency_release
    ) :
        queue(_queue),
        translated(_translated),
        translate_count(_translate_count),
        dependency_release_started(_dependency_release_started),
        observed_dependency_release(_observed_dependency_release) {}

    void Execute(const VkDispatchContext&) const override {
        observed_dependency_release->store(
            dependency_release_started->load(std::memory_order_acquire),
            std::memory_order_release
        );
        translate_count->fetch_add(1, std::memory_order_acq_rel);
        translated->release();
    }

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }

    EQueueType               queue;
    std::binary_semaphore*   translated;
    std::atomic<uint32>*     translate_count;
    const std::atomic<bool>* dependency_release_started;
    std::atomic<bool>*       observed_dependency_release;
};

class BlockingTranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    BlockingTranslateProbeCommand(
        EQueueType               _queue,
        std::binary_semaphore*   _entered,
        std::binary_semaphore*   _release,
        std::atomic<bool>*       _finished                      = nullptr,
        const std::atomic<bool>* _predecessor_finished          = nullptr,
        std::atomic<bool>*       _observed_predecessor_finished = nullptr
    ) :
        queue(_queue),
        entered(_entered),
        release(_release),
        finished(_finished),
        predecessor_finished(_predecessor_finished),
        observed_predecessor_finished(_observed_predecessor_finished) {}

    void Execute(const VkDispatchContext&) const override {
        if (predecessor_finished != nullptr && observed_predecessor_finished != nullptr) {
            observed_predecessor_finished->store(
                predecessor_finished->load(std::memory_order_acquire), std::memory_order_release
            );
        }
        entered->release();
        release->acquire();
        if (finished != nullptr) {
            finished->store(true, std::memory_order_release);
        }
    }

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }

    EQueueType               queue;
    std::binary_semaphore*   entered;
    std::binary_semaphore*   release;
    std::atomic<bool>*       finished;
    const std::atomic<bool>* predecessor_finished;
    std::atomic<bool>*       observed_predecessor_finished;
};

class ThrowingTranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    explicit ThrowingTranslateProbeCommand(bool) {}

    void Execute(const VkDispatchContext&) const override {
        throw std::runtime_error("injected outer Translate failure");
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }
};

class OneShotSemaphoreRelease final {
public:
    explicit OneShotSemaphoreRelease(std::binary_semaphore& _semaphore) :
        semaphore(_semaphore) {}

    ~OneShotSemaphoreRelease() {
        Release();
    }

    OneShotSemaphoreRelease(const OneShotSemaphoreRelease&) = delete;
    OneShotSemaphoreRelease& operator=(const OneShotSemaphoreRelease&) = delete;

    void Release() noexcept {
        if (!armed) {
            return;
        }
        armed = false;
        semaphore.release();
    }

private:
    std::binary_semaphore& semaphore;
    bool                   armed{true};
};

class HeadlessScriptedSwapchain final : public Swapchain {
public:
    HeadlessScriptedSwapchain() {
        format = PF_R8G8B8A8_UNORM;
        size   = Extent2D(4, 4);
    }

    [[nodiscard]] bool Recreate(const SwapchainCreateInfo& info) override {
        if (info.size.x == 0 || info.size.y == 0) {
            return false;
        }
        size = info.size;
        return IsPresentationReady();
    }
    void Sync() override {}
    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return size.x != 0 && size.y != 0;
    }
    [[nodiscard]] WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return {};
    }
};

class SourceSubmissionCapture final {
public:
    explicit SourceSubmissionCapture(uint64 _async_queue_scope) : async_queue_scope(_async_queue_scope) {}

    static void Observe(void* _context, const VulkanSourceSubmissionEvent& _event) noexcept {
        auto& capture = *static_cast<SourceSubmissionCapture*>(_context);
        if (_event.async_queue_scope != capture.async_queue_scope) {
            return;
        }

        const size_t index = capture.count.load(std::memory_order_relaxed);
        if (index >= capture.events.size()) {
            capture.overflow.store(true, std::memory_order_release);
            return;
        }
        capture.events[index] = _event;
        capture.count.store(index + 1, std::memory_order_release);
    }

    [[nodiscard]] size_t Count() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Overflowed() const noexcept {
        return overflow.load(std::memory_order_acquire);
    }

    [[nodiscard]] const VulkanSourceSubmissionEvent& Event(size_t _index) const noexcept {
        return events[_index];
    }

private:
    uint64                                     async_queue_scope{0};
    std::array<VulkanSourceSubmissionEvent, 16> events{};
    std::atomic<size_t>                        count{0};
    std::atomic<bool>                          overflow{false};
};

class ScopedSourceSubmissionObserver final {
public:
    explicit ScopedSourceSubmissionObserver(SourceSubmissionCapture& _capture) :
        observer{
            .context  = &_capture,
            .callback = &SourceSubmissionCapture::Observe,
        } {
        if (!TryInstallVulkanSourceSubmissionObserver(&observer)) {
            throw std::runtime_error("Vulkan source submission observer was already installed");
        }
        installed = true;
    }

    ~ScopedSourceSubmissionObserver() noexcept {
        if (installed && !RemoveVulkanSourceSubmissionObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedSourceSubmissionObserver(const ScopedSourceSubmissionObserver&)            = delete;
    ScopedSourceSubmissionObserver& operator=(const ScopedSourceSubmissionObserver&) = delete;

private:
    VulkanSourceSubmissionObserver observer{};
    bool                           installed{false};
};

class SourceTranslationCapture final {
public:
    explicit SourceTranslationCapture(
        uint64     _async_queue_scope,
        EQueueType _queue_filter = EQueueType::Ignore
    ) :
        async_queue_scope(_async_queue_scope),
        queue_filter(_queue_filter) {}

    static void Observe(
        void*                               _context,
        const VulkanSourceTranslationEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<SourceTranslationCapture*>(_context);
        if (_event.async_queue_scope != capture.async_queue_scope) {
            return;
        }
        if (capture.queue_filter != EQueueType::Ignore &&
            _event.queue != capture.queue_filter) {
            return;
        }

        const size_t phase =
            static_cast<size_t>(_event.phase);
        if (_event.source_index >= MaxSources ||
            phase >= PhaseCount) {
            capture.overflow.store(true, std::memory_order_release);
            return;
        }
        const size_t index =
            static_cast<size_t>(_event.source_index) *
                PhaseCount +
            phase;
        const bool duplicate =
            capture.claimed[index].exchange(
                true, std::memory_order_acq_rel
            );
        if (duplicate) {
            capture.duplicate.store(true, std::memory_order_release);
            return;
        }
        capture.events[index] = _event;
        capture.seen[index].store(
            true, std::memory_order_release
        );
        if (_event.thread_role != ERHIThreadRole::Translate) {
            capture.wrong_owner.store(true, std::memory_order_release);
        }
        if (_event.queue == EQueueType::Copy &&
            _event.phase ==
                EVulkanSourceTranslationPhase::Recorded &&
            !capture.copy_recorded_signalled.exchange(
                true, std::memory_order_acq_rel
            )) {
            capture.copy_recorded.release();
        }
    }

    [[nodiscard]] bool Seen(
        uint32                        _source_index,
        EVulkanSourceTranslationPhase _phase
    ) const noexcept {
        if (_source_index >= MaxSources) {
            return false;
        }
        const size_t index =
            static_cast<size_t>(_source_index) *
                PhaseCount +
            static_cast<size_t>(_phase);
        return seen[index].load(std::memory_order_acquire);
    }

    [[nodiscard]] const VulkanSourceTranslationEvent& Event(
        uint32                        _source_index,
        EVulkanSourceTranslationPhase _phase
    ) const noexcept {
        const size_t index =
            static_cast<size_t>(_source_index) *
                PhaseCount +
            static_cast<size_t>(_phase);
        return events[index];
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !overflow.load(std::memory_order_acquire) &&
               !duplicate.load(std::memory_order_acquire) &&
               !wrong_owner.load(std::memory_order_acquire);
    }

    std::binary_semaphore copy_recorded{0};

private:
    static constexpr size_t MaxSources = 16;
    static constexpr size_t PhaseCount = 3;

    uint64 async_queue_scope{0};
    EQueueType queue_filter{EQueueType::Ignore};
    std::array<
        VulkanSourceTranslationEvent,
        MaxSources * PhaseCount> events{};
    std::array<
        std::atomic<bool>,
        MaxSources * PhaseCount> claimed{};
    std::array<
        std::atomic<bool>,
        MaxSources * PhaseCount> seen{};
    std::atomic<bool> overflow{false};
    std::atomic<bool> duplicate{false};
    std::atomic<bool> wrong_owner{false};
    std::atomic<bool> copy_recorded_signalled{false};
};

class ScopedSourceTranslationObserver final {
public:
    explicit ScopedSourceTranslationObserver(
        SourceTranslationCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &SourceTranslationCapture::Observe,
        } {
        if (!TryInstallVulkanSourceTranslationObserver(
                &observer
            )) {
            throw std::runtime_error(
                "Vulkan source translation observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedSourceTranslationObserver() noexcept {
        if (installed &&
            !RemoveVulkanSourceTranslationObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedSourceTranslationObserver(
        const ScopedSourceTranslationObserver&
    ) = delete;
    ScopedSourceTranslationObserver& operator=(
        const ScopedSourceTranslationObserver&
    ) = delete;

private:
    VulkanSourceTranslationObserver observer{};
    bool                            installed{false};
};

class SerialQueryRecordFailureTranslationGate final {
public:
    explicit SerialQueryRecordFailureTranslationGate(
        uint64 _async_queue_scope
    ) :
        async_queue_scope(_async_queue_scope),
        observer{
            .context  = this,
            .callback =
                &SerialQueryRecordFailureTranslationGate::Observe,
        } {
        if (!TryInstallVulkanSourceTranslationObserver(
                &observer
            )) {
            throw std::runtime_error(
                "Vulkan source translation observer was already installed"
            );
        }
        installed = true;
    }

    ~SerialQueryRecordFailureTranslationGate() noexcept {
        ReleaseTerminalPhase();
        if (installed &&
            !RemoveVulkanSourceTranslationObserver(&observer)) {
            std::terminate();
        }
    }

    SerialQueryRecordFailureTranslationGate(
        const SerialQueryRecordFailureTranslationGate&
    ) = delete;
    SerialQueryRecordFailureTranslationGate& operator=(
        const SerialQueryRecordFailureTranslationGate&
    ) = delete;

    void NotifyFailureCallbackEntryChecked() noexcept {
        const uint32 entry =
            callback_entries_checked.fetch_add(
                1, std::memory_order_acq_rel
            ) +
            1;
        if (entry == 1) {
            early_failure_callback_entered.release();
        }
    }

    [[nodiscard]] bool WaitForTerminalPhase(
        std::chrono::milliseconds _timeout
    ) noexcept {
        return terminal_phase_entered.try_acquire_for(_timeout);
    }

    [[nodiscard]] bool FailureCallbackEnteredBeforeRelease(
        std::chrono::milliseconds _timeout
    ) noexcept {
        return early_failure_callback_entered.try_acquire_for(
            _timeout
        );
    }

    void ReleaseTerminalPhase() noexcept {
        if (!terminal_phase_release_sent.exchange(
                true, std::memory_order_acq_rel
            )) {
            terminal_phase_release.release();
        }
    }

    [[nodiscard]] uint32 FailurePhaseCount() const noexcept {
        return failure_phase_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32 RecordedPhaseCount() const noexcept {
        return recorded_phase_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32 CheckedCallbackEntryCount() const noexcept {
        return callback_entries_checked.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool TimedOut() const noexcept {
        return timed_out.load(std::memory_order_acquire);
    }

private:
    static void Observe(
        void*                               _context,
        const VulkanSourceTranslationEvent& _event
    ) noexcept {
        using namespace std::chrono_literals;

        auto& gate =
            *static_cast<
                SerialQueryRecordFailureTranslationGate*>(_context);
        if (_event.async_queue_scope != gate.async_queue_scope ||
            _event.source_index != 0) {
            return;
        }
        if (_event.phase ==
            EVulkanSourceTranslationPhase::Failed) {
            gate.failure_phase_count.fetch_add(
                1, std::memory_order_acq_rel
            );
        } else if (
            _event.phase ==
            EVulkanSourceTranslationPhase::Recorded) {
            gate.recorded_phase_count.fetch_add(
                1, std::memory_order_acq_rel
            );
        } else {
            return;
        }

        // TranslateForRuntime has returned, but Submission has not yet
        // consumed that result. The terminal-packet path therefore cannot
        // have enqueued Completion yet. The regressed path enqueued the
        // current source directly before reporting Failed; while this gate is
        // held, its callback can deterministically expose a still-Pending
        // sibling.
        gate.terminal_phase_entered.release();
        if (!gate.terminal_phase_release.try_acquire_for(2s)) {
            gate.timed_out.store(true, std::memory_order_release);
        }
    }

    uint64 async_queue_scope{0};
    VulkanSourceTranslationObserver observer{};
    std::binary_semaphore           terminal_phase_entered{0};
    std::binary_semaphore           terminal_phase_release{0};
    std::binary_semaphore           early_failure_callback_entered{0};
    std::atomic<uint32>             failure_phase_count{0};
    std::atomic<uint32>             recorded_phase_count{0};
    std::atomic<uint32>             callback_entries_checked{0};
    std::atomic<bool>               timed_out{false};
    std::atomic<bool>               terminal_phase_release_sent{false};
    bool                            installed{false};
};

class NativeSubmissionCapture final {
public:
    static void Observe(
        void*                              _context,
        const VulkanNativeSubmissionEvent& _event
    ) noexcept {
        auto& capture = *static_cast<NativeSubmissionCapture*>(_context);
        const size_t index =
            capture.count.fetch_add(1, std::memory_order_acq_rel);
        if (index >= capture.events.size()) {
            capture.overflow.store(true, std::memory_order_release);
            return;
        }
        capture.events[index] = _event;
    }

    [[nodiscard]] size_t Count() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Overflowed() const noexcept {
        return overflow.load(std::memory_order_acquire);
    }

    [[nodiscard]] const VulkanNativeSubmissionEvent& Event(
        size_t _index
    ) const noexcept {
        return events[_index];
    }

private:
    std::array<VulkanNativeSubmissionEvent, 16> events{};
    std::atomic<size_t>                         count{0};
    std::atomic<bool>                           overflow{false};
};

class ScopedNativeSubmissionObserver final {
public:
    explicit ScopedNativeSubmissionObserver(
        NativeSubmissionCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &NativeSubmissionCapture::Observe,
        } {
        if (!TryInstallVulkanNativeSubmissionObserver(&observer)) {
            throw std::runtime_error(
                "Vulkan native submission observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedNativeSubmissionObserver() noexcept {
        if (installed &&
            !RemoveVulkanNativeSubmissionObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedNativeSubmissionObserver(
        const ScopedNativeSubmissionObserver&
    ) = delete;
    ScopedNativeSubmissionObserver& operator=(
        const ScopedNativeSubmissionObserver&
    ) = delete;

private:
    VulkanNativeSubmissionObserver observer{};
    bool                           installed{false};
};

class SubmissionBoundaryCapture final {
public:
    static void Observe(
        void*                              _context,
        const VulkanSubmissionBoundaryEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<SubmissionBoundaryCapture*>(_context);
        const size_t index =
            capture.count.load(std::memory_order_relaxed);
        if (index >= capture.events.size()) {
            capture.overflow.store(true, std::memory_order_release);
            return;
        }
        capture.events[index] = _event;
        capture.count.store(index + 1, std::memory_order_release);
        if (_event.thread_role != ERHIThreadRole::Submission) {
            capture.wrong_owner.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] size_t Count() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    [[nodiscard]] const VulkanSubmissionBoundaryEvent& Event(
        size_t _index
    ) const noexcept {
        return events[_index];
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !overflow.load(std::memory_order_acquire) &&
               !wrong_owner.load(std::memory_order_acquire);
    }

private:
    std::array<VulkanSubmissionBoundaryEvent, 32> events{};
    std::atomic<size_t>                         count{0};
    std::atomic<bool>                           overflow{false};
    std::atomic<bool>                           wrong_owner{false};
};

class ScopedSubmissionBoundaryObserver final {
public:
    explicit ScopedSubmissionBoundaryObserver(
        SubmissionBoundaryCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &SubmissionBoundaryCapture::Observe,
        } {
        if (!TryInstallVulkanSubmissionBoundaryObserver(&observer)) {
            throw std::runtime_error(
                "Vulkan submission-boundary observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedSubmissionBoundaryObserver() noexcept {
        if (installed &&
            !RemoveVulkanSubmissionBoundaryObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedSubmissionBoundaryObserver(
        const ScopedSubmissionBoundaryObserver&
    ) = delete;
    ScopedSubmissionBoundaryObserver& operator=(
        const ScopedSubmissionBoundaryObserver&
    ) = delete;

private:
    VulkanSubmissionBoundaryObserver observer{};
    bool                             installed{false};
};

class ScriptedPresentCapture final {
public:
    explicit ScriptedPresentCapture(
        VulkanScriptedPresentResult _result
    ) :
        result(_result) {}

    static VulkanScriptedPresentResult Observe(
        void*  _context,
        uint64 _timeline
    ) noexcept {
        auto& capture =
            *static_cast<ScriptedPresentCapture*>(_context);
        capture.timeline.store(_timeline, std::memory_order_relaxed);
        capture.thread_id.store(
            Platform::GetCurrentThreadID(), std::memory_order_relaxed
        );
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Submission) {
            capture.wrong_owner.store(true, std::memory_order_relaxed);
        }
        capture.count.fetch_add(1, std::memory_order_acq_rel);
        return capture.result;
    }

    static void LatchDeviceFaultBeforeSourceRejection(
        void* _context
    ) noexcept {
        auto& capture =
            *static_cast<ScriptedPresentCapture*>(_context);
        capture.source_rejection_hook_count.fetch_add(
            1, std::memory_order_acq_rel
        );
        if (!TryLatchVulkanDeviceFaultForTesting(
                VK_ERROR_DEVICE_LOST
            )) {
            capture.source_rejection_hook_failed.store(
                true, std::memory_order_release
            );
        }
    }

    VulkanScriptedPresentResult result{};
    std::atomic<uint32>         count{0};
    std::atomic<uint64>         timeline{0};
    std::atomic<uint32>         thread_id{0};
    std::atomic<bool>           wrong_owner{false};
    std::atomic<uint32>         source_rejection_hook_count{0};
    std::atomic<bool>           source_rejection_hook_failed{false};
};

class ScopedScriptedPresentOverride final {
public:
    explicit ScopedScriptedPresentOverride(
        ScriptedPresentCapture& _capture,
        bool _require_present_source_ready = false,
        bool _latch_fault_before_source_rejection = false
    ) :
        scripted_override{
            .context  = &_capture,
            .callback = &ScriptedPresentCapture::Observe,
            .require_present_source_ready =
                _require_present_source_ready,
            .before_source_rejection =
                _latch_fault_before_source_rejection ?
                    &ScriptedPresentCapture::
                        LatchDeviceFaultBeforeSourceRejection :
                    nullptr,
        } {
        if (!TryInstallVulkanScriptedPresentOverrideForTesting(
                &scripted_override
            )) {
            throw std::runtime_error(
                "Vulkan scripted Present override was already installed"
            );
        }
        installed = true;
    }

    ~ScopedScriptedPresentOverride() noexcept {
        if (installed &&
            !RemoveVulkanScriptedPresentOverrideForTesting(
                &scripted_override
            )) {
            std::terminate();
        }
    }

    ScopedScriptedPresentOverride(
        const ScopedScriptedPresentOverride&
    ) = delete;
    ScopedScriptedPresentOverride& operator=(
        const ScopedScriptedPresentOverride&
    ) = delete;

private:
    VulkanScriptedPresentOverrideForTesting scripted_override{};
    bool                                    installed{false};
};

class ScriptedQueryPreparationCapture final {
public:
    static VkResult Observe(
        void*      _context,
        EQueueType _queue,
        uint64     _timeline,
        uint32     _query_count
    ) noexcept {
        auto& capture =
            *static_cast<ScriptedQueryPreparationCapture*>(_context);
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Translate) {
            capture.wrong_owner.store(true, std::memory_order_relaxed);
        }
        const uint32 invocation =
            capture.count.fetch_add(1, std::memory_order_acq_rel);
        if (invocation != 0) {
            return VK_SUCCESS;
        }
        capture.queue.store(
            static_cast<uint32>(_queue), std::memory_order_relaxed
        );
        capture.timeline.store(_timeline, std::memory_order_relaxed);
        capture.query_count.store(_query_count, std::memory_order_relaxed);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    std::atomic<uint32> count{0};
    std::atomic<uint32> queue{
        static_cast<uint32>(EQueueType::Ignore)
    };
    std::atomic<uint64> timeline{0};
    std::atomic<uint32> query_count{0};
    std::atomic<bool>   wrong_owner{false};
};

class ScopedScriptedQueryPreparationOverride final {
public:
    explicit ScopedScriptedQueryPreparationOverride(
        ScriptedQueryPreparationCapture& _capture
    ) :
        scripted_override{
            .context  = &_capture,
            .callback = &ScriptedQueryPreparationCapture::Observe,
        } {
        if (!TryInstallVulkanScriptedQueryPreparationOverrideForTesting(
                &scripted_override
            )) {
            throw std::runtime_error(
                "Vulkan scripted Query preparation override was already installed"
            );
        }
        installed = true;
    }

    ~ScopedScriptedQueryPreparationOverride() noexcept {
        if (installed &&
            !RemoveVulkanScriptedQueryPreparationOverrideForTesting(
                &scripted_override
            )) {
            std::terminate();
        }
    }

    ScopedScriptedQueryPreparationOverride(
        const ScopedScriptedQueryPreparationOverride&
    ) = delete;
    ScopedScriptedQueryPreparationOverride& operator=(
        const ScopedScriptedQueryPreparationOverride&
    ) = delete;

private:
    VulkanScriptedQueryPreparationOverrideForTesting scripted_override{};
    bool                                             installed{false};
};

class ScriptedTimestampValidBitsCapture final {
public:
    static uint32 ForceUnsupported(
        void*      _context,
        EQueueType _queue,
        uint32     _native_valid_bits
    ) noexcept {
        auto& capture =
            *static_cast<ScriptedTimestampValidBitsCapture*>(_context);
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Translate) {
            capture.wrong_owner.store(true, std::memory_order_relaxed);
        }
        capture.queue.store(
            static_cast<uint32>(_queue), std::memory_order_relaxed
        );
        capture.native_valid_bits.store(
            _native_valid_bits, std::memory_order_relaxed
        );
        capture.count.fetch_add(1, std::memory_order_release);
        return 0;
    }

    std::atomic<uint32> count{0};
    std::atomic<uint32> queue{
        static_cast<uint32>(EQueueType::Ignore)
    };
    std::atomic<uint32> native_valid_bits{0};
    std::atomic<bool>   wrong_owner{false};
};

class ScopedScriptedTimestampValidBitsOverride final {
public:
    explicit ScopedScriptedTimestampValidBitsOverride(
        ScriptedTimestampValidBitsCapture& _capture
    ) :
        scripted_override{
            .context  = &_capture,
            .callback =
                &ScriptedTimestampValidBitsCapture::ForceUnsupported,
        } {
        if (!TryInstallVulkanScriptedTimestampValidBitsOverrideForTesting(
                &scripted_override
            )) {
            throw std::runtime_error(
                "Vulkan scripted timestamp valid-bits override was already installed"
            );
        }
        installed = true;
    }

    ~ScopedScriptedTimestampValidBitsOverride() noexcept {
        if (installed &&
            !RemoveVulkanScriptedTimestampValidBitsOverrideForTesting(
                &scripted_override
            )) {
            std::terminate();
        }
    }

    ScopedScriptedTimestampValidBitsOverride(
        const ScopedScriptedTimestampValidBitsOverride&
    ) = delete;
    ScopedScriptedTimestampValidBitsOverride& operator=(
        const ScopedScriptedTimestampValidBitsOverride&
    ) = delete;

private:
    VulkanScriptedTimestampValidBitsOverrideForTesting
        scripted_override{};
    bool installed{false};
};

class DependencyWaitCapture final {
public:
    explicit DependencyWaitCapture(EQueueType _queue) :
        queue(_queue) {}

    static void Observe(
        void*                                        _context,
        const VulkanSubmissionDependencyWaitEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<DependencyWaitCapture*>(_context);
        if (_event.queue != capture.queue) {
            return;
        }
        if (_event.thread_role != ERHIThreadRole::Submission) {
            capture.wrong_owner.store(true, std::memory_order_release);
        }
        if (capture.count.fetch_add(1, std::memory_order_acq_rel) == 0) {
            capture.entered.release();
        }
    }

    std::binary_semaphore entered{0};
    std::atomic<uint32>   count{0};
    std::atomic<bool>     wrong_owner{false};

private:
    EQueueType queue{EQueueType::Ignore};
};

class ScopedDependencyWaitObserver final {
public:
    explicit ScopedDependencyWaitObserver(
        DependencyWaitCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &DependencyWaitCapture::Observe,
        } {
        if (!TryInstallVulkanSubmissionDependencyWaitObserver(
                &observer
            )) {
            throw std::runtime_error(
                "Vulkan dependency-wait observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedDependencyWaitObserver() noexcept {
        if (installed &&
            !RemoveVulkanSubmissionDependencyWaitObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedDependencyWaitObserver(
        const ScopedDependencyWaitObserver&
    ) = delete;
    ScopedDependencyWaitObserver& operator=(
        const ScopedDependencyWaitObserver&
    ) = delete;

private:
    VulkanSubmissionDependencyWaitObserver observer{};
    bool                                   installed{false};
};

class BackendSyncWaitCapture final {
public:
    static void Observe(
        void*                             _context,
        const VulkanBackendSyncWaitEvent&
    ) noexcept {
        auto& capture = *static_cast<BackendSyncWaitCapture*>(_context);
        if (capture.count.fetch_add(1, std::memory_order_acq_rel) == 0) {
            capture.entered.release();
        }
    }

    std::binary_semaphore entered{0};
    std::atomic<uint32>   count{0};
};

class ScopedBackendSyncWaitObserver final {
public:
    explicit ScopedBackendSyncWaitObserver(
        BackendSyncWaitCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &BackendSyncWaitCapture::Observe,
        } {
        if (!TryInstallVulkanBackendSyncWaitObserver(&observer)) {
            throw std::runtime_error(
                "Vulkan backend Sync-wait observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedBackendSyncWaitObserver() noexcept {
        if (installed &&
            !RemoveVulkanBackendSyncWaitObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedBackendSyncWaitObserver(
        const ScopedBackendSyncWaitObserver&
    ) = delete;
    ScopedBackendSyncWaitObserver& operator=(
        const ScopedBackendSyncWaitObserver&
    ) = delete;

private:
    VulkanBackendSyncWaitObserver observer{};
    bool                          installed{false};
};

class QueueLocalSyncWaitCapture final {
public:
    static void Observe(
        void*                                    _context,
        const VulkanQueueLocalSyncWaitEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<QueueLocalSyncWaitCapture*>(_context);
        const uint32 queue_bit =
            _event.queue == EQueueType::Graphics ? 1u :
            _event.queue == EQueueType::Compute ? 2u :
            _event.queue == EQueueType::Copy ? 4u :
            0u;
        if (queue_bit == 0 ||
            _event.thread_role == ERHIThreadRole::Completion ||
            _event.target_retirement_serial == 0 ||
            _event.completion_group_count == 0) {
            capture.invalid.store(true, std::memory_order_release);
        }
        capture.queue_mask.fetch_or(
            queue_bit, std::memory_order_acq_rel
        );
        capture.count.fetch_add(1, std::memory_order_acq_rel);
        capture.entered.release();
    }

    [[nodiscard]] bool WaitForTwo(
        std::chrono::milliseconds _timeout
    ) noexcept {
        return entered.try_acquire_for(_timeout) &&
               entered.try_acquire_for(_timeout);
    }

    [[nodiscard]] bool ValidGraphicsComputePair() const noexcept {
        return !invalid.load(std::memory_order_acquire) &&
               count.load(std::memory_order_acquire) == 2 &&
               queue_mask.load(std::memory_order_acquire) == 3u;
    }

private:
    std::counting_semaphore<2> entered{0};
    std::atomic<uint32>        count{0};
    std::atomic<uint32>        queue_mask{0};
    std::atomic<bool>          invalid{false};
};

class ScopedQueueLocalSyncWaitObserver final {
public:
    explicit ScopedQueueLocalSyncWaitObserver(
        QueueLocalSyncWaitCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &QueueLocalSyncWaitCapture::Observe,
        } {
        if (!TryInstallVulkanQueueLocalSyncWaitObserver(
                &observer
            )) {
            throw std::runtime_error(
                "Vulkan queue-local Sync observer was already installed"
            );
        }
        installed = true;
    }

    ~ScopedQueueLocalSyncWaitObserver() noexcept {
        if (installed &&
            !RemoveVulkanQueueLocalSyncWaitObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedQueueLocalSyncWaitObserver(
        const ScopedQueueLocalSyncWaitObserver&
    ) = delete;
    ScopedQueueLocalSyncWaitObserver& operator=(
        const ScopedQueueLocalSyncWaitObserver&
    ) = delete;

private:
    VulkanQueueLocalSyncWaitObserver observer{};
    bool                             installed{false};
};

class BatchPreflightRejectionCapture final {
public:
    static void Observe(
        void*                                      _context,
        const VulkanBatchPreflightRejectionEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<BatchPreflightRejectionCapture*>(_context);
        capture.batch_sequence.store(
            _event.batch_sequence, std::memory_order_relaxed
        );
        capture.thread_id.store(
            _event.thread_id, std::memory_order_relaxed
        );
        if (_event.thread_role != ERHIThreadRole::Translate) {
            capture.wrong_owner.store(true, std::memory_order_relaxed);
        }
        if (_event.executable_preflight) {
            capture.executable_preflight.store(
                true, std::memory_order_relaxed
            );
        }
        if (capture.count.fetch_add(1, std::memory_order_acq_rel) == 0) {
            capture.entered.release();
        }
    }

    std::binary_semaphore entered{0};
    std::atomic<uint32>   count{0};
    std::atomic<uint32>   thread_id{0};
    std::atomic<uint64>   batch_sequence{0};
    std::atomic<bool>     wrong_owner{false};
    std::atomic<bool>     executable_preflight{false};
};

class ScopedBatchPreflightRejectionObserver final {
public:
    explicit ScopedBatchPreflightRejectionObserver(
        BatchPreflightRejectionCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &BatchPreflightRejectionCapture::Observe,
        } {
        if (!TryInstallVulkanBatchPreflightRejectionObserver(&observer)) {
            throw std::runtime_error(
                "Vulkan batch preflight-rejection observer was already "
                "installed"
            );
        }
        installed = true;
    }

    ~ScopedBatchPreflightRejectionObserver() noexcept {
        if (installed &&
            !RemoveVulkanBatchPreflightRejectionObserver(&observer)) {
            std::terminate();
        }
    }

    ScopedBatchPreflightRejectionObserver(
        const ScopedBatchPreflightRejectionObserver&
    ) = delete;
    ScopedBatchPreflightRejectionObserver& operator=(
        const ScopedBatchPreflightRejectionObserver&
    ) = delete;

private:
    VulkanBatchPreflightRejectionObserver observer{};
    bool                                  installed{false};
};

template<size_t N>
Array<Moer::byte> OwnedBytes(const std::array<uint32, N>& _values) {
    Array<Moer::byte> bytes(sizeof(uint32) * N);
    std::memcpy(bytes.data(), _values.data(), bytes.size());
    return bytes;
}

template<size_t N>
std::span<Moer::byte> WritableBytes(std::array<uint32, N>& _values) {
    return {
        reinterpret_cast<Moer::byte*>(_values.data()),
        sizeof(uint32) * N,
    };
}

bool HasArgument(int _argc, const char** _argv, std::string_view _argument) {
    for (int index = 1; index < _argc; ++index) {
        if (std::string_view(_argv[index]) == _argument) {
            return true;
        }
    }
    return false;
}

void ValidateArguments(int _argc, const char** _argv) {
    for (int index = 1; index < _argc; ++index) {
        const std::string_view argument = _argv[index];
        if (argument != "--parallel" && argument != "--inject-worker-failure" &&
            argument != "--production-gate" && argument != "--production-heavy" &&
            argument != "--inject-translate-failure" &&
            argument != "--inject-multi-segment-translate-failure" &&
            argument != "--pipeline-window1" &&
            argument != "--pipeline-window2" &&
            argument != "--present-boundary" &&
            argument != "--present-hard" &&
            argument != "--rt-export-rejection" &&
            argument != "--readback-future" &&
            argument != "--occlusion-query" &&
            argument != "--timestamp-query" &&
            argument != "--timestamp-query-success-batch" &&
            argument != "--timestamp-query-mid-failure" &&
            argument != "--timestamp-query-record-failure" &&
            argument != "--gpu-scope-stream") {
            throw std::invalid_argument("unsupported argument: " + std::string(argument));
        }
    }
    if (HasArgument(_argc, _argv, "--pipeline-window1") &&
        HasArgument(_argc, _argv, "--pipeline-window2")) {
        throw std::invalid_argument(
            "--pipeline-window1 and --pipeline-window2 are mutually exclusive"
        );
    }
    if (HasArgument(_argc, _argv, "--present-boundary") &&
        HasArgument(_argc, _argv, "--present-hard")) {
        throw std::invalid_argument(
            "--present-boundary and --present-hard are mutually exclusive"
        );
    }
    if (HasArgument(_argc, _argv, "--inject-worker-failure") &&
        !HasArgument(_argc, _argv, "--parallel")) {
        throw std::invalid_argument("--inject-worker-failure requires --parallel");
    }
    if (HasArgument(_argc, _argv, "--production-gate") &&
        !HasArgument(_argc, _argv, "--parallel")) {
        throw std::invalid_argument("--production-gate requires --parallel");
    }
    if (HasArgument(_argc, _argv, "--production-heavy") &&
        !HasArgument(_argc, _argv, "--parallel")) {
        throw std::invalid_argument("--production-heavy requires --parallel");
    }
    if (HasArgument(_argc, _argv, "--inject-translate-failure") &&
        HasArgument(_argc, _argv, "--inject-worker-failure")) {
        throw std::invalid_argument("--inject-translate-failure cannot be combined with "
                                    "--inject-worker-failure");
    }
    if (HasArgument(_argc, _argv, "--inject-multi-segment-translate-failure") &&
        (HasArgument(_argc, _argv, "--inject-worker-failure") ||
         HasArgument(_argc, _argv, "--inject-translate-failure"))) {
        throw std::invalid_argument("--inject-multi-segment-translate-failure cannot be combined with "
                                    "another fault injection");
    }
}

void RunOrderedReadback(
    bool _parallel,
    bool _inject_worker_failure,
    bool _production_gate,
    bool _production_heavy
) {
    auto& device = RenderDevice::Get();
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;

    std::array<BufferRef, 4> sources{
        device.CreateBuffer<uint32>("parallel_order_src_a", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_src_b", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_src_c", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_src_d", kElementCount, usage),
    };
    BufferRef destination =
        device.CreateBuffer<uint32>("parallel_order_destination", kElementCount, usage);
    std::array<BufferRef, 4> checkpoints{
        device.CreateBuffer<uint32>("parallel_order_checkpoint_a", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_checkpoint_b", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_checkpoint_c", kElementCount, usage),
        device.CreateBuffer<uint32>("parallel_order_checkpoint_d", kElementCount, usage),
    };
    Array<BufferRef> heavy_checkpoints;
    if (_production_heavy) {
        heavy_checkpoints.reserve(kHeavyCopyCount);
        for (uint32 index = 0; index < kHeavyCopyCount; ++index) {
            heavy_checkpoints.push_back(device.CreateBuffer<uint32>(
                "parallel_heavy_checkpoint_" + std::to_string(index), kElementCount, usage
            ));
        }
    }

    for (uint32 iteration = 0; iteration < kIterations; ++iteration) {
        std::array<std::array<uint32, kElementCount>, 4> values{};
        for (uint32 source = 0; source < values.size(); ++source) {
            for (uint32 element = 0; element < kElementCount; ++element) {
                values[source][element] =
                    0x10000000u * (source + 1u) + iteration * 4096u + element * 17u + 3u;
            }
        }
        std::array<std::array<uint32, kElementCount>, 4> readbacks{};
        Array<std::array<uint32, kElementCount>> heavy_readbacks(
            _production_heavy ? kHeavyCopyCount : 0
        );

        CommandList commands;
        for (uint32 source = 0; source < sources.size(); ++source) {
            commands.CopyFrom(
                OwnedBytes(values[source]),
                sources[source]->GetView(),
                "ParallelOrderUpload"
            );
        }
        if (_production_heavy) {
            for (uint32 index = 0; index < kHeavyCopiesPerWave; ++index) {
                commands.CopyFrom(
                    sources[0]->GetView(),
                    heavy_checkpoints[index]->GetView(),
                    "ParallelHeavyWave0"
                );
            }
        }

        // Two dependent safe-copy layers form the first worker wave. Their
        // completion order is irrelevant; GPU execution must retain A then B.
        commands.CopyFrom(
            sources[0]->GetView(), destination->GetView(), "ParallelOrderWave0A"
        );
        commands.CopyFrom(
            destination->GetView(), checkpoints[0]->GetView(), "ParallelOrderCheckpointA"
        );
        commands.CopyFrom(
            sources[1]->GetView(), destination->GetView(), "ParallelOrderWave0B"
        );
        commands.CopyFrom(
            destination->GetView(), checkpoints[1]->GetView(), "ParallelOrderCheckpointB"
        );

        // A coordinator-only scope is an explicit serial island. The second
        // worker wave may not start translating before the first wave joins.
        commands.PushScope("ParallelOrderSerialIsland", {0.8f, 0.3f, 0.1f, 1.0f});
        if (_production_heavy) {
            for (uint32 index = kHeavyCopiesPerWave; index < kHeavyCopyCount; ++index) {
                commands.CopyFrom(
                    sources[2]->GetView(),
                    heavy_checkpoints[index]->GetView(),
                    "ParallelHeavyWave1"
                );
            }
        }
        commands.CopyFrom(
            sources[2]->GetView(), destination->GetView(), "ParallelOrderWave1C"
        );
        commands.CopyFrom(
            destination->GetView(), checkpoints[2]->GetView(), "ParallelOrderCheckpointC"
        );
        commands.CopyFrom(
            sources[3]->GetView(), destination->GetView(), "ParallelOrderWave1D"
        );
        commands.CopyFrom(
            destination->GetView(), checkpoints[3]->GetView(), "ParallelOrderCheckpointD"
        );
        commands.PopScope();

        for (uint32 checkpoint = 0; checkpoint < checkpoints.size(); ++checkpoint) {
            commands.CopyFrom(
                checkpoints[checkpoint]->GetView(),
                WritableBytes(readbacks[checkpoint]),
                "ParallelOrderReadback"
            );
        }
        if (_production_heavy) {
            for (uint32 index = 0; index < kHeavyCopyCount; ++index) {
                commands.CopyFrom(
                    heavy_checkpoints[index]->GetView(),
                    WritableBytes(heavy_readbacks[index]),
                    "ParallelHeavyReadback"
                );
            }
        }

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            commands.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        for (size_t checkpoint = 0; checkpoint < readbacks.size(); ++checkpoint) {
            if (readbacks[checkpoint] == values[checkpoint]) {
                continue;
            }
            for (size_t index = 0; index < readbacks[checkpoint].size(); ++index) {
                if (readbacks[checkpoint][index] != values[checkpoint][index]) {
                    throw std::runtime_error(
                        "ordered GPU checkpoint mismatch at iteration=" +
                        std::to_string(iteration) + " checkpoint=" +
                        std::to_string(checkpoint) + " index=" + std::to_string(index) +
                        " expected=" + std::to_string(values[checkpoint][index]) +
                        " actual=" + std::to_string(readbacks[checkpoint][index])
                    );
                }
            }
            throw std::runtime_error("ordered GPU checkpoint mismatch");
        }
        for (size_t checkpoint = 0; checkpoint < heavy_readbacks.size(); ++checkpoint) {
            const size_t source = checkpoint < kHeavyCopiesPerWave ? 0 : 2;
            if (heavy_readbacks[checkpoint] != values[source]) {
                throw std::runtime_error(
                    "heavy weighted checkpoint mismatch at iteration=" +
                    std::to_string(iteration) + " checkpoint=" + std::to_string(checkpoint)
                );
            }
        }
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=ParallelRecordOrderedReadback mode={} worker_fault={} "
        "production_gate={} production_heavy={} iterations={}",
        _parallel ? "parallel" : "serial",
        _inject_worker_failure,
        _production_gate,
        _production_heavy,
        kIterations
    );
}

void RunActiveRdgExplicitBarrierReadback(bool _parallel) {
    auto& device = RenderDevice::Get();
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    constexpr uint64_t byte_size = sizeof(uint32) * kElementCount;

    BufferRef source =
        device.CreateBuffer<uint32>("active_rdg_source", kElementCount, usage);
    BufferRef destination =
        device.CreateBuffer<uint32>("active_rdg_destination", kElementCount, usage);

    std::array<uint32, kElementCount> values{};
    std::array<uint32, kElementCount> readback{};
    for (uint32 index = 0; index < values.size(); ++index) {
        values[index] = 0xA1100000u + index * 37u + 11u;
    }

    RenderGraph graph("ActiveRdgExplicitBarrier");
    const auto source_handle = graph.ImportBuffer(
        "Source",
        source,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    const auto destination_handle = graph.ImportBuffer(
        "Destination",
        destination,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    graph.SetInitialState(
        source_handle,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );
    graph.SetInitialState(
        destination_handle,
        RenderGraph::BufferState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None
    );

    graph.AddRecordPass(
        "Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    source_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [source, values](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(values), source->GetView(), "ActiveRdgUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "Copy",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(source_handle, RenderGraph::BufferState::TransferSource)
                .Write(
                    destination_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [source, destination](CommandList& commands) {
            commands.CopyFrom(
                source->GetView(),
                destination->GetView(),
                "ActiveRdgBufferCopy"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "Readback",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    destination_handle,
                    RenderGraph::BufferState::TransferSource
                )
                .SideEffect();
        },
        [destination, &readback](CommandList& commands) {
            commands.CopyFrom(
                destination->GetView(),
                WritableBytes(readback),
                "ActiveRdgReadback"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.Export(
        source_handle,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        destination_handle,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "active RDG compile failed: " + graph.GetCompileError()
        );
    }
    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        )) {
        throw std::runtime_error(
            "active RDG execution failed: " + graph.GetCompileError()
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (readback != values) {
        throw std::runtime_error(
            "active RDG explicit barrier readback mismatch"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgExplicitBarrier mode={} "
        "passes=3 state_owner=rdg readback=verified",
        _parallel ? "parallel" : "serial"
    );
}

void RunActiveRdgAsyncQueueDag(bool _parallel) {
    auto& device = RenderDevice::Get();
    const auto topology = RenderGraph::QueueTopology::FromRHI();
    const bool dedicated_compute =
        topology.graphics.native_queue_id != topology.compute.native_queue_id;
    const bool same_queue_family =
        topology.graphics.family_id == topology.compute.family_id;
    const bool graphics_compute_available =
        topology.graphics.available && topology.compute.available;
    if (dedicated_compute) {
        RenderGraph stale_topology_graph("ActiveRdgRejectStaleTopology");
        const auto graphics_done =
            stale_topology_graph.CreateTransientToken("GraphicsDone");
        stale_topology_graph.AddRecordPass(
            "Graphics",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(graphics_done)
                    .SideEffect();
            },
            [](CommandList&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        stale_topology_graph.AddRecordPass(
            "Compute",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Copy
                       )
                    .Read(graphics_done)
                    .SideEffect();
            },
            [](CommandList&) {},
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        if (!stale_topology_graph.Compile()) {
            throw std::runtime_error(
                "stale-topology rejection graph failed to compile: " +
                stale_topology_graph.GetCompileError()
            );
        }
        if (stale_topology_graph.ExecuteRecording(
                {},
                {},
                _parallel,
                {},
                RenderGraph::ActiveRecordingOptions{.enabled = true}
            )) {
            throw std::runtime_error(
                "active RDG accepted a SingleQueue plan on dedicated queues"
            );
        }
        if (stale_topology_graph.GetCompileError().find(
                "compiled queue topology does not match"
            ) == std::string::npos) {
            throw std::runtime_error(
                "active RDG stale-topology rejection was not diagnostic"
            );
        }
        LOG_INFO(
            "[TESTCASE][PASS] name=ActiveRdgRejectStaleTopology "
            "compiled=single runtime=dedicated execution=rejected"
        );
    }

    std::atomic<uint32> caller_callbacks{0};
    RenderGraph caller_graph(
        "ActiveRdgRejectCallerThreadMultiQueue", topology
    );
    const auto caller_graphics_done =
        caller_graph.CreateTransientToken("GraphicsDone");
    caller_graph.AddPass(
            "Graphics",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(caller_graphics_done)
                    .SideEffect();
            },
            [&] {
                caller_callbacks.fetch_add(1, std::memory_order_relaxed);
            }
    );
    caller_graph.AddPass(
            "Compute",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Copy
                       )
                    .Read(caller_graphics_done)
                    .SideEffect();
            },
            [&] {
                caller_callbacks.fetch_add(1, std::memory_order_relaxed);
            }
    );
    if (!caller_graph.Compile()) {
        throw std::runtime_error(
            "caller-thread multi-queue rejection graph failed to compile: " +
            caller_graph.GetCompileError()
        );
    }
    CommandList caller_commands(EQueueType::Graphics);
    if (caller_graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled = true,
                .main_thread_command_list = &caller_commands,
            }
        )) {
        throw std::runtime_error(
            "active RDG accepted caller-thread multi-queue endpoints"
        );
    }
    if (caller_callbacks.load(std::memory_order_relaxed) != 0 ||
        !caller_commands.IsEmpty() ||
        caller_graph.GetCompileError().find(
            "managed recording handoff"
        ) == std::string::npos) {
        throw std::runtime_error(
            "caller-thread multi-queue rejection was not immutable and diagnostic"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgRejectCallerThreadMultiQueue "
        "callbacks=0 command_stream=empty execution=rejected"
    );

    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    constexpr uint64_t byte_size = sizeof(uint32) * kElementCount;

    struct QueueBuffer {
        BufferRef buffer{};
        std::array<uint32, kElementCount> values{};
        std::array<uint32, kElementCount> readback{};
    };
    QueueBuffer graphics_root{
        .buffer = device.CreateBuffer<uint32>(
            "active_rdg_async_graphics_root", kElementCount, usage
        ),
    };
    QueueBuffer compute_independent{
        .buffer = device.CreateBuffer<uint32>(
            "active_rdg_async_compute_independent", kElementCount, usage
        ),
    };
    QueueBuffer graphics_independent{
        .buffer = device.CreateBuffer<uint32>(
            "active_rdg_async_graphics_independent", kElementCount, usage
        ),
    };
    QueueBuffer compute_dependent{
        .buffer = device.CreateBuffer<uint32>(
            "active_rdg_async_compute_dependent", kElementCount, usage
        ),
    };
    std::array<QueueBuffer*, 4> buffers{
        &graphics_root,
        &compute_independent,
        &graphics_independent,
        &compute_dependent,
    };
    for (uint32 buffer_index = 0; buffer_index < buffers.size(); ++buffer_index) {
        for (uint32 element = 0; element < kElementCount; ++element) {
            buffers[buffer_index]->values[element] =
                0xB1000000u + buffer_index * 0x01000000u + element * 41u + 13u;
        }
    }

    {
        std::binary_semaphore graphics_record_started{0};
        std::binary_semaphore compute_record_started{0};
        RenderGraph independent_graph(
            "ActiveRdgIndependentQueueRoots", topology
        );
        const auto graphics_handle = independent_graph.ImportBuffer(
            "GraphicsIndependent",
            graphics_independent.buffer,
            RenderGraph::BufferDesc{.byte_size = byte_size}
        );
        const auto compute_handle = independent_graph.ImportBuffer(
            "ComputeIndependent",
            compute_independent.buffer,
            RenderGraph::BufferDesc{.byte_size = byte_size}
        );
        for (const auto handle : {graphics_handle, compute_handle}) {
            independent_graph.SetInitialState(
                handle,
                RenderGraph::BufferState::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
        }
        independent_graph.AddRecordPass(
            "GraphicsRoot",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(
                        graphics_handle,
                        RenderGraph::BufferState::TransferDestination
                    )
                    .SideEffect();
            },
            [
                &graphics_independent,
                &graphics_record_started,
                &compute_record_started,
                _parallel
            ](CommandList& commands) {
                if (_parallel) {
                    graphics_record_started.release();
                    if (!compute_record_started.try_acquire_for(
                            std::chrono::seconds(2)
                        )) {
                        throw std::runtime_error(
                            "Compute queue root did not record concurrently"
                        );
                    }
                }
                commands.CopyFrom(
                    OwnedBytes(graphics_independent.values),
                    graphics_independent.buffer->GetView(),
                    "ActiveRdgIndependentGraphicsRoot"
                );
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        independent_graph.AddRecordPass(
            "ComputeRoot",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(
                        compute_handle,
                        RenderGraph::BufferState::TransferDestination
                    )
                    .SideEffect();
            },
            [
                &compute_independent,
                &graphics_record_started,
                &compute_record_started,
                _parallel
            ](CommandList& commands) {
                if (_parallel) {
                    compute_record_started.release();
                    if (!graphics_record_started.try_acquire_for(
                            std::chrono::seconds(2)
                        )) {
                        throw std::runtime_error(
                            "Graphics queue root did not record concurrently"
                        );
                    }
                }
                commands.CopyFrom(
                    OwnedBytes(compute_independent.values),
                    compute_independent.buffer->GetView(),
                    "ActiveRdgIndependentComputeRoot"
                );
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        independent_graph.Export(
            graphics_handle,
            RenderGraph::BufferState::TransferDestination,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Write
        );
        independent_graph.Export(
            compute_handle,
            RenderGraph::BufferState::TransferDestination,
            RenderGraph::QueueRole::Compute,
            RenderGraph::AccessMode::Write
        );
        if (!independent_graph.Compile() ||
            std::any_of(
                independent_graph.GetCompiledPlan().queue_syncs.begin(),
                independent_graph.GetCompiledPlan().queue_syncs.end(),
                [](const RenderGraph::CompiledQueueSync& sync) {
                    return sync.gpu_wait_required;
                }
            )) {
            throw std::runtime_error(
                "independent queue roots did not compile as a zero-edge DAG: " +
                independent_graph.GetCompileError()
            );
        }
        if (!independent_graph.ExecuteRecording(
                {},
                {},
                _parallel,
                {},
                RenderGraph::ActiveRecordingOptions{.enabled = true}
            )) {
            throw std::runtime_error(
                "independent queue roots failed active execution: " +
                independent_graph.GetCompileError()
            );
        }

        CommandList compute_readback(EQueueType::Compute);
        compute_readback.CopyFrom(
            compute_independent.buffer->GetView(),
            WritableBytes(compute_independent.readback),
            "ActiveRdgIndependentComputeReadback"
        );
        RHIExecutor::Get().Submit(
            EQueueType::Compute,
            compute_readback.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        CommandList graphics_readback(EQueueType::Graphics);
        graphics_readback.CopyFrom(
            graphics_independent.buffer->GetView(),
            WritableBytes(graphics_independent.readback),
            "ActiveRdgIndependentGraphicsReadback"
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            graphics_readback.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (graphics_independent.readback != graphics_independent.values ||
            compute_independent.readback != compute_independent.values) {
            throw std::runtime_error(
                "independent queue root readback mismatch"
            );
        }
        graphics_independent.readback.fill(0);
        compute_independent.readback.fill(0);
        LOG_INFO(
            "[TESTCASE][PASS] name=ActiveRdgIndependentQueueRoots "
            "logical_syncs={} gpu_syncs=0 distinct_native={} execution={} "
            "cpu_record_cross_queue={}",
            independent_graph.GetCompiledPlan().queue_syncs.size(),
            dedicated_compute,
            dedicated_compute ? "dependency-led" : "native-serial",
            _parallel ? "parallel-verified" : "serial"
        );
    }

    if (graphics_compute_available && dedicated_compute) {
        QueueBuffer shared_source{
            .buffer = device.CreateBuffer<uint32>(
                "active_rdg_distinct_native_shared_source",
                kElementCount,
                usage
            ),
        };
        QueueBuffer shared_destination{
            .buffer = device.CreateBuffer<uint32>(
                "active_rdg_distinct_native_shared_destination",
                kElementCount,
                usage
            ),
        };
        for (uint32 element = 0; element < kElementCount; ++element) {
            shared_source.values[element] =
                0xD3100000u + element * 67u + 19u;
        }
        shared_destination.values = shared_source.values;

        RenderGraph physical_graph(
            "ActiveRdgDistinctNativePhysicalRaw", topology
        );
        const auto source_handle = physical_graph.ImportBuffer(
            "SharedSource",
            shared_source.buffer,
            RenderGraph::BufferDesc{.byte_size = byte_size}
        );
        const auto destination_handle = physical_graph.ImportBuffer(
            "SharedDestination",
            shared_destination.buffer,
            RenderGraph::BufferDesc{.byte_size = byte_size}
        );
        for (const auto handle : {source_handle, destination_handle}) {
            physical_graph.SetInitialState(
                handle,
                RenderGraph::BufferState::Undefined,
                RenderGraph::QueueRole::None,
                RenderGraph::AccessMode::None
            );
        }
        const auto graphics_write_pass = physical_graph.AddRecordPass(
            "GraphicsWriteShared",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Copy
                       )
                    .Write(
                        source_handle,
                        RenderGraph::BufferState::TransferDestination
                    )
                    .SideEffect();
            },
            [&shared_source](CommandList& commands) {
                commands.CopyFrom(
                    OwnedBytes(shared_source.values),
                    shared_source.buffer->GetView(),
                    "ActiveRdgDistinctNativeGraphicsWrite"
                );
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        const auto compute_read_pass = physical_graph.AddRecordPass(
            "ComputeReadShared",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Compute,
                           RenderGraph::PipelineType::Copy
                       )
                    .Read(
                        source_handle,
                        RenderGraph::BufferState::TransferSource
                    )
                    .Write(
                        destination_handle,
                        RenderGraph::BufferState::TransferDestination
                    )
                    .SideEffect();
            },
            [&shared_source, &shared_destination](CommandList& commands) {
                commands.CopyFrom(
                    shared_source.buffer->GetView(),
                    shared_destination.buffer->GetView(),
                    "ActiveRdgDistinctNativeComputeRead"
                );
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        physical_graph.Export(
            source_handle,
            RenderGraph::BufferState::TransferSource,
            RenderGraph::QueueRole::Compute,
            RenderGraph::AccessMode::Read
        );
        physical_graph.Export(
            destination_handle,
            RenderGraph::BufferState::TransferDestination,
            RenderGraph::QueueRole::Compute,
            RenderGraph::AccessMode::Write
        );
        if (!physical_graph.Compile() ||
            !std::any_of(
                physical_graph.GetCompiledPlan().queue_syncs.begin(),
                physical_graph.GetCompiledPlan().queue_syncs.end(),
                [](const RenderGraph::CompiledQueueSync& sync) {
                    return sync.gpu_wait_required;
                }
            )) {
            throw std::runtime_error(
                "distinct-native physical RAW did not compile with a GPU sync: " +
                physical_graph.GetCompileError()
            );
        }
        RenderGraphLowering::LoweredPlan lowered{};
        std::string                     lowering_error{};
        if (!RenderGraphLowering::Lower(
                physical_graph, lowered, lowering_error
            )) {
            throw std::runtime_error(
                "distinct-native physical RAW lowering failed: " +
                lowering_error
            );
        }

        const auto is_source_resource =
            [source = source_handle.Untyped()](
                const RenderGraphLowering::LoweredInstruction& instruction
            ) {
                return instruction.resource == source;
            };
        const auto before_compute = lowered.Before(compute_read_pass);
        const auto after_graphics = lowered.After(graphics_write_pass);
        if (same_queue_family) {
            const auto local_acquire = std::find_if(
                before_compute.begin(),
                before_compute.end(),
                [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                    return is_source_resource(instruction) &&
                           instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::Barrier &&
                           instruction.queue_acquire;
                }
            );
            const bool has_ownership_half = std::any_of(
                    after_graphics.begin(),
                    after_graphics.end(),
                    [](const RenderGraphLowering::LoweredInstruction& instruction) {
                        return instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::QueueRelease;
                    }
                ) ||
                std::any_of(
                    before_compute.begin(),
                    before_compute.end(),
                    [](const RenderGraphLowering::LoweredInstruction& instruction) {
                        return instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::QueueAcquire;
                    }
                );
            if (local_acquire == before_compute.end() ||
                has_ownership_half) {
                throw std::runtime_error(
                    "same-family physical RAW did not lower to one "
                    "destination-local acquire"
                );
            }
        } else {
            const auto release = std::find_if(
                after_graphics.begin(),
                after_graphics.end(),
                [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                    return is_source_resource(instruction) &&
                           instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::QueueRelease;
                }
            );
            const auto acquire = std::find_if(
                before_compute.begin(),
                before_compute.end(),
                [&](const RenderGraphLowering::LoweredInstruction& instruction) {
                    return is_source_resource(instruction) &&
                           instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::QueueAcquire;
                }
            );
            if (release == after_graphics.end() ||
                acquire == before_compute.end() ||
                release->barrier_index ==
                    RenderGraphLowering::InvalidBarrierIndex ||
                release->barrier_index != acquire->barrier_index ||
                release->transfer_source != topology.graphics ||
                release->transfer_destination != topology.compute ||
                acquire->transfer_source != topology.graphics ||
                acquire->transfer_destination != topology.compute) {
                throw std::runtime_error(
                    "distinct-family physical RAW did not lower to a matched "
                    "Graphics-release/Compute-acquire pair"
                );
            }
            const bool transfer_sync_correlated = std::any_of(
                lowered.queue_syncs.begin(),
                lowered.queue_syncs.end(),
                [&](const RenderGraphLowering::QueueSyncInstruction& sync) {
                    return sync.signal_queue == topology.graphics &&
                           sync.wait_queue == topology.compute &&
                           std::find(
                               sync.ownership_transfer_barriers.begin(),
                               sync.ownership_transfer_barriers.end(),
                               release->barrier_index
                           ) != sync.ownership_transfer_barriers.end();
                }
            );
            if (!transfer_sync_correlated) {
                throw std::runtime_error(
                    "distinct-family physical RAW release/acquire pair is not "
                    "correlated with its GPU queue sync"
                );
            }
        }
        if (!physical_graph.ExecuteRecording(
                {},
                {},
                _parallel,
                {},
                RenderGraph::ActiveRecordingOptions{.enabled = true}
            )) {
            throw std::runtime_error(
                "distinct-native physical RAW active execution failed: " +
                physical_graph.GetCompileError()
            );
        }
        CommandList physical_readback(EQueueType::Compute);
        physical_readback.CopyFrom(
            shared_destination.buffer->GetView(),
            WritableBytes(shared_destination.readback),
            "ActiveRdgDistinctNativePhysicalReadback"
        );
        RHIExecutor::Get().Submit(
            EQueueType::Compute,
            physical_readback.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (shared_destination.readback != shared_destination.values) {
            throw std::runtime_error(
                "distinct-native physical RAW readback mismatch"
            );
        }
        LOG_INFO(
            "[TESTCASE][PASS] name=ActiveRdgDistinctNativePhysicalRaw "
            "producer=Graphics consumer=Compute ownership={} readback=verified",
            same_queue_family ? "local-acquire" : "release-acquire"
        );
    } else {
        LOG_INFO(
            "[TESTCASE][SKIP] name=ActiveRdgDistinctNativePhysicalRaw "
            "reason={} compute_native={} graphics_native={} "
            "compute_family={} graphics_family={}",
            graphics_compute_available ? "shared_native_queue" :
                                         "queue_unavailable",
            topology.compute.native_queue_id,
            topology.graphics.native_queue_id,
            topology.compute.family_id,
            topology.graphics.family_id
        );
    }

    RenderGraph graph("ActiveRdgAsyncQueueDag", topology);
    const auto graphics_root_handle = graph.ImportBuffer(
        "GraphicsRoot",
        graphics_root.buffer,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    const auto compute_independent_handle = graph.ImportBuffer(
        "ComputeIndependent",
        compute_independent.buffer,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    const auto graphics_independent_handle = graph.ImportBuffer(
        "GraphicsIndependent",
        graphics_independent.buffer,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    const auto compute_dependent_handle = graph.ImportBuffer(
        "ComputeDependent",
        compute_dependent.buffer,
        RenderGraph::BufferDesc{.byte_size = byte_size}
    );
    for (const auto handle : {
             graphics_root_handle,
             compute_independent_handle,
             graphics_independent_handle,
             compute_dependent_handle,
         }) {
        graph.SetInitialState(
            handle,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
    }
    const auto graphics_root_done =
        graph.CreateTransientToken("GraphicsRootDone");

    graph.AddRecordPass(
        "GraphicsRoot",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    graphics_root_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .Write(graphics_root_done)
                .SideEffect();
        },
        [&graphics_root](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(graphics_root.values),
                graphics_root.buffer->GetView(),
                "ActiveRdgAsyncGraphicsRoot"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "ComputeIndependent",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    compute_independent_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [&compute_independent](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(compute_independent.values),
                compute_independent.buffer->GetView(),
                "ActiveRdgAsyncComputeIndependent"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "GraphicsIndependent",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    graphics_independent_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [&graphics_independent](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(graphics_independent.values),
                graphics_independent.buffer->GetView(),
                "ActiveRdgAsyncGraphicsIndependent"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "ComputeDependent",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Compute,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(graphics_root_done)
                .Write(
                    compute_dependent_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [&compute_dependent](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(compute_dependent.values),
                compute_dependent.buffer->GetView(),
                "ActiveRdgAsyncComputeDependent"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        graphics_root_handle,
        RenderGraph::BufferState::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    graph.Export(
        compute_independent_handle,
        RenderGraph::BufferState::TransferDestination,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Write
    );
    graph.Export(
        graphics_independent_handle,
        RenderGraph::BufferState::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write
    );
    graph.Export(
        compute_dependent_handle,
        RenderGraph::BufferState::TransferDestination,
        RenderGraph::QueueRole::Compute,
        RenderGraph::AccessMode::Write
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "active RDG async queue compile failed: " +
            graph.GetCompileError()
        );
    }
    const auto& plan = graph.GetCompiledPlan();
    const bool has_gpu_sync = std::any_of(
        plan.queue_syncs.begin(),
        plan.queue_syncs.end(),
        [](const RenderGraph::CompiledQueueSync& sync) {
            return sync.gpu_wait_required;
        }
    );
    if (has_gpu_sync != dedicated_compute) {
        throw std::runtime_error(
            "active RDG async queue plan disagrees with runtime topology"
        );
    }
    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        )) {
        throw std::runtime_error(
            "active RDG async queue execution failed: " +
            graph.GetCompileError()
        );
    }

    CommandList compute_readback(EQueueType::Compute);
    compute_readback.CopyFrom(
        compute_independent.buffer->GetView(),
        WritableBytes(compute_independent.readback),
        "ActiveRdgAsyncComputeIndependentReadback"
    );
    compute_readback.CopyFrom(
        compute_dependent.buffer->GetView(),
        WritableBytes(compute_dependent.readback),
        "ActiveRdgAsyncComputeDependentReadback"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Compute,
        compute_readback.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );

    CommandList graphics_readback(EQueueType::Graphics);
    graphics_readback.CopyFrom(
        graphics_root.buffer->GetView(),
        WritableBytes(graphics_root.readback),
        "ActiveRdgAsyncGraphicsRootReadback"
    );
    graphics_readback.CopyFrom(
        graphics_independent.buffer->GetView(),
        WritableBytes(graphics_independent.readback),
        "ActiveRdgAsyncGraphicsIndependentReadback"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        graphics_readback.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    for (const QueueBuffer* buffer : buffers) {
        if (buffer->readback != buffer->values) {
            throw std::runtime_error(
                "active RDG async queue readback mismatch"
            );
        }
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgAsyncQueueDag mode={} "
        "compute_native={} graphics_native={} gpu_sync={} "
        "submission_owner=serial",
        _parallel ? "parallel" : "serial",
        topology.compute.native_queue_id,
        topology.graphics.native_queue_id,
        has_gpu_sync
    );
}

void RunActiveRdgGraphicsCopyRoundTrip(bool _parallel) {
    auto&      device   = RenderDevice::Get();
    const auto topology = RenderGraph::QueueTopology::FromRHI();
    if (!topology.copy.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=ActiveRdgGraphicsCopyRoundTrip "
            "reason=copy_queue_unavailable graphics_native={} copy_native={} graphics_family={} "
            "copy_family={}",
            topology.graphics.native_queue_id,
            topology.copy.native_queue_id,
            topology.graphics.family_id,
            topology.copy.family_id
        );
        return;
    }

    constexpr uint64_t byte_size = sizeof(uint32) * kElementCount;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>(
        "active_rdg_graphics_copy_round_trip_source",
        kElementCount,
        usage
    );
    BufferRef intermediate = device.CreateBuffer<uint32>(
        "active_rdg_graphics_copy_round_trip_intermediate",
        kElementCount,
        usage
    );
    BufferRef destination = device.CreateBuffer<uint32>(
        "active_rdg_graphics_copy_round_trip_destination",
        kElementCount,
        usage
    );
    std::array<uint32, kElementCount> expected{};
    std::array<uint32, kElementCount> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        expected[index] = 0xC7400000u + index * 73u + 29u;
    }

    RenderGraph graph("ActiveRdgGraphicsCopyRoundTrip", topology);
    const auto source_handle = graph.ImportBuffer(
        "RoundTripSource",
        source,
        RenderGraph::BufferDesc{
            .byte_size    = byte_size,
            .sharing_mode =
                RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    const auto intermediate_handle = graph.ImportBuffer(
        "RoundTripIntermediate",
        intermediate,
        RenderGraph::BufferDesc{
            .byte_size    = byte_size,
            .sharing_mode =
                RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    const auto destination_handle = graph.ImportBuffer(
        "RoundTripDestination",
        destination,
        RenderGraph::BufferDesc{
            .byte_size    = byte_size,
            .sharing_mode =
                RenderGraph::TextureDesc::SharingMode::Exclusive,
        }
    );
    for (const auto handle : {
             source_handle,
             intermediate_handle,
             destination_handle,
         }) {
        graph.SetInitialState(
            handle,
            RenderGraph::BufferState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
    }

    const auto graphics_upload = graph.AddRecordPass(
        "GraphicsUploadSource",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    source_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [source, expected](CommandList& commands) {
            commands.CopyFrom(
                OwnedBytes(expected),
                source->GetView(),
                "ActiveRdgGraphicsCopyRoundTripUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto copy_to_intermediate = graph.AddRecordPass(
        "CopyToIntermediate",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Copy,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    source_handle,
                    RenderGraph::BufferState::TransferSource
                )
                .Write(
                    intermediate_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [source, intermediate](CommandList& commands) {
            commands.CopyFrom(
                source->GetView(),
                intermediate->GetView(),
                "ActiveRdgGraphicsCopyRoundTripToCopy"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    const auto graphics_copy_to_destination = graph.AddRecordPass(
        "GraphicsCopyToDestination",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    intermediate_handle,
                    RenderGraph::BufferState::TransferSource
                )
                .Write(
                    destination_handle,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [intermediate, destination](CommandList& commands) {
            commands.CopyFrom(
                intermediate->GetView(),
                destination->GetView(),
                "ActiveRdgGraphicsCopyRoundTripToGraphics"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "GraphicsReadback",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    destination_handle,
                    RenderGraph::BufferState::TransferSource
                )
                .SideEffect();
        },
        [destination, &readback](CommandList& commands) {
            commands.CopyFrom(
                destination->GetView(),
                WritableBytes(readback),
                "ActiveRdgGraphicsCopyRoundTripReadback"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.Export(
        source_handle,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Copy,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        intermediate_handle,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        destination_handle,
        RenderGraph::BufferState::TransferSource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "Graphics-Copy round trip compile failed: " +
            graph.GetCompileError()
        );
    }
    const size_t gpu_sync_count = std::count_if(
        graph.GetCompiledPlan().queue_syncs.begin(),
        graph.GetCompiledPlan().queue_syncs.end(),
        [](const RenderGraph::CompiledQueueSync& sync) {
            return sync.gpu_wait_required;
        }
    );
    const bool native_alias =
        topology.graphics.native_queue_id ==
        topology.copy.native_queue_id;
    const size_t expected_gpu_sync_count = native_alias ? 0 : 2;
    if (gpu_sync_count != expected_gpu_sync_count) {
        throw std::runtime_error(
            "Graphics-Copy round trip GPU synchronization count disagrees "
            "with native queue aliasing"
        );
    }

    RenderGraphLowering::LoweredPlan lowered{};
    std::string                     lowering_error{};
    if (!RenderGraphLowering::Lower(graph, lowered, lowering_error)) {
        throw std::runtime_error(
            "Graphics-Copy round trip lowering failed: " + lowering_error
        );
    }
    const bool same_queue_family =
        topology.graphics.family_id == topology.copy.family_id;
    const auto validate_boundary =
        [&](std::string_view             label,
            RenderGraph::BufferHandle    resource,
            RenderGraph::PassHandle      producer,
            RenderGraph::PassHandle      consumer,
            RenderGraph::QueueBinding    source_queue,
            RenderGraph::QueueBinding    destination_queue) {
            const auto after_producer  = lowered.After(producer);
            const auto before_consumer = lowered.Before(consumer);
            const auto matches_resource =
                [resource = resource.Untyped()](
                    const RenderGraphLowering::LoweredInstruction& instruction
                ) {
                    return instruction.resource == resource;
                };
            const auto count_kind =
                [&](auto instructions,
                    RenderGraphLowering::InstructionKind kind) {
                    return std::count_if(
                        instructions.begin(),
                        instructions.end(),
                        [&](const RenderGraphLowering::LoweredInstruction&
                                instruction) {
                            return matches_resource(instruction) &&
                                   instruction.instruction_kind == kind;
                        }
                    );
                };

            if (same_queue_family) {
                const auto local_acquire = std::find_if(
                    before_consumer.begin(),
                    before_consumer.end(),
                    [&](const RenderGraphLowering::LoweredInstruction&
                            instruction) {
                        return matches_resource(instruction) &&
                               instruction.instruction_kind ==
                                   RenderGraphLowering::InstructionKind::Barrier &&
                               instruction.queue_acquire;
                    }
                );
                const size_t local_acquire_count = std::count_if(
                    before_consumer.begin(),
                    before_consumer.end(),
                    [&](const RenderGraphLowering::LoweredInstruction&
                            instruction) {
                        return matches_resource(instruction) &&
                               instruction.instruction_kind ==
                                   RenderGraphLowering::InstructionKind::Barrier &&
                               instruction.queue_acquire;
                    }
                );
                if (local_acquire_count != 1 ||
                    count_kind(
                        after_producer,
                        RenderGraphLowering::InstructionKind::QueueRelease
                    ) != 0 ||
                    count_kind(
                        before_consumer,
                        RenderGraphLowering::InstructionKind::QueueAcquire
                    ) != 0 ||
                    local_acquire->source.stages !=
                        ERHIPipelineStageFlags::PS_NONE ||
                    local_acquire->source.access !=
                        ERHIAccessFlags::UNDEFINED) {
                    throw std::runtime_error(
                        std::string(label) +
                        " did not lower to a destination-local acquire"
                    );
                }
                return;
            }

            const auto release = std::find_if(
                after_producer.begin(),
                after_producer.end(),
                [&](const RenderGraphLowering::LoweredInstruction&
                        instruction) {
                    return matches_resource(instruction) &&
                           instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::
                                   QueueRelease;
                }
            );
            const auto acquire = std::find_if(
                before_consumer.begin(),
                before_consumer.end(),
                [&](const RenderGraphLowering::LoweredInstruction&
                        instruction) {
                    return matches_resource(instruction) &&
                           instruction.instruction_kind ==
                               RenderGraphLowering::InstructionKind::
                                   QueueAcquire;
                }
            );
            if (count_kind(
                    after_producer,
                    RenderGraphLowering::InstructionKind::QueueRelease
                ) != 1 ||
                count_kind(
                    before_consumer,
                    RenderGraphLowering::InstructionKind::QueueAcquire
                ) != 1 ||
                release == after_producer.end() ||
                acquire == before_consumer.end() ||
                release->barrier_index ==
                    RenderGraphLowering::InvalidBarrierIndex ||
                release->barrier_index != acquire->barrier_index ||
                release->transfer_source != source_queue ||
                release->transfer_destination != destination_queue ||
                acquire->transfer_source != source_queue ||
                acquire->transfer_destination != destination_queue) {
                throw std::runtime_error(
                    std::string(label) +
                    " did not lower to one matched release/acquire pair"
                );
            }
            const size_t correlated_sync_count = std::count_if(
                lowered.queue_syncs.begin(),
                lowered.queue_syncs.end(),
                [&](const RenderGraphLowering::QueueSyncInstruction& sync) {
                    return sync.signal_queue == source_queue &&
                           sync.wait_queue == destination_queue &&
                           std::find(
                               sync.ownership_transfer_barriers.begin(),
                               sync.ownership_transfer_barriers.end(),
                               release->barrier_index
                           ) != sync.ownership_transfer_barriers.end();
                    }
            );
            if (correlated_sync_count != 1) {
                throw std::runtime_error(
                    std::string(label) +
                    " release/acquire pair is not correlated with its GPU sync"
                );
            }
        };
    validate_boundary(
        "Graphics-to-Copy source transfer",
        source_handle,
        graphics_upload,
        copy_to_intermediate,
        topology.graphics,
        topology.copy
    );
    validate_boundary(
        "Copy-to-Graphics intermediate transfer",
        intermediate_handle,
        copy_to_intermediate,
        graphics_copy_to_destination,
        topology.copy,
        topology.graphics
    );

    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        )) {
        throw std::runtime_error(
            "Graphics-Copy round trip active execution failed: " +
            graph.GetCompileError()
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (readback != expected) {
        throw std::runtime_error(
            "Graphics-Copy round trip readback mismatch"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgGraphicsCopyRoundTrip mode={} "
        "ownership={} transfers=2 gpu_syncs={} graphics_native={} "
        "copy_native={} native_alias={} readback=verified",
        _parallel ? "parallel" : "serial",
        same_queue_family ? "local-acquire" : "release-acquire",
        gpu_sync_count,
        topology.graphics.native_queue_id,
        topology.copy.native_queue_id,
        native_alias
    );
}

void RunActiveRdgTransientAliasReadback(bool _parallel) {
    constexpr uint64_t byte_size = sizeof(uint32) * kElementCount;
    const RGTransientBufferDesc transient_desc{
        .element_count = kElementCount,
        .stride        = sizeof(uint32),
        .usage         = EBufferUsageFlags::TRANSFER_SRC |
                         EBufferUsageFlags::TRANSFER_DST,
    };

    std::array<uint32, kElementCount> first_values{};
    std::array<uint32, kElementCount> expected{};
    std::array<uint32, kElementCount> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        first_values[index] = 0xA1200000u + index * 19u;
        expected[index]     = 0xA1300000u + index * 53u + 7u;
    }

    RenderGraphResourcePool       pool{};
    RenderGraphTransientAllocator allocator(pool);
    RenderGraph graph("ActiveRdgTransientAlias");
    const auto first =
        graph.CreateTransientBuffer("AliasFirst", transient_desc);
    const auto second =
        graph.CreateTransientBuffer("AliasSecond", transient_desc);

    graph.AddRecordPass(
        "WriteAliasFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    first,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [&graph, first, first_values](CommandList& commands) {
            const BufferRef physical = graph.GetPhysicalBuffer(first);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "first transient alias has no physical buffer"
                );
            }
            commands.CopyFrom(
                OwnedBytes(first_values),
                physical->GetView(),
                "ActiveRdgAliasFirstUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "WriteAliasSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    second,
                    RenderGraph::BufferState::TransferDestination
                )
                .SideEffect();
        },
        [&graph, second, expected](CommandList& commands) {
            const BufferRef physical = graph.GetPhysicalBuffer(second);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "second transient alias has no physical buffer"
                );
            }
            commands.CopyFrom(
                OwnedBytes(expected),
                physical->GetView(),
                "ActiveRdgAliasSecondUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "ReadAliasSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    second,
                    RenderGraph::BufferState::TransferSource
                )
                .SideEffect();
        },
        [&graph, second, &readback](CommandList& commands) {
            const BufferRef physical = graph.GetPhysicalBuffer(second);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "transient alias readback has no physical buffer"
                );
            }
            commands.CopyFrom(
                physical->GetView(),
                WritableBytes(readback),
                "ActiveRdgAliasReadback"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "active RDG transient alias compile failed: " +
            graph.GetCompileError()
        );
    }
    const auto& plan = graph.GetCompiledPlan();
    if (plan.resources[first.resource.index].transient_slot ==
            RenderGraph::PassHandle::InvalidIndex ||
        plan.resources[first.resource.index].transient_slot !=
            plan.resources[second.resource.index].transient_slot ||
        plan.alias_boundaries.size() != 1) {
        throw std::runtime_error(
            "active RDG transient alias compiler plan did not reuse one slot"
        );
    }
    if (transient_desc.ByteSize() != byte_size) {
        throw std::runtime_error("transient alias descriptor byte size mismatch");
    }
    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled             = true,
                .transient_allocator = &allocator,
            }
        )) {
        throw std::runtime_error(
            "active RDG transient alias execution failed: " +
            graph.GetCompileError()
        );
    }
    if (pool.BufferCount() != 1) {
        throw std::runtime_error(
            "active RDG transient aliases allocated more than one buffer"
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (readback != expected) {
        throw std::runtime_error(
            "active RDG transient alias readback mismatch"
        );
    }
    if (pool.AvailableBufferCount() != 1) {
        throw std::runtime_error(
            "active RDG transient alias allocation did not retire at Completion"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgTransientAlias mode={} "
        "passes=3 slots=1 buffers=1 state_owner=rdg readback=verified",
        _parallel ? "parallel" : "serial"
    );
}

void RunActiveRdgTransientTextureAliasReadback(bool _parallel) {
    constexpr uint32 kWidth      = 4;
    constexpr uint32 kHeight     = 4;
    constexpr uint32 kTexelCount = kWidth * kHeight;
    const RGTransientTextureDesc transient_desc{
        .dimension    = ETextureDimension::TEX_2D,
        .extent       = Extent3D(kWidth, kHeight, 1),
        .format       = PF_R8G8B8A8_UNORM,
        .usage        = ETextureUsageFlags::TRANSFER_SRC |
                        ETextureUsageFlags::TRANSFER_DST,
        .aspect_flags = ETextureAspectFlags::COLOR,
        .mip_count    = 1,
        .array_size   = 1,
    };

    std::array<uint32, kTexelCount> first_values{};
    std::array<uint32, kTexelCount> expected{};
    std::array<uint32, kTexelCount> readback{};
    for (uint32 index = 0; index < kTexelCount; ++index) {
        first_values[index] = 0xFF102030u + index;
        expected[index]     = 0xFF405060u + index * 0x00010101u;
    }

    RenderGraphResourcePool       pool{};
    RenderGraphTransientAllocator allocator(pool);
    RenderGraph graph("ActiveRdgTransientTextureAlias");
    const auto first =
        graph.CreateTransientTexture("TextureAliasFirst", transient_desc);
    const auto second =
        graph.CreateTransientTexture("TextureAliasSecond", transient_desc);
    graph.AddRecordPass(
        "WriteTextureAliasFirst",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    first,
                    RenderGraph::TextureState::TransferDestination
                )
                .SideEffect();
        },
        [&graph, first, first_values](CommandList& commands) {
            const TextureRef physical = graph.GetPhysicalTexture(first);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "first transient texture alias has no physical texture"
                );
            }
            commands.CopyFrom(
                OwnedBytes(first_values),
                physical->GetView(),
                "ActiveRdgTextureAliasFirstUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "WriteTextureAliasSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    second,
                    RenderGraph::TextureState::TransferDestination
                )
                .SideEffect();
        },
        [&graph, second, expected](CommandList& commands) {
            const TextureRef physical = graph.GetPhysicalTexture(second);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "second transient texture alias has no physical texture"
                );
            }
            commands.CopyFrom(
                OwnedBytes(expected),
                physical->GetView(),
                "ActiveRdgTextureAliasSecondUpload"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "ReadTextureAliasSecond",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Read(
                    second,
                    RenderGraph::TextureState::TransferSource
                )
                .SideEffect();
        },
        [&graph, second, &readback](CommandList& commands) {
            const TextureRef physical = graph.GetPhysicalTexture(second);
            if (!physical.IsValid()) {
                throw std::runtime_error(
                    "transient texture alias readback has no physical texture"
                );
            }
            commands.CopyFrom(
                physical->GetView(),
                WritableBytes(readback),
                "ActiveRdgTextureAliasReadback"
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "active RDG transient texture alias compile failed: " +
            graph.GetCompileError()
        );
    }
    const auto& plan = graph.GetCompiledPlan();
    if (plan.resources[first.resource.index].transient_slot !=
            plan.resources[second.resource.index].transient_slot ||
        plan.alias_boundaries.size() != 1) {
        throw std::runtime_error(
            "active RDG transient texture alias did not reuse one slot"
        );
    }
    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{
                .enabled             = true,
                .transient_allocator = &allocator,
            }
        )) {
        throw std::runtime_error(
            "active RDG transient texture alias execution failed: " +
            graph.GetCompileError()
        );
    }
    if (pool.TextureCount() != 1) {
        throw std::runtime_error(
            "active RDG transient texture aliases allocated more than one texture"
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (readback != expected) {
        throw std::runtime_error(
            "active RDG transient texture alias readback mismatch"
        );
    }
    if (pool.AvailableTextureCount() != 1) {
        throw std::runtime_error(
            "active RDG transient texture alias did not retire at Completion"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgTransientTextureAlias mode={} "
        "passes=3 slots=1 textures=1 state_owner=rdg readback=verified",
        _parallel ? "parallel" : "serial"
    );
}

void RunTransientDepthStencilAspectAllocation() {
    TextureRef legacy = RenderDevice::Get().CreateTexture(
        "LegacyDepthStencilDefaultAspect",
        Extent3D(8, 8, 1),
        PF_D32_SFLOAT_S8_UINT,
        ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
    );
    if (!legacy.IsValid() ||
        legacy->GetAspectFlags() != ETextureAspectFlags::DEPTH_SLICE) {
        throw std::runtime_error(
            "legacy depth-stencil texture no longer defaults to a depth-only view"
        );
    }
    legacy = {};

    const RGTransientTextureDesc desc{
        .dimension = ETextureDimension::TEX_2D,
        .extent    = Extent3D(8, 8, 1),
        .format    = PF_D32_SFLOAT_S8_UINT,
        .usage     = ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT,
        .aspect_flags =
            ETextureAspectFlags::DEPTH_SLICE |
            ETextureAspectFlags::STENCIL_SLICE,
        .mip_count  = 1,
        .array_size = 1,
    };

    RenderGraphResourcePool pool{};
    TextureRef texture =
        pool.AcquireTexture("TransientDepthStencilAspect", desc);
    if (!texture.IsValid() || texture->GetAspectFlags() != desc.aspect_flags) {
        throw std::runtime_error(
            "transient depth-stencil allocation lost a physical aspect"
        );
    }
    texture = {};
    if (pool.AvailableTextureCount() != 1) {
        throw std::runtime_error(
            "transient depth-stencil allocation did not return to the pool"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=TransientDepthStencilAspectAllocation "
        "format=D32S8 transient=depth,stencil legacy=depth factory=default"
    );
}

void RunActiveRdgTextureArraySubrange(bool _parallel) {
    auto& device = RenderDevice::Get();
    constexpr uint32 kLayerCount = 4;
    constexpr uint32 kFirstLayer = 1;
    constexpr uint32 kTestLayerCount = 2;
    TextureRef texture = device.CreateTexture(
        "active_rdg_array_subrange",
        Extent3D(8, 8, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC,
        1,
        kLayerCount
    );

    RenderGraph graph("ActiveRdgTextureArraySubrange");
    const auto texture_handle = graph.ImportTexture(
        "ArrayTexture",
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = 1,
            .layer_count = kLayerCount,
            .aspects     = RenderGraph::TextureAspect::Color,
        }
    );
    auto range =
        RenderGraph::TextureRange::Layers(kFirstLayer, kTestLayerCount);
    range.aspects = RenderGraph::TextureAspect::Color;
    graph.SetInitialState(
        texture_handle,
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        range
    );
    graph.AddRecordPass(
        "ClearArrayLayers",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(
                    texture_handle,
                    RenderGraph::TextureState::TransferDestination,
                    range
                )
                .SideEffect();
        },
        [texture](CommandList& commands) {
            commands.ClearResource(
                texture->GetView().Slice(kFirstLayer, kTestLayerCount),
                float4{0.125f, 0.25f, 0.5f, 1.0f}
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.Export(
        texture_handle,
        RenderGraph::TextureState::TransferDestination,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Write,
        range
    );

    if (!graph.Compile()) {
        throw std::runtime_error(
            "active RDG texture array compile failed: " + graph.GetCompileError()
        );
    }
    if (!graph.ExecuteRecording(
            {},
            {},
            _parallel,
            {},
            RenderGraph::ActiveRecordingOptions{.enabled = true}
        )) {
        throw std::runtime_error(
            "active RDG texture array execution failed: " + graph.GetCompileError()
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    LOG_INFO(
        "[TESTCASE][PASS] name=ActiveRdgTextureArraySubrange mode={} "
        "layers={}+{} state_owner=rdg",
        _parallel ? "parallel" : "serial",
        kFirstLayer,
        kTestLayerCount
    );
}

void RunExplicitTextureArrayRangeShapeChange(bool _parallel) {
    auto& device = RenderDevice::Get();
    constexpr uint32 kLayerCount = 4;
    constexpr uint32 kFirstLayer = 1;
    constexpr uint32 kSubsetLayerCount = 2;
    TextureRef texture = device.CreateTexture(
        "explicit_array_shape_change",
        Extent3D(8, 8, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST,
        1,
        kLayerCount
    );
    const TextureView whole = texture->GetView().Slice(0, kLayerCount);
    const TextureView subset =
        texture->GetView().Slice(kFirstLayer, kSubsetLayerCount);
    const BarrierState undefined = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TOP_OF_PIPE,
        ERHIAccessFlags::UNDEFINED,
        ETextureLayout::TEXTURE_LAYOUT_UNDEFINED
    );
    const BarrierState transfer_destination = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_WRITE,
        ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST
    );

    CommandList commands(EQueueType::Graphics);
    commands.Barriers({
        BarrierCreateInfo::Transition(
            whole,
            undefined,
            transfer_destination,
            ETextureAspectFlags::COLOR
        ),
    });
    commands.ClearResource(whole, float4{0.125f, 0.25f, 0.5f, 1.0f});
    commands.Barriers({
        BarrierCreateInfo::Transition(
            subset,
            transfer_destination,
            transfer_destination,
            ETextureAspectFlags::COLOR
        ),
    });
    commands.ClearResource(subset, float4{0.75f, 0.5f, 0.25f, 1.0f});
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    LOG_INFO(
        "[TESTCASE][PASS] name=ExplicitTextureArrayRangeShapeChange mode={} "
        "shape=0+{}->{}+{} state_owner=explicit",
        _parallel ? "parallel" : "serial",
        kLayerCount,
        kFirstLayer,
        kSubsetLayerCount
    );
}

void RunUpperTopologyBatch() {
    auto& device = RenderDevice::Get();
    constexpr size_t element_count = 16;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>("topology_source", element_count, usage);
    BufferRef destination =
        device.CreateBuffer<uint32>("topology_destination", element_count, usage);

    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        expected[index] = 0x71000000u + index * 31u;
    }

    std::mutex callback_mutex{};
    std::vector<uint32> callback_order{};
    auto append_callback = [&](uint32 value) {
        return [&, value] {
            std::lock_guard lock(callback_mutex);
            callback_order.push_back(value);
        };
    };

    Array<CommandList> command_lists{};
    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().CopyFrom(OwnedBytes(expected), source->GetView(), "TopologyUpload");
    command_lists.back().AddCallback(append_callback(0));

    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    command_lists.back().CopyFrom(
        source->GetView(), destination->GetView(), "TopologySerialControlCopy"
    );
    command_lists.back().AddCallback(append_callback(1));

    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().CopyFrom(
        destination->GetView(), WritableBytes(readback), "TopologyReadback"
    );
    command_lists.back().AddCallback(append_callback(2));

    // Empty command streams with observable completion work remain topology
    // nodes and must retire after every prior source submission.
    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    command_lists.back().AddCallback(append_callback(3));

    RHIExecutor::Get().Submit(
        std::move(command_lists), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (readback != expected) {
        throw std::runtime_error("upper topology batch GPU ordering mismatch");
    }
    const std::vector<uint32> expected_callbacks{0, 1, 2, 3};
    if (callback_order != expected_callbacks) {
        throw std::runtime_error("upper topology batch callback ordering mismatch");
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=UpperSubmissionTopology sources=4 serial_control=2 "
        "empty_side_effect=1"
    );
}

void RunPendingSourceTopologyBatch() {
    auto& device = RenderDevice::Get();
    constexpr size_t element_count = 16;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef source =
        device.CreateBuffer<uint32>("topology_pending_source", element_count, usage);
    BufferRef destination =
        device.CreateBuffer<uint32>("topology_pending_destination", element_count, usage);

    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        expected[index] = 0x39000000u + index * 17u;
    }

    std::mutex          callback_mutex{};
    std::vector<uint32> callback_order{};
    auto append_callback = [&](uint32 value) {
        return [&, value] {
            std::lock_guard lock(callback_mutex);
            callback_order.push_back(value);
        };
    };

    Array<CommandList> pass_lists{};
    pass_lists.emplace_back(EQueueType::Graphics);
    pass_lists.back().CopyFrom(OwnedBytes(expected), source->GetView(), "PendingPassUpload");
    pass_lists.back().AddCallback(append_callback(0));

    pass_lists.emplace_back(EQueueType::Graphics);
    pass_lists.back().CopyFrom(
        source->GetView(), destination->GetView(), "PendingPassCopy"
    );
    pass_lists.back().AddCallback(append_callback(1));

    pass_lists.emplace_back(EQueueType::Graphics);
    pass_lists.back().CopyFrom(
        destination->GetView(), WritableBytes(readback), "PendingPassReadback"
    );
    pass_lists.back().AddCallback(append_callback(2));

    pass_lists.emplace_back(EQueueType::Graphics);
    pass_lists.back().AddCallback(append_callback(3));

    for (size_t index = 0; index < pass_lists.size(); ++index) {
        CmdSubmit pass_submit = pass_lists[index]
                                    .SetTranslateExecutionClass(
                                        ERHITranslateExecutionClass::SerialControl
                                    )
                                    .Submit();
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(pass_submit),
            index + 1 == pass_lists.size() ? ERHIExecSubmitFlags::FlushGPU :
                                             ERHIExecSubmitFlags::None
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (readback != expected) {
        throw std::runtime_error("pending-source topology GPU ordering mismatch");
    }
    if (callback_order != std::vector<uint32>{0, 1, 2, 3}) {
        throw std::runtime_error("pending-source topology callback ordering mismatch");
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=PendingSourceSubmissionTopology sources=4 "
        "publish=final-source"
    );
}

void RunContinuousFrameInFlightRetirement() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();

    constexpr uint64 frame_count   = 16;
    constexpr uint64 max_in_flight = s_queue_max_frame_in_flight;
    constexpr size_t element_count = 32;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;

    BufferRef frame_output =
        device.CreateBuffer<uint32>("continuous_frame_output", element_count, usage);
    FenceRef frame_fence = device.CreateFence();

    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    std::mutex                        callback_mutex{};
    std::vector<uint64>               callback_order{};
    uint64                            submitted_frame = 0;
    std::binary_semaphore             blocked_callback_entered{0};
    std::binary_semaphore             release_blocked_callback{0};
    std::binary_semaphore             fourth_translate_reached{0};
    OneShotSemaphoreRelease release_guard(release_blocked_callback);

    try {
        for (uint64 frame = 0; frame < frame_count; ++frame) {
            // Match Renderer::PrepareRenderFrame(): allow three outstanding
            // frames and wait only on the shared external completion fence.
            // Deliberately do not call RHIExecutor::Sync() inside the loop.
            if (submitted_frame >= max_in_flight) {
                frame_fence->Wait(submitted_frame - max_in_flight);
            }
            if (frame == max_in_flight) {
                if (!blocked_callback_entered.try_acquire_for(5s)) {
                    throw std::runtime_error(
                        "continuous frame-in-flight Completion callback did not block"
                    );
                }
                // Make the capacity condition deterministic: the first three
                // packets reached native submission while frame 1's ordinary
                // Completion callback is still blocked.
                std::atomic_bool continue_waiting_for_submission{true};
                std::jthread submission_deadline(
                    [&continue_waiting_for_submission](std::stop_token _stop) {
                        const auto deadline =
                            std::chrono::steady_clock::now() + 5s;
                        while (!_stop.stop_requested() &&
                               std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(10ms);
                        }
                        if (!_stop.stop_requested()) {
                            continue_waiting_for_submission.store(
                                false, std::memory_order_release
                            );
                        }
                    }
                );
                const bool submitted = ResourceCast(frame_fence.Get())
                                           ->WaitSubmitted(
                                               submitted_frame,
                                               &continue_waiting_for_submission
                                           );
                submission_deadline.request_stop();
                submission_deadline.join();
                if (!submitted) {
                    throw std::runtime_error(
                        "continuous frame-in-flight packets were not submitted "
                        "before the deadline"
                    );
                }
            }

            std::array<uint32, element_count> frame_values{};
            for (uint32 index = 0; index < frame_values.size(); ++index) {
                frame_values[index] =
                    0x68000000u + static_cast<uint32>(frame) * 4096u +
                    index * 23u;
            }
            if (frame + 1 == frame_count) {
                expected = frame_values;
            }

            CommandList commands(EQueueType::Graphics);
            if (frame == max_in_flight) {
                commands.AddCustomCommand(
                    MakeUnique<TranslateProbeCommand>(&fourth_translate_reached),
                    "ContinuousFrameTranslateProbe"
                );
            }
            commands.CopyFrom(
                OwnedBytes(frame_values),
                frame_output->GetView(),
                "ContinuousFrameUpload"
            );
            if (frame + 1 == frame_count) {
                commands.CopyFrom(
                    frame_output->GetView(),
                    WritableBytes(readback),
                    "ContinuousFrameReadback"
                );
            }
            commands.AddCallback([&, completed_frame = frame + 1] {
                {
                    std::lock_guard lock(callback_mutex);
                    callback_order.push_back(completed_frame);
                }
                if (completed_frame == 1) {
                    blocked_callback_entered.release();
                    release_blocked_callback.acquire();
                }
            });

            submitted_frame = frame + 1;
            CmdSubmit submit = commands.Submit();
            submit.Signal(frame_fence.Get(), submitted_frame);
            RHIExecutor::Get().Submit(
                EQueueType::Graphics,
                std::move(submit),
                ERHIExecSubmitFlags::FlushGPU
            );
            if (frame == max_in_flight) {
                const bool translated_while_completion_blocked =
                    fourth_translate_reached.try_acquire_for(5s);
                release_guard.Release();
                if (!translated_while_completion_blocked) {
                    throw std::runtime_error(
                        "Completion callback blocked allocator pool reuse"
                    );
                }
            }
        }

        frame_fence->Wait(frame_count);
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (readback != expected) {
            throw std::runtime_error(
                "continuous frame-in-flight final GPU readback mismatch"
            );
        }
        if (callback_order.size() != frame_count) {
            throw std::runtime_error(
                "continuous frame-in-flight callbacks did not all retire"
            );
        }
        for (uint64 frame = 0; frame < frame_count; ++frame) {
            if (callback_order[frame] != frame + 1) {
                throw std::runtime_error(
                    "continuous frame-in-flight callback ordering mismatch"
                );
            }
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        // Keep every callback capture and the external Vulkan fence alive
        // until all accepted packets have retired. This avoids turning an
        // assertion failure into use-after-free or semaphore-in-use noise.
        release_guard.Release();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            // Unwinding would destroy callback captures and the external
            // Vulkan fence while accepted packets may still reference them.
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=ContinuousFrameInFlightRetirement "
        "frames={} max_in_flight={} intermediate_sync=0 shared_fence=1 "
        "allocator_reuse=completion_pool overflow=true blocked_callback_frame=1",
        frame_count,
        max_in_flight
    );
}

void RunCrossQueueTopologyBatch() {
    auto& device = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    constexpr size_t element_count = 32;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>("topology_cross_source", element_count, usage);
    BufferRef compute_stage =
        device.CreateBuffer<uint32>("topology_cross_compute", element_count, usage);
    BufferRef copy_stage =
        device.CreateBuffer<uint32>("topology_cross_copy", element_count, usage);

    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    std::array<std::atomic<uint32>, 4> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 4> success_callbacks{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        expected[index] = 0x53000000u + index * 43u;
    }

    Array<CommandList> command_lists{};
    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().CopyFrom(
        OwnedBytes(expected), source->GetView(), "TopologyCrossQueueUpload"
    );
    command_lists.back().ExportResourcesToQueue(
        EQueueType::Compute,
        {},
        Array<ExportBuffer>{{source->GetView(), EBufferState::TRANSFER}}
    );

    command_lists.emplace_back(EQueueType::Compute);
    command_lists.back().ImportResourcesFromQueue(
        EQueueType::Graphics,
        {},
        Array<ImportBuffer>{{source->GetView(), EBufferState::TRANSFER}}
    );
    command_lists.back().CopyFrom(
        source->GetView(), compute_stage->GetView(), "TopologyCrossQueueCompute"
    );
    command_lists.back().ExportResourcesToQueue(
        EQueueType::Copy,
        {},
        Array<ExportBuffer>{{compute_stage->GetView(), EBufferState::TRANSFER}}
    );

    command_lists.emplace_back(EQueueType::Copy);
    command_lists.back().ImportResourcesFromQueue(
        EQueueType::Compute,
        {},
        Array<ImportBuffer>{{compute_stage->GetView(), EBufferState::TRANSFER}}
    );
    command_lists.back().CopyFrom(
        compute_stage->GetView(), copy_stage->GetView(), "TopologyCrossQueueCopy"
    );
    command_lists.back().ExportResourcesToQueue(
        EQueueType::Graphics,
        {},
        Array<ExportBuffer>{{copy_stage->GetView(), EBufferState::TRANSFER}}
    );

    command_lists.emplace_back(EQueueType::Graphics);
    command_lists.back().ImportResourcesFromQueue(
        EQueueType::Copy,
        {},
        Array<ImportBuffer>{{copy_stage->GetView(), EBufferState::TRANSFER}}
    );
    command_lists.back().CopyFrom(
        copy_stage->GetView(), WritableBytes(readback), "TopologyCrossQueueReadback"
    );

    for (size_t index = 0; index < command_lists.size(); ++index) {
        command_lists[index].AddCallback([&, index] {
            ordinary_callbacks[index].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        command_lists[index].AddSuccessCallback([&, index] {
            success_callbacks[index].fetch_add(
                1, std::memory_order_relaxed
            );
        });
    }

    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    const uint32 main_thread_id = Platform::GetCurrentThreadID();
    for (CommandList& command_list : command_lists) {
        const EQueueType queue = command_list.GetQueueType();
        RHIExecutor::Get().Submit(
            queue, command_list.Submit(), ERHIExecSubmitFlags::FlushGPU
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (readback != expected) {
        throw std::runtime_error("cross-queue topology GPU ordering mismatch");
    }
    if (native_capture.Overflowed() || native_capture.Count() != 4) {
        throw std::runtime_error(
            "cross-queue batch did not issue exactly four native submits"
        );
    }
    constexpr std::array expected_queues{
        EQueueType::Graphics,
        EQueueType::Compute,
        EQueueType::Copy,
        EQueueType::Graphics,
    };
    const uint32 submission_thread_id =
        native_capture.Event(0).thread_id;
    for (size_t index = 0; index < expected_queues.size(); ++index) {
        const VulkanNativeSubmissionEvent& event =
            native_capture.Event(index);
        if (event.queue != expected_queues[index] ||
            event.thread_role != ERHIThreadRole::Submission ||
            event.thread_id != submission_thread_id ||
            event.thread_id == main_thread_id ||
            !event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "Graphics/Compute/Copy native submit escaped the sole "
                "Submission owner or changed source order"
            );
        }
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "cross-queue callbacks did not retire exactly once"
            );
        }
    }
    const bool native_alias =
        native_capture.Event(0).native_queue_handle ==
        native_capture.Event(2).native_queue_handle;
    if (native_alias !=
        (topology.graphics.native_queue_id ==
         topology.copy.native_queue_id)) {
        throw std::runtime_error(
            "observed Graphics/Copy native queue alias disagrees with topology"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 4) {
        throw std::runtime_error(
            "a second Sync unexpectedly issued another native submit"
        );
    }
    for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "a second Sync replayed cross-queue callbacks"
            );
        }
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=CrossQueueSubmissionTopology "
        "batches=4 queues=Graphics,Compute,Copy,Graphics ownership=explicit "
        "native_owner=verified native_alias={} callbacks=exactly_once replay=0",
        native_alias
    );
}

void RunAsyncQueueParallelTranslateSmoke() {
    using namespace std::chrono_literals;

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available ||
        !topology.compute.available ||
        !topology.copy.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "graphics_available={} compute_available={} copy_available={} "
            "graphics_native={} compute_native={} copy_native={} "
            "reason=queue_unavailable "
            "cpu_seam=VulkanTranslateWaveScheduler",
            topology.graphics.available,
            topology.compute.available,
            topology.copy.available,
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id,
            topology.copy.native_queue_id
        );
        return;
    }
    if (topology.graphics.native_queue_id == topology.compute.native_queue_id) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "graphics_available={} compute_available={} copy_available={} "
            "graphics_native={} compute_native={} copy_native={} "
            "reason=graphics_compute_native_alias "
            "cpu_seam=VulkanTranslateWaveScheduler",
            topology.graphics.available,
            topology.compute.available,
            topology.copy.available,
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id,
            topology.copy.native_queue_id
        );
        return;
    }

    constexpr uint64 async_queue_scope =
        0x5048415345313545ull;
    constexpr std::array<uint32, 8> copy_expected{
        0x15E00001u,
        0x15E00002u,
        0x15E00003u,
        0x15E00004u,
        0x15E00005u,
        0x15E00006u,
        0x15E00007u,
        0x15E00008u,
    };
    std::array<uint32, copy_expected.size()> copy_readback{};
    BufferRef copy_buffer = device.CreateBuffer<uint32>(
        "phase15e_copy_ready_lane",
        copy_expected.size(),
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );
    FenceRef copy_done = device.CreateFence();

    SourceSubmissionCapture        submission_capture(
        async_queue_scope
    );
    ScopedSourceSubmissionObserver submission_observer(
        submission_capture
    );
    SourceTranslationCapture        translation_capture(
        async_queue_scope
    );
    ScopedSourceTranslationObserver translation_observer(
        translation_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(
        native_capture
    );
    std::array<std::atomic<uint32>, 4> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 4> success_callbacks{};
    std::binary_semaphore graphics_front_entered{0};
    std::binary_semaphore graphics_tail_entered{0};
    std::binary_semaphore compute_later_entered{0};
    std::binary_semaphore release_graphics_front{0};
    std::binary_semaphore release_compute_later{0};
    OneShotSemaphoreRelease release_graphics_guard(
        release_graphics_front
    );
    OneShotSemaphoreRelease release_compute_guard(
        release_compute_later
    );
    const bool copy_has_independent_native_lane =
        topology.copy.native_queue_id !=
            topology.graphics.native_queue_id &&
        topology.copy.native_queue_id !=
            topology.compute.native_queue_id;
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    try {
        CommandList graphics_front(EQueueType::Graphics);
        graphics_front.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_front.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Graphics,
                &graphics_front_entered,
                &release_graphics_front
            ),
            "AsyncQueueGraphicsFrontBlockingTranslateProbe"
        );
        graphics_front.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(1, std::memory_order_relaxed);
        });
        graphics_front.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(1, std::memory_order_relaxed);
        });
        CmdSubmit graphics_front_submit = graphics_front.Submit();
        graphics_front_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        graphics_front_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_front_submit.async_queue_scope = async_queue_scope;

        CommandList graphics_tail(EQueueType::Graphics);
        graphics_tail.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_tail.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &graphics_tail_entered
            ),
            "AsyncQueueGraphicsTailTranslateProbe"
        );
        graphics_tail.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(1, std::memory_order_relaxed);
        });
        graphics_tail.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
        });
        CmdSubmit graphics_tail_submit = graphics_tail.Submit();
        graphics_tail_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        graphics_tail_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_tail_submit.async_queue_scope = async_queue_scope;

        CommandList compute_later(EQueueType::Compute);
        compute_later.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        compute_later.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Compute,
                &compute_later_entered,
                &release_compute_later
            ),
            "AsyncQueueComputeLaterBlockingTranslateProbe"
        );
        compute_later.AddCallback([&] {
            ordinary_callbacks[2].fetch_add(1, std::memory_order_relaxed);
        });
        compute_later.AddSuccessCallback([&] {
            success_callbacks[2].fetch_add(1, std::memory_order_relaxed);
        });
        CmdSubmit compute_later_submit = compute_later.Submit();
        compute_later_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        compute_later_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        compute_later_submit.async_queue_scope = async_queue_scope;

        CommandList copy(EQueueType::Copy);
        copy.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy.CopyFrom(
            OwnedBytes(copy_expected),
            copy_buffer->GetView(),
            "Phase15ECopyReadyUpload"
        );
        copy.CopyFrom(
            copy_buffer->GetView(),
            WritableBytes(copy_readback),
            "Phase15ECopyReadyReadback"
        );
        copy.AddCallback([&] {
            ordinary_callbacks[3].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        copy.AddSuccessCallback([&] {
            success_callbacks[3].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit copy_submit = copy.Submit();
        copy_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        copy_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy_submit.async_queue_scope = async_queue_scope;
        copy_submit.Signal(copy_done.Get(), 1);

        Array<RHIBackendSubmissionBatchEntry> submits{};
        submits.emplace_back(
            EQueueType::Graphics,
            std::move(graphics_front_submit)
        );
        submits.emplace_back(
            EQueueType::Graphics,
            std::move(graphics_tail_submit)
        );
        submits.emplace_back(
            EQueueType::Compute,
            std::move(compute_later_submit)
        );
        submits.emplace_back(
            EQueueType::Copy,
            std::move(copy_submit)
        );
        RHIExecutor::Get().Submit(
            std::move(submits),
            ERHIExecSubmitFlags::FlushGPU
        );

        const auto deadline =
            std::chrono::steady_clock::now() + 5s;
        const bool graphics_front_translate_entered =
            graphics_front_entered.try_acquire_until(deadline);
        const bool compute_later_translate_entered =
            compute_later_entered.try_acquire_until(deadline);
        bool copy_recorded_before_release = false;
        if (copy_has_independent_native_lane) {
            copy_recorded_before_release =
                translation_capture.copy_recorded.try_acquire_until(
                    deadline
                );
        }
        const bool graphics_tail_entered_before_release =
            graphics_tail_entered.try_acquire();
        const bool source_submitted_before_release =
            submission_capture.Count() != 0;
        const bool native_submitted_before_release =
            native_capture.Count() != 0;
        release_graphics_guard.Release();
        release_compute_guard.Release();

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const bool graphics_tail_translate_entered =
            graphics_tail_entered_before_release ||
            graphics_tail_entered.try_acquire();

        if (!graphics_front_translate_entered ||
            !compute_later_translate_entered ||
            (copy_has_independent_native_lane &&
             !copy_recorded_before_release) ||
            graphics_tail_entered_before_release ||
            source_submitted_before_release ||
            native_submitted_before_release ||
            !graphics_tail_translate_entered) {
            throw std::runtime_error(
                "G/C/Copy ready native lanes or stable submission cursor "
                "violated their release contract"
            );
        }
        if (copy_readback != copy_expected) {
            throw std::runtime_error(
                "Copy ready-lane GPU readback mismatch"
            );
        }
        auto* vk_copy_done = ResourceCast(copy_done.Get());
        if (vk_copy_done->HostWait(1) != VK_SUCCESS ||
            vk_copy_done->IsRejected(1) ||
            vk_copy_done->IsFailed()) {
            throw std::runtime_error(
                "Copy ready-lane signal did not complete successfully"
            );
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "G/C/Copy parallel Translate callbacks did not "
                    "retire exactly once"
                );
            }
        }

        if (submission_capture.Overflowed() ||
            submission_capture.Count() != 4) {
            throw std::runtime_error(
                "G/C/Copy parallel Translate did not produce exactly "
                "four source submissions"
            );
        }
        constexpr std::array expected_queues{
            EQueueType::Graphics,
            EQueueType::Graphics,
            EQueueType::Compute,
            EQueueType::Copy,
        };
        const uint64 observed_batch_sequence =
            submission_capture.Event(0).batch_sequence;
        if (observed_batch_sequence == 0) {
            throw std::runtime_error(
                "Submission observer reported an invalid batch sequence"
            );
        }
        for (size_t index = 0; index < expected_queues.size(); ++index) {
            const VulkanSourceSubmissionEvent& event =
                submission_capture.Event(index);
            if (event.source_index != index ||
                event.queue != expected_queues[index] ||
                event.batch_sequence != observed_batch_sequence) {
                throw std::runtime_error(
                    "Submission owner did not preserve stable G,G,C,Copy "
                    "source order in one batch"
                );
            }
        }

        if (!translation_capture.IsValid()) {
            throw std::runtime_error(
                "source translation observer overflowed, duplicated a "
                "phase, or observed a wrong owner"
            );
        }
        for (size_t index = 0; index < expected_queues.size();
             ++index) {
            if (!translation_capture.Seen(
                    static_cast<uint32>(index),
                    EVulkanSourceTranslationPhase::Begin
                ) ||
                !translation_capture.Seen(
                    static_cast<uint32>(index),
                    EVulkanSourceTranslationPhase::Recorded
                ) ||
                translation_capture.Seen(
                    static_cast<uint32>(index),
                    EVulkanSourceTranslationPhase::Failed
                )) {
                throw std::runtime_error(
                    "source translation observer lost a successful "
                    "G/G/C/Copy phase"
                );
            }
            const VulkanSourceTranslationEvent& event =
                translation_capture.Event(
                    static_cast<uint32>(index),
                    EVulkanSourceTranslationPhase::Recorded
                );
            if (event.queue != expected_queues[index] ||
                event.batch_sequence != observed_batch_sequence ||
                event.thread_role != ERHIThreadRole::Translate ||
                event.native_queue_id !=
                    topology.Resolve(event.queue).native_queue_id) {
                throw std::runtime_error(
                    "source translation observer reported unstable "
                    "identity or ownership"
                );
            }
        }

        if (native_capture.Overflowed() ||
            native_capture.Count() != expected_queues.size()) {
            throw std::runtime_error(
                "G/G/C/Copy ready-lane batch did not issue exactly four "
                "native submits"
            );
        }
        const uint32 submission_thread_id =
            native_capture.Event(0).thread_id;
        for (size_t index = 0; index < expected_queues.size();
             ++index) {
            const VulkanNativeSubmissionEvent& event =
                native_capture.Event(index);
            if (event.queue != expected_queues[index] ||
                event.thread_role != ERHIThreadRole::Submission ||
                event.thread_id != submission_thread_id ||
                event.thread_id == main_thread_id ||
                !event.outcome.WasSubmitted()) {
                throw std::runtime_error(
                    "native G/G/C/Copy submit lost stable order or the "
                    "sole Submission owner"
                );
            }
        }
        for (size_t lhs = 0; lhs < expected_queues.size(); ++lhs) {
            for (size_t rhs = lhs + 1;
                 rhs < expected_queues.size();
                 ++rhs) {
                const bool topology_alias =
                    topology.Resolve(expected_queues[lhs])
                        .native_queue_id ==
                    topology.Resolve(expected_queues[rhs])
                        .native_queue_id;
                const bool observed_alias =
                    native_capture.Event(lhs).native_queue_handle ==
                    native_capture.Event(rhs).native_queue_handle;
                if (topology_alias != observed_alias) {
                    throw std::runtime_error(
                        "native observer queue alias disagreed with "
                        "RHIQueueTopology"
                    );
                }
            }
        }

        const size_t source_count_before_replay =
            submission_capture.Count();
        const size_t native_count_before_replay =
            native_capture.Count();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "a second Sync replayed G/C/Copy Translate callbacks"
                );
            }
        }
        if (submission_capture.Count() !=
                source_count_before_replay ||
            native_capture.Count() !=
                native_count_before_replay) {
            throw std::runtime_error(
                "a second Sync replayed source or native submission"
            );
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        release_graphics_guard.Release();
        release_compute_guard.Release();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=AsyncQueueParallelTranslateSmoke "
        "graphics_native={} compute_native={} copy_native={} "
        "sources=4 batch=1 source_order=G,G,C,Copy "
        "first_ready_lanes={} observed_submit_order=G,G,C,Copy "
        "copy_translate_owner=Translate native_owner=Submission "
        "async_scope={} explicit_state=true readback=verified "
        "callbacks=exactly_once replay=0",
        topology.graphics.native_queue_id,
        topology.compute.native_queue_id,
        topology.copy.native_queue_id,
        copy_has_independent_native_lane ? "G,C,Copy" : "alias_fallback",
        async_queue_scope
    );
}

void RunMultiSegmentCompletionAggregateCpuProbe() {
    const VulkanMultiSegmentCompletionProbeResult result = RunVulkanMultiSegmentCompletionProbeForTesting();
    if (!result.suffix_retirement_deferred_callbacks || !result.prefix_retirement_completed_callbacks ||
        !result.repeated_retirement_suppressed || result.ordinary_callback_count != 1 ||
        result.success_callback_count != 0 ||
        result.materialized_prefix_signal_count != 1 ||
        result.materialized_prefix_keepalive_count != 1 ||
        result.materialized_suffix_signal_count != 2 ||
        result.materialized_suffix_keepalive_count != 2 ||
        !result.materialized_signal_identity_matches ||
        !result.source_signal_remained_unrejected) {
        throw std::runtime_error("multi-segment completion aggregate CPU probe violated callback ordering");
    }

    LOG_INFO("[TESTCASE][PASS] name=MultiSegmentCompletionAggregateCpuProbe "
             "suffix_first=deferred prefix_second=ordinary1_success0 "
             "signals=1,2 keepalives=1,2 identities=owned replay=0");
}

void RunMultiSegmentSourceExecution() {
    using namespace std::chrono_literals;

    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x50483135434D554Cull;
    if (!topology.graphics.available || !topology.compute.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=MultiSegmentSourceExecution "
            "reason=queue_unavailable graphics_native={} compute_native={}",
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id
        );
        return;
    }

    const bool distinct_native = topology.graphics.native_queue_id != topology.compute.native_queue_id;
    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    FenceRef                       source_done = device.CreateFence();
    std::atomic<uint32>            ordinary_callbacks{0};
    std::atomic<uint32>            success_callbacks{0};
    std::binary_semaphore          graphics_entered{0};
    std::binary_semaphore          compute_entered{0};
    std::binary_semaphore          release_graphics{0};
    std::binary_semaphore          release_compute{0};
    OneShotSemaphoreRelease        release_graphics_guard(release_graphics);
    OneShotSemaphoreRelease        release_compute_guard(release_compute);
    std::atomic<bool>              graphics_translate_finished{false};
    std::atomic<bool>              compute_observed_graphics_finished{false};
    const uint32                   main_thread_id = Platform::GetCurrentThreadID();

    try {
        CommandList source(EQueueType::Graphics);
        source.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        source.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Graphics, &graphics_entered, &release_graphics, &graphics_translate_finished
            ),
            "MultiSegmentGraphicsTranslateProbe"
        );
        source.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Compute,
                &compute_entered,
                &release_compute,
                nullptr,
                &graphics_translate_finished,
                &compute_observed_graphics_finished
            ),
            "MultiSegmentComputeTranslateProbe"
        );
        source.AddCallback([&] {
            ordinary_callbacks.fetch_add(1, std::memory_order_relaxed);
        });
        source.AddSuccessCallback([&] {
            success_callbacks.fetch_add(1, std::memory_order_relaxed);
        });

        CmdSubmit submit = source.Submit();
        submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
        submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        submit.async_queue_scope = async_queue_scope;
        submit.segments          = {
            RHISubmitSegment{EQueueType::Graphics, 0, 1},
            RHISubmitSegment{EQueueType::Compute, 1, 2},
        };
        submit.Signal(source_done.Get(), 1);

        Array<RHIBackendSubmissionBatchEntry> batch{};
        batch.emplace_back(EQueueType::Graphics, std::move(submit));
        RHIExecutor::Get().Submit(std::move(batch), ERHIExecSubmitFlags::FlushGPU);

        if (!graphics_entered.try_acquire_for(5s)) {
            throw std::runtime_error("multi-segment Graphics Translate did not enter");
        }
        if (distinct_native) {
            if (!compute_entered.try_acquire_for(5s)) {
                throw std::runtime_error("distinct-native multi-segment Translate did not enter "
                                         "Graphics and Compute in one ready wave");
            }
        }

        release_graphics_guard.Release();
        if (!distinct_native && !compute_entered.try_acquire_for(5s)) {
            throw std::runtime_error("native-alias multi-segment Translate did not advance to "
                                     "Compute after Graphics release");
        }
        const bool compute_saw_graphics_finished =
            compute_observed_graphics_finished.load(std::memory_order_acquire);
        if (!distinct_native && !compute_saw_graphics_finished) {
            throw std::runtime_error(
                "native-alias Compute Translate entered before Graphics Translate returned"
            );
        }
        release_compute_guard.Release();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (ResourceCast(source_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(source_done.Get())->IsRejected(1) || ResourceCast(source_done.Get())->IsFailed()) {
            throw std::runtime_error("multi-segment source signal did not complete successfully");
        }
        if (ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
            success_callbacks.load(std::memory_order_acquire) != 1) {
            throw std::runtime_error("multi-segment source aggregate callbacks did not retire "
                                     "exactly once");
        }

        if (source_capture.Overflowed() || source_capture.Count() != 2) {
            throw std::runtime_error("multi-segment source observer did not report exactly two "
                                     "executable segments");
        }
        constexpr std::array expected_queues{
            EQueueType::Graphics,
            EQueueType::Compute,
        };
        const uint64 batch_sequence = source_capture.Event(0).batch_sequence;
        if (batch_sequence == 0) {
            throw std::runtime_error("multi-segment source observer reported an invalid batch");
        }
        for (size_t index = 0; index < expected_queues.size(); ++index) {
            const VulkanSourceSubmissionEvent& event = source_capture.Event(index);
            if (event.batch_sequence != batch_sequence || event.source_index != index ||
                event.original_source_index != 0 || event.source_segment_index != index ||
                event.source_segment_count != 2 || event.queue != expected_queues[index] ||
                event.async_queue_scope != async_queue_scope ||
                event.cross_native_predecessor_wait != (index == 1 && distinct_native)) {
                throw std::runtime_error("multi-segment source observer lost stable source or "
                                         "segment identity");
            }
        }

        if (native_capture.Overflowed() || native_capture.Count() != 2) {
            throw std::runtime_error("multi-segment source did not issue exactly two native submits");
        }
        const uint32 submission_thread_id = native_capture.Event(0).thread_id;
        for (size_t index = 0; index < expected_queues.size(); ++index) {
            const VulkanNativeSubmissionEvent& event = native_capture.Event(index);
            if (event.queue != expected_queues[index] || event.thread_role != ERHIThreadRole::Submission ||
                event.thread_id != submission_thread_id || event.thread_id == main_thread_id ||
                !event.outcome.WasSubmitted()) {
                throw std::runtime_error("multi-segment native submission escaped stable "
                                         "Graphics-to-Compute Submission ownership");
            }
        }
        if ((native_capture.Event(0).native_queue_handle == native_capture.Event(1).native_queue_handle) !=
            !distinct_native) {
            throw std::runtime_error("multi-segment native observer disagrees with queue topology");
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (source_capture.Count() != 2 || native_capture.Count() != 2 ||
            ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
            success_callbacks.load(std::memory_order_acquire) != 1) {
            throw std::runtime_error("a second Sync replayed multi-segment submission or callbacks");
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        release_graphics_guard.Release();
        release_compute_guard.Release();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=MultiSegmentSourceExecution "
        "source=1 segments=2 queues=Graphics,Compute native_alias={} "
        "first_ready_lanes={} observed_submit_order=G,C "
        "source_segments=0:0/2,0:1/2 predecessor_waits={} "
        "compute_saw_graphics_finished={} "
        "native_owner=Submission callbacks=exactly_once signal=success replay=0",
        !distinct_native,
        distinct_native ? "G,C" : "G",
        distinct_native ? 1 : 0,
        compute_observed_graphics_finished.load(std::memory_order_acquire)
    );
}

void RunMultiSegmentCopyRoundTrip() {
    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x50483135434F5059ull;
    if (!topology.graphics.available || !topology.copy.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=MultiSegmentCopyRoundTrip "
            "reason=copy_queue_unavailable graphics_native={} copy_native={}",
            topology.graphics.native_queue_id,
            topology.copy.native_queue_id
        );
        return;
    }

    constexpr size_t        element_count = 32;
    const EBufferUsageFlags usage         = EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef source     = device.CreateBuffer<uint32>("multi_segment_copy_source", element_count, usage);
    BufferRef copy_stage = device.CreateBuffer<uint32>("multi_segment_copy_stage", element_count, usage);
    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        expected[index] = 0x15C00000u + index * 59u + 11u;
    }

    const bool graphics_copy_alias = topology.graphics.native_queue_id == topology.copy.native_queue_id;
    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    FenceRef                       source_done = device.CreateFence();
    std::atomic<uint32>            ordinary_callbacks{0};
    std::atomic<uint32>            success_callbacks{0};
    const uint32                   main_thread_id = Platform::GetCurrentThreadID();

    CommandList source_commands(EQueueType::Graphics);
    source_commands.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    source_commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);

    source_commands.CopyFrom(OwnedBytes(expected), source->GetView(), "MultiSegmentCopyUpload");
    source_commands.ExportResourcesToQueue(
        EQueueType::Copy, {}, Array<ExportBuffer>{{source->GetView(), EBufferState::TRANSFER}}
    );

    source_commands.ImportResourcesFromQueue(
        EQueueType::Graphics, {}, Array<ImportBuffer>{{source->GetView(), EBufferState::TRANSFER}}
    );
    source_commands.CopyFrom(source->GetView(), copy_stage->GetView(), "MultiSegmentCopyNativeCopy");
    source_commands.ExportResourcesToQueue(
        EQueueType::Graphics, {}, Array<ExportBuffer>{{copy_stage->GetView(), EBufferState::TRANSFER}}
    );

    source_commands.ImportResourcesFromQueue(
        EQueueType::Copy, {}, Array<ImportBuffer>{{copy_stage->GetView(), EBufferState::TRANSFER}}
    );
    source_commands.CopyFrom(copy_stage->GetView(), WritableBytes(readback), "MultiSegmentCopyReadback");
    source_commands.AddCallback([&] {
        ordinary_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    source_commands.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    CmdSubmit submit = source_commands.Submit();
    submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    submit.async_queue_scope = async_queue_scope;
    submit.segments          = {
        RHISubmitSegment{EQueueType::Graphics, 0, 2},
        RHISubmitSegment{EQueueType::Copy, 2, 5},
        RHISubmitSegment{EQueueType::Graphics, 5, 7},
    };
    submit.Signal(source_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(EQueueType::Graphics, std::move(submit));
    RHIExecutor::Get().Submit(std::move(batch), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (readback != expected) {
        throw std::runtime_error("multi-segment Copy source GPU readback mismatch");
    }
    if (ResourceCast(source_done.Get())->HostWait(1) != VK_SUCCESS ||
        ResourceCast(source_done.Get())->IsRejected(1) || ResourceCast(source_done.Get())->IsFailed()) {
        throw std::runtime_error("multi-segment Copy source signal did not complete successfully");
    }
    if (ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error("multi-segment Copy source callbacks did not retire exactly once");
    }

    constexpr std::array expected_queues{
        EQueueType::Graphics,
        EQueueType::Copy,
        EQueueType::Graphics,
    };
    if (source_capture.Overflowed() || source_capture.Count() != expected_queues.size()) {
        throw std::runtime_error("multi-segment Copy source observer did not report three segments");
    }
    const uint64 batch_sequence = source_capture.Event(0).batch_sequence;
    if (batch_sequence == 0) {
        throw std::runtime_error("multi-segment Copy source observer reported an invalid batch");
    }
    for (size_t index = 0; index < expected_queues.size(); ++index) {
        const VulkanSourceSubmissionEvent& event                     = source_capture.Event(index);
        const bool                         expected_predecessor_wait = index != 0 && !graphics_copy_alias;
        if (event.batch_sequence != batch_sequence || event.source_index != index ||
            event.original_source_index != 0 || event.source_segment_index != index ||
            event.source_segment_count != expected_queues.size() || event.queue != expected_queues[index] ||
            event.async_queue_scope != async_queue_scope ||
            event.cross_native_predecessor_wait != expected_predecessor_wait) {
            throw std::runtime_error(
                "multi-segment Copy source observer lost stable segment identity or dependency"
            );
        }
    }

    if (native_capture.Overflowed() || native_capture.Count() != expected_queues.size()) {
        throw std::runtime_error("multi-segment Copy source did not issue three native submits");
    }
    const uint32 submission_thread_id = native_capture.Event(0).thread_id;
    for (size_t index = 0; index < expected_queues.size(); ++index) {
        const VulkanNativeSubmissionEvent& event = native_capture.Event(index);
        if (event.queue != expected_queues[index] || event.thread_role != ERHIThreadRole::Submission ||
            event.thread_id != submission_thread_id || event.thread_id == main_thread_id ||
            !event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "multi-segment Copy native submits lost stable Submission order or ownership"
            );
        }
    }
    if ((native_capture.Event(0).native_queue_handle == native_capture.Event(1).native_queue_handle) !=
            graphics_copy_alias ||
        native_capture.Event(0).native_queue_handle != native_capture.Event(2).native_queue_handle) {
        throw std::runtime_error("multi-segment Copy native observer disagrees with queue topology");
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (source_capture.Count() != expected_queues.size() ||
        native_capture.Count() != expected_queues.size() ||
        ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error("a second Sync replayed multi-segment Copy work or callbacks");
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=MultiSegmentCopyRoundTrip "
        "source=1 segments=3 queues=Graphics,Copy,Graphics native_alias={} "
        "observed_submit_order=G,Copy,G source_segments=0:0/3,0:1/3,0:2/3 "
        "predecessor_waits={} native_owner=Submission callbacks=exactly_once "
        "signal=success readback=verified replay=0",
        graphics_copy_alias,
        graphics_copy_alias ? 0 : 2
    );
}

void RunMultiSegmentRecoverableRejection() {
    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x504831354D52454Aull;
    if (!topology.graphics.available || !topology.compute.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=MultiSegmentRecoverableRejection "
            "reason=queue_unavailable graphics_native={} compute_native={}",
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id
        );
        return;
    }

    const bool distinct_native     = topology.graphics.native_queue_id != topology.compute.native_queue_id;
    FenceRef   rejected_dependency = device.CreateFence();
    FenceRef   rejected_done       = device.CreateFence();
    FenceRef   recovered_done      = device.CreateFence();
    rejected_dependency->Reject(1);

    std::atomic<uint32>            rejected_callbacks{0};
    std::atomic<uint32>            rejected_success_callbacks{0};
    std::atomic<uint32>            recovered_callbacks{0};
    std::atomic<uint32>            recovered_success_callbacks{0};
    std::binary_semaphore          graphics_translated{0};
    std::binary_semaphore          compute_translated{0};
    std::binary_semaphore          recovered_translated{0};
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    const uint32                   main_thread_id = Platform::GetCurrentThreadID();

    CommandList rejected(EQueueType::Graphics);
    rejected.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    rejected.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&graphics_translated, EQueueType::Graphics),
        "MultiSegmentRejectedGraphicsTranslateProbe"
    );
    rejected.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&compute_translated, EQueueType::Compute),
        "MultiSegmentRejectedComputeTranslateProbe"
    );
    rejected.AddCallback([&] {
        rejected_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    rejected.AddSuccessCallback([&] {
        rejected_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    CmdSubmit rejected_submit = rejected.Submit();
    rejected_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    rejected_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    rejected_submit.async_queue_scope = async_queue_scope;
    rejected_submit.segments          = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Compute, 1, 2},
    };
    rejected_submit.Wait(rejected_dependency.Get(), 1);
    rejected_submit.Signal(rejected_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> rejected_batch{};
    rejected_batch.emplace_back(EQueueType::Graphics, std::move(rejected_submit));
    RHIExecutor::Get().Submit(std::move(rejected_batch), ERHIExecSubmitFlags::FlushGPU);
    if (ResourceCast(rejected_done.Get())->HostWait(1) != VK_ERROR_UNKNOWN ||
        !ResourceCast(rejected_done.Get())->IsRejected(1) || ResourceCast(rejected_done.Get())->IsFailed()) {
        throw std::runtime_error("multi-segment rejected dependency did not reject the source signal");
    }

    const bool graphics_was_translated = graphics_translated.try_acquire();
    const bool compute_was_translated  = compute_translated.try_acquire();
    if (!graphics_was_translated || compute_was_translated != distinct_native) {
        throw std::runtime_error("multi-segment recoverable rejection did not preserve "
                                 "native-lane Translate wave semantics");
    }

    CommandList recovered(EQueueType::Graphics);
    recovered.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    recovered.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&recovered_translated, EQueueType::Graphics),
        "MultiSegmentRejectionRecoveryProbe"
    );
    recovered.AddCallback([&] {
        recovered_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    recovered.AddSuccessCallback([&] {
        recovered_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit recovered_submit = recovered.Submit();
    recovered_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    recovered_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    recovered_submit.async_queue_scope = async_queue_scope;
    recovered_submit.Signal(recovered_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics, std::move(recovered_submit), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!recovered_translated.try_acquire() ||
        ResourceCast(recovered_done.Get())->HostWait(1) != VK_SUCCESS ||
        ResourceCast(recovered_done.Get())->IsRejected(1) || ResourceCast(recovered_done.Get())->IsFailed()) {
        throw std::runtime_error("multi-segment dependency rejection latched the runtime");
    }
    if (rejected_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_success_callbacks.load(std::memory_order_acquire) != 0 ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error("multi-segment aggregate callback retirement was not exactly once");
    }
    if (native_capture.Overflowed() || native_capture.Count() != 1) {
        throw std::runtime_error("rejected multi-segment source reached native submit or recovery "
                                 "did not submit exactly once");
    }
    const VulkanNativeSubmissionEvent& recovered_event = native_capture.Event(0);
    if (recovered_event.queue != EQueueType::Graphics ||
        recovered_event.thread_role != ERHIThreadRole::Submission ||
        recovered_event.thread_id == main_thread_id || !recovered_event.outcome.WasSubmitted()) {
        throw std::runtime_error("multi-segment rejection recovery escaped Submission ownership");
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 1 || rejected_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_success_callbacks.load(std::memory_order_acquire) != 0 ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error("a second Sync replayed multi-segment rejection retirement");
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=MultiSegmentRecoverableRejection "
        "source=1 segments=2 dependency=rejected distinct_native={} "
        "suffix_recorded={} native_rejected=0 "
        "callbacks=ordinary1_success0 signal=rejected "
        "native_accepted=1 recovered_callbacks=ordinary1_success1 "
        "runtime=recovered replay=0",
        distinct_native,
        compute_was_translated
    );
}

void RunMultiSegmentPrefixSubmitSuffixTranslateFailure() {
    auto&            device            = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x50483135434D4844ull;

    FenceRef                       source_done = device.CreateFence();
    FenceRef                       later_done  = device.CreateFence();
    std::atomic<uint32>            source_callbacks{0};
    std::atomic<uint32>            source_success_callbacks{0};
    std::atomic<uint32>            source_completion_callbacks{0};
    std::atomic<uint32>            later_callbacks{0};
    std::atomic<uint32>            later_success_callbacks{0};
    std::atomic<uint32>            later_completion_callbacks{0};
    std::atomic<uint32>            completion_callback_errors{0};
    std::binary_semaphore          prefix_translated{0};
    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    const uint32                   main_thread_id = Platform::GetCurrentThreadID();

    CommandList source(EQueueType::Graphics);
    source.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    GpuCompletionFuture source_completion =
        source.TrackGpuCompletion(
            "Phase19AMultiSegmentTailCompletion"
        );
    source.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&prefix_translated, EQueueType::Graphics),
        "MultiSegmentHardFailureAcceptedPrefix"
    );
    source.AddCustomCommand(
        MakeUnique<ThrowingTranslateProbeCommand>(true), "MultiSegmentHardFailureThrowingSuffix"
    );
    source_completion.Then(
        [&](const GpuCompletionResult& _result) {
            if (_result.status != GpuCompletionStatus::Error ||
                _result.error_reason.empty() ||
                GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                !ResourceCast(source_done.Get())->IsFailed()) {
                completion_callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            source_completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );
    source.AddCallback([&] {
        if (source_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1) {
            completion_callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        source_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    source.AddSuccessCallback([&] {
        source_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    CmdSubmit submit = source.Submit();
    submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    submit.async_queue_scope = async_queue_scope;
    submit.segments          = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    submit.Signal(source_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(EQueueType::Graphics, std::move(submit));
    RHIExecutor::Get().Submit(std::move(batch), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const GpuCompletionResult source_completion_result =
        source_completion.Get();
    if (!prefix_translated.try_acquire()) {
        throw std::runtime_error("multi-segment hard failure did not Translate the prefix");
    }
    if (!ResourceCast(source_done.Get())->IsFailed() || ResourceCast(source_done.Get())->IsRejected(1)) {
        throw std::runtime_error("multi-segment hard failure did not fail the tail external signal");
    }
    if (source_callbacks.load(std::memory_order_acquire) != 1 ||
        source_success_callbacks.load(std::memory_order_acquire) != 0 ||
        source_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        source_completion_result.status !=
            GpuCompletionStatus::Error ||
        source_completion_result.error_reason.empty() ||
        completion_callback_errors.load(
            std::memory_order_acquire
        ) != 0) {
        throw std::runtime_error("multi-segment hard failure did not aggregate source callbacks exactly once"
        );
    }
    if (source_capture.Overflowed() || source_capture.Count() != 1) {
        throw std::runtime_error(
            "multi-segment hard failure did not publish exactly one accepted prefix source"
        );
    }
    const VulkanSourceSubmissionEvent& source_event = source_capture.Event(0);
    if (source_event.original_source_index != 0 || source_event.source_segment_index != 0 ||
        source_event.source_segment_count != 2 || source_event.queue != EQueueType::Graphics) {
        throw std::runtime_error("multi-segment hard failure lost accepted prefix source identity");
    }
    if (native_capture.Overflowed() || native_capture.Count() != 1) {
        throw std::runtime_error("multi-segment hard failure did not native-submit exactly one prefix");
    }
    const VulkanNativeSubmissionEvent& native_event = native_capture.Event(0);
    if (native_event.queue != EQueueType::Graphics ||
        native_event.thread_role != ERHIThreadRole::Submission || native_event.thread_id == main_thread_id ||
        !native_event.outcome.WasSubmitted()) {
        throw std::runtime_error("multi-segment hard-failure prefix escaped native Submission ownership");
    }

    CommandList later(EQueueType::Graphics);
    GpuCompletionFuture later_completion =
        later.TrackGpuCompletion(
            "Phase19AHardLatchCompletion"
        );
    later_completion.Then(
        [&](const GpuCompletionResult& _result) {
            if (_result.status != GpuCompletionStatus::Error ||
                GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion) {
                completion_callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            later_completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );
    later.AddCallback([&] {
        if (later_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1) {
            completion_callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        later_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    later.AddSuccessCallback([&] {
        later_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit later_submit = later.Submit();
    later_submit.Signal(later_done.Get(), 1);
    RHIExecutor::Get().Submit(EQueueType::Graphics, std::move(later_submit), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const GpuCompletionResult later_completion_result =
        later_completion.Get();

    if (!ResourceCast(later_done.Get())->IsFailed() || ResourceCast(later_done.Get())->IsRejected(1)) {
        throw std::runtime_error("multi-segment hard failure did not latch a later batch");
    }
    if (later_callbacks.load(std::memory_order_acquire) != 1 ||
        later_success_callbacks.load(std::memory_order_acquire) != 0 ||
        later_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        later_completion_result.status !=
            GpuCompletionStatus::Error ||
        completion_callback_errors.load(
            std::memory_order_acquire
        ) != 0) {
        throw std::runtime_error("multi-segment hard latch did not retire later callbacks exactly once");
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 1 || source_capture.Count() != 1 ||
        source_callbacks.load(std::memory_order_acquire) != 1 ||
        source_success_callbacks.load(std::memory_order_acquire) != 0 ||
        source_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        later_callbacks.load(std::memory_order_acquire) != 1 ||
        later_success_callbacks.load(std::memory_order_acquire) != 0 ||
        later_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1) {
        throw std::runtime_error("multi-segment hard failure replayed submission or callback retirement");
    }

    LOG_INFO("[TESTCASE][PASS] name=MultiSegmentPrefixSubmitSuffixTranslateFailure "
             "source=1 segments=2 queues=Graphics,Graphics "
             "prefix_translated=true source_submitted=0:0/2 "
             "native_accepted_prefix=1 native_owner=Submission "
             "completion=Error callbacks=ordinary1_success0 signal=failed "
             "later_callbacks=ordinary1_success0 hard_latch=later_batch_failed replay=0");
}

void RunParallelTranslateFailureRetirement() {
    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x5048313545464149ull;

    FenceRef                           graphics_done = device.CreateFence();
    FenceRef                           copy_done     = device.CreateFence();
    FenceRef                           later_done    = device.CreateFence();
    std::array<std::atomic<uint32>, 3> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 3> success_callbacks{};
    SourceSubmissionCapture            source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver     source_observer(source_capture);
    SourceTranslationCapture           translation_capture(
        async_queue_scope, EQueueType::Copy
    );
    ScopedSourceTranslationObserver translation_observer(
        translation_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    constexpr std::array<uint32, 4> copy_values{
        0x15E30001u,
        0x15E30002u,
        0x15E30003u,
        0x15E30004u,
    };
    BufferRef copy_destination = device.CreateBuffer<uint32>(
        "phase15e_hard_failure_copy_suffix",
        copy_values.size(),
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );

    CommandList graphics(EQueueType::Graphics);
    graphics.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    graphics.AddCustomCommand(
        MakeUnique<ThrowingTranslateProbeCommand>(true), "InjectedOuterTranslateFailure"
    );
    graphics.AddCallback([&] {
        ordinary_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });
    graphics.AddSuccessCallback([&] {
        success_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    graphics_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    graphics_submit.async_queue_scope = async_queue_scope;
    graphics_submit.Signal(graphics_done.Get(), 1);

    CommandList copy(EQueueType::Copy);
    copy.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    copy.CopyFrom(
        OwnedBytes(copy_values),
        copy_destination->GetView(),
        "ParallelTranslateRecordedCopySuffix"
    );
    copy.AddCallback([&] {
        ordinary_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });
    copy.AddSuccessCallback([&] {
        success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit copy_submit = copy.Submit();
    copy_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    copy_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    copy_submit.async_queue_scope = async_queue_scope;
    copy_submit.Signal(copy_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> failing_batch{};
    failing_batch.emplace_back(EQueueType::Graphics, std::move(graphics_submit));
    failing_batch.emplace_back(EQueueType::Copy, std::move(copy_submit));
    RHIExecutor::Get().Submit(std::move(failing_batch), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const bool distinct_native =
        topology.graphics.native_queue_id !=
        topology.copy.native_queue_id;
    const bool suffix_was_translated =
        translation_capture.Seen(
            1,
            EVulkanSourceTranslationPhase::Recorded
        );
    if (suffix_was_translated != distinct_native) {
        throw std::runtime_error("parallel Translate failure did not preserve native-lane wave "
                                 "dispatch semantics");
    }
    if (!translation_capture.IsValid()) {
        throw std::runtime_error(
            "hard-failure Copy translation observer lost ownership or "
            "published duplicate phases"
        );
    }
    if (!ResourceCast(graphics_done.Get())->IsFailed() ||
        !ResourceCast(copy_done.Get())->IsFailed() ||
        ResourceCast(graphics_done.Get())->IsRejected(1) ||
        ResourceCast(copy_done.Get())->IsRejected(1)) {
        throw std::runtime_error("hard parallel Translate failure did not fail every wave signal");
    }
    if (source_capture.Count() != 0 ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "hard-failure recorded Copy suffix reached source or native "
            "submission"
        );
    }

    // A hard Translate failure latches the backend. A later independent
    // source must be terminalized without entering Translate or replaying any
    // earlier packet ownership.
    CommandList later(EQueueType::Graphics);
    later.AddCallback([&] {
        ordinary_callbacks[2].fetch_add(1, std::memory_order_relaxed);
    });
    later.AddSuccessCallback([&] {
        success_callbacks[2].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit later_submit = later.Submit();
    later_submit.Signal(later_done.Get(), 1);
    RHIExecutor::Get().Submit(EQueueType::Graphics, std::move(later_submit), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!ResourceCast(later_done.Get())->IsFailed() || ResourceCast(later_done.Get())->IsRejected(1)) {
        throw std::runtime_error("hard Translate failure did not fail a later batch");
    }
    for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 0) {
            throw std::runtime_error("hard Translate failure did not terminalize callbacks "
                                     "exactly once");
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 0) {
            throw std::runtime_error("hard Translate failure replayed callback retirement");
        }
    }
    if (source_capture.Count() != 0 ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "hard Translate failure replayed a rejected Copy packet"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
        "distinct_native={} suffix_recorded={} signals=failed "
        "suffix_queue=Copy native_submit=0 callbacks=exactly_once "
        "hard_latch=later_batch_failed replay=0",
        distinct_native,
        suffix_was_translated
    );
}

void RunParallelTranslateRecoverableRejection() {
    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x5048313541524543ull;

    FenceRef                           rejected_dependency = device.CreateFence();
    FenceRef                           graphics_done       = device.CreateFence();
    FenceRef                           compute_done        = device.CreateFence();
    FenceRef                           recovered_done      = device.CreateFence();
    std::array<std::atomic<uint32>, 3> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 3> success_callbacks{};
    std::binary_semaphore              graphics_translated{0};
    std::binary_semaphore              compute_translated{0};
    std::binary_semaphore              recovered_translated{0};

    rejected_dependency->Reject(1);

    CommandList graphics(EQueueType::Graphics);
    graphics.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    graphics.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&graphics_translated), "RecoverableGraphicsTranslateProbe"
    );
    graphics.AddCallback([&] {
        ordinary_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });
    graphics.AddSuccessCallback([&] {
        success_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    graphics_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    graphics_submit.async_queue_scope = async_queue_scope;
    graphics_submit.Wait(rejected_dependency.Get(), 1);
    graphics_submit.Signal(graphics_done.Get(), 1);

    CommandList compute(EQueueType::Compute);
    compute.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    compute.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&compute_translated), "RecoverableComputeRecordedSuffixProbe"
    );
    compute.AddCallback([&] {
        ordinary_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });
    compute.AddSuccessCallback([&] {
        success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit compute_submit = compute.Submit();
    compute_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    compute_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    compute_submit.async_queue_scope = async_queue_scope;
    compute_submit.Signal(compute_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> rejected_batch{};
    rejected_batch.emplace_back(EQueueType::Graphics, std::move(graphics_submit));
    rejected_batch.emplace_back(EQueueType::Compute, std::move(compute_submit));
    RHIExecutor::Get().Submit(std::move(rejected_batch), ERHIExecSubmitFlags::FlushGPU);

    // Wait for exact Submission-side publication without calling Sync:
    // ProcessSync itself clears async-scope admission state and would hide a
    // stale seen-lane bug between two backend batches reusing this scope.
    if (ResourceCast(graphics_done.Get())->HostWait(1) != VK_ERROR_UNKNOWN ||
        ResourceCast(compute_done.Get())->HostWait(1) != VK_ERROR_UNKNOWN) {
        throw std::runtime_error("recoverable parallel Translate signals were not published");
    }

    const bool distinct_native = topology.graphics.native_queue_id != topology.compute.native_queue_id;
    if (!graphics_translated.try_acquire() || compute_translated.try_acquire() != distinct_native) {
        throw std::runtime_error("recoverable parallel Translate wave did not preserve native-lane "
                                 "dispatch semantics");
    }
    if (!ResourceCast(graphics_done.Get())->IsRejected(1) ||
        !ResourceCast(compute_done.Get())->IsRejected(1) || ResourceCast(graphics_done.Get())->IsFailed() ||
        ResourceCast(compute_done.Get())->IsFailed()) {
        throw std::runtime_error("recoverable parallel Translate failure did not reject exact "
                                 "signal values");
    }

    // The rejected dependency is packet-local. A later independent source
    // must still enter Translate, submit, and complete successfully.
    CommandList recovered(EQueueType::Compute);
    recovered.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    recovered.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&recovered_translated), "ParallelTranslateRecoveryProbe"
    );
    recovered.AddCallback([&] {
        ordinary_callbacks[2].fetch_add(1, std::memory_order_relaxed);
    });
    recovered.AddSuccessCallback([&] {
        success_callbacks[2].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit recovered_submit = recovered.Submit();
    recovered_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    recovered_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    recovered_submit.async_queue_scope = async_queue_scope;
    recovered_submit.Signal(recovered_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Compute, std::move(recovered_submit), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!recovered_translated.try_acquire() ||
        ResourceCast(recovered_done.Get())->HostWait(1) != VK_SUCCESS ||
        ResourceCast(recovered_done.Get())->IsRejected(1) || ResourceCast(recovered_done.Get())->IsFailed()) {
        throw std::runtime_error("recoverable parallel Translate rejection latched the runtime");
    }
    for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
        const uint32 expected_success = index == 2 ? 1u : 0u;
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != expected_success) {
            throw std::runtime_error("recoverable parallel Translate callbacks retired incorrectly");
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
        const uint32 expected_success = index == 2 ? 1u : 0u;
        if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != expected_success) {
            throw std::runtime_error("recoverable parallel Translate retirement replayed callbacks");
        }
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=ParallelTranslateRecoverableRejection "
        "distinct_native={} suffix_recorded={} signals=rejected "
        "callbacks=exactly_once runtime=recovered "
        "same_scope_reentry=Compute",
        distinct_native,
        distinct_native
    );
}

void RunRaytracingExportAcceptanceRejection() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();

    using Outcome = EExportSubmissionOutcome;

    const auto source_failed = ExportSubmissionTransaction::ResolveFrameDecision(
        true,
        Outcome::Failed,
        Outcome::NotSubmitted,
        Outcome::Accepted,
        false,
        true,
        false
    );
    const auto tail_independently_rejected =
        ExportSubmissionTransaction::ResolveFrameDecision(
            true,
            Outcome::Accepted,
            Outcome::Accepted,
            Outcome::Rejected,
            true,
            true,
            false
        );
    const auto prefix_tail_rejected_with_independent_failure =
        ExportSubmissionTransaction::ResolveFrameDecision(
            true,
            Outcome::Rejected,
            Outcome::NotSubmitted,
            Outcome::Rejected,
            false,
            true,
            true
        );
    const auto prefix_rejected_tail_accepted =
        ExportSubmissionTransaction::ResolveFrameDecision(
            true,
            Outcome::Rejected,
            Outcome::NotSubmitted,
            Outcome::Accepted,
            false,
            true,
            false
        );
    const auto readback_hard_failed =
        ExportSubmissionTransaction::ResolveFrameDecision(
            true,
            Outcome::Accepted,
            Outcome::Failed,
            Outcome::Accepted,
            false,
            true,
            false
        );
    if (source_failed.frame_accepted ||
        source_failed.retryable_frame_rejection ||
        !source_failed.retry_requested || !source_failed.latch_renderer ||
        tail_independently_rejected.frame_accepted ||
        tail_independently_rejected.retry_requested ||
        tail_independently_rejected.retryable_frame_rejection ||
        !tail_independently_rejected.export_consumed ||
        !tail_independently_rejected.latch_renderer ||
        prefix_tail_rejected_with_independent_failure.frame_accepted ||
        prefix_tail_rejected_with_independent_failure.retryable_frame_rejection ||
        !prefix_tail_rejected_with_independent_failure.retry_requested ||
        !prefix_tail_rejected_with_independent_failure.independent_source_failed ||
        !prefix_tail_rejected_with_independent_failure.latch_renderer ||
        prefix_rejected_tail_accepted.frame_accepted ||
        prefix_rejected_tail_accepted.retryable_frame_rejection ||
        !prefix_rejected_tail_accepted.retry_requested ||
        !prefix_rejected_tail_accepted.latch_renderer ||
        readback_hard_failed.frame_accepted ||
        readback_hard_failed.export_consumed ||
        !readback_hard_failed.retry_requested ||
        !readback_hard_failed.independent_source_failed ||
        readback_hard_failed.retryable_frame_rejection ||
        !readback_hard_failed.latch_renderer) {
        throw std::runtime_error(
            "raytracing export production decision table misclassified a "
            "failed source/readback, independent tail, or inconsistent "
            "prefix/tail"
        );
    }

    FenceRef rejected_prefix_dependency   = device.CreateFence();
    FenceRef rejected_readback_dependency = device.CreateFence();
    rejected_prefix_dependency->Reject(1);
    rejected_readback_dependency->Reject(1);

    ExportSubmissionTransaction attempt1_submission(device.CreateFence());
    ExportSubmissionTransaction readback_retry_submission(device.CreateFence());
    ExportSubmissionTransaction recovery_submission(device.CreateFence());
    FenceRef attempt1_tail_receipt       = device.CreateFence();
    FenceRef readback_retry_tail_receipt = device.CreateFence();
    FenceRef recovery_tail_receipt       = device.CreateFence();

    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;
    BufferRef attempt1_source_buffer = device.CreateBuffer<uint32>(
        "rt_export_attempt1_source", 4, usage
    );
    BufferRef attempt1_tail_target = device.CreateBuffer<uint32>(
        "rt_export_attempt1_tail", 4, usage
    );
    BufferRef readback_retry_source = device.CreateBuffer<uint32>(
        "rt_export_readback_retry_source", 4, usage
    );
    BufferRef readback_retry_tail_target = device.CreateBuffer<uint32>(
        "rt_export_readback_retry_tail", 4, usage
    );
    BufferRef recovery_source = device.CreateBuffer<uint32>(
        "rt_export_recovery_source", 4, usage
    );
    BufferRef recovery_tail_target = device.CreateBuffer<uint32>(
        "rt_export_recovery_tail", 4, usage
    );

    std::atomic<uint32> blocker_callbacks{0};
    std::array<std::atomic<uint32>, 2> attempt1_callbacks{};
    std::array<std::atomic<uint32>, 2> attempt1_success_callbacks{};
    std::array<std::atomic<uint32>, 3> readback_retry_callbacks{};
    std::array<std::atomic<uint32>, 3> readback_retry_success_callbacks{};
    std::array<std::atomic<uint32>, 3> recovery_callbacks{};
    std::array<std::atomic<uint32>, 3> recovery_success_callbacks{};
    std::atomic<uint32> encoder_dispatches{0};
    const std::array<uint32, 4> readback_retry_expected{41, 43, 47, 53};
    const std::array<uint32, 4> recovery_expected{59, 61, 67, 71};
    std::binary_semaphore blocker_entered{0};
    std::binary_semaphore release_blocker{0};
    OneShotSemaphoreRelease release_guard(release_blocker);

    // Hold Completion so the test can observe that both raw CmdSubmit fence
    // identities still have callback-owned strong references after Submission
    // has published terminal rejection.
    CommandList blocker(EQueueType::Graphics);
    blocker.AddCallback([&] {
        blocker_callbacks.fetch_add(1, std::memory_order_relaxed);
        blocker_entered.release();
        release_blocker.acquire();
    });
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        blocker.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    if (!blocker_entered.try_acquire_for(5s)) {
        throw std::runtime_error(
            "raytracing export acceptance test could not block Completion"
        );
    }

    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    try {
        CommandList attempt1_source(EQueueType::Graphics);
        attempt1_source.CopyFrom(
            OwnedBytes(std::array<uint32, 4>{11, 13, 17, 19}),
            attempt1_source_buffer->GetView(),
            "RaytracingExportAttempt1Source"
        );
        attempt1_source.AddCallback([&] {
            attempt1_callbacks[0].fetch_add(1, std::memory_order_relaxed);
        });
        attempt1_source.AddSuccessCallback([&] {
            attempt1_success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit attempt1_source_submit = attempt1_source.Submit();
        attempt1_source_submit.Wait(rejected_prefix_dependency.Get(), 1);
        attempt1_submission.AttachSignal(attempt1_source_submit);
        if (attempt1_source_submit.signal_rejection_keepalives.size() != 1 ||
            attempt1_source_submit.signal_rejection_keepalives.front().Get() !=
                attempt1_submission.GetReceiptFence()) {
            throw std::runtime_error(
                "raytracing export receipt signal did not install an "
                "explicit rejection keepalive"
            );
        }
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(attempt1_source_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        CommandList attempt1_tail(EQueueType::Graphics);
        attempt1_tail.CopyFrom(
            OwnedBytes(std::array<uint32, 4>{23, 29, 31, 37}),
            attempt1_tail_target->GetView(),
            "RaytracingExportAttempt1DependentTail"
        );
        attempt1_tail.AddCallback([&] {
            attempt1_callbacks[1].fetch_add(1, std::memory_order_relaxed);
        });
        attempt1_tail.AddSuccessCallback([&] {
            attempt1_success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit attempt1_tail_submit = attempt1_tail.Submit();
        attempt1_submission.AttachDependentWait(attempt1_tail_submit);
        attempt1_tail_submit.Signal(attempt1_tail_receipt.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(attempt1_tail_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        const Outcome attempt1_tail_outcome =
            ExportSubmissionTransaction::ResolveReceiptOutcome(
                attempt1_tail_receipt.Get(), 1
            );
        const auto attempt1_decision =
            attempt1_submission.ClassifyFrameAcceptance(
                attempt1_tail_outcome,
                false,
                false
            );
        if (attempt1_decision.source_outcome != Outcome::Rejected ||
            attempt1_decision.tail_outcome != Outcome::Rejected ||
            attempt1_decision.frame_accepted ||
            attempt1_decision.export_consumed ||
            !attempt1_decision.retry_requested ||
            !attempt1_decision.retryable_frame_rejection ||
            attempt1_decision.latch_renderer ||
            attempt1_decision.readback_attempted ||
            !ResourceCast(attempt1_submission.GetReceiptFence())
                 ->IsRejected(attempt1_submission.GetReceiptValue()) ||
            !ResourceCast(attempt1_tail_receipt.Get())->IsRejected(1)) {
            throw std::runtime_error(
                "production export decision did not keep the rejected prefix "
                "and dependent tail retryable"
            );
        }
        if (attempt1_callbacks[0].load(std::memory_order_acquire) != 0 ||
            attempt1_callbacks[1].load(std::memory_order_acquire) != 0 ||
            attempt1_submission.GetReceiptFence()->GetRefCount() < 3) {
            throw std::runtime_error(
                "raytracing export receipt keepalive retired before terminal "
                "Completion callbacks"
            );
        }
        if (native_capture.Overflowed() || native_capture.Count() != 0) {
            throw std::runtime_error(
                "rejected raytracing export transaction reached native submit"
            );
        }

        release_guard.Release();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (blocker_callbacks.load(std::memory_order_acquire) != 1 ||
            attempt1_callbacks[0].load(std::memory_order_acquire) != 1 ||
            attempt1_callbacks[1].load(std::memory_order_acquire) != 1 ||
            attempt1_success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            attempt1_success_callbacks[1].load(std::memory_order_acquire) != 0 ||
            attempt1_submission.GetReceiptFence()->GetRefCount() != 1) {
            throw std::runtime_error(
                "raytracing export rejection did not retire callbacks and "
                "keepalives exactly once"
            );
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (attempt1_callbacks[0].load(std::memory_order_acquire) != 1 ||
            attempt1_callbacks[1].load(std::memory_order_acquire) != 1 ||
            attempt1_submission.GetReceiptFence()->GetRefCount() != 1 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "a second Sync replayed rejected raytracing export work"
            );
        }

        CommandList readback_retry_prefix(EQueueType::Graphics);
        readback_retry_prefix.CopyFrom(
            OwnedBytes(readback_retry_expected),
            readback_retry_source->GetView(),
            "RaytracingExportReadbackRetryPrefix"
        );
        readback_retry_prefix.AddCallback([&] {
            readback_retry_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        readback_retry_prefix.AddSuccessCallback([&] {
            readback_retry_success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit readback_retry_prefix_submit =
            readback_retry_prefix.Submit();
        readback_retry_submission.AttachSignal(
            readback_retry_prefix_submit
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(readback_retry_prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (readback_retry_submission.SourceOutcome() != Outcome::Accepted) {
            throw std::runtime_error(
                "raytracing readback-retry prefix was not accepted"
            );
        }

        readback_retry_submission.BeginReadback(device.CreateFence());
        CommandList rejected_readback_commands(EQueueType::Graphics);
        ReadbackFuture rejected_readback =
            rejected_readback_commands.Readback(
            readback_retry_source->GetView(),
            "RaytracingExportRejectedReadback"
        );
        rejected_readback_commands.AddCallback([&] {
            readback_retry_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        rejected_readback_commands.AddSuccessCallback([&] {
            readback_retry_success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit rejected_readback_submit =
            rejected_readback_commands.Submit();
        rejected_readback_submit.Wait(
            rejected_readback_dependency.Get(), 1
        );
        readback_retry_submission.AttachReadbackSignal(
            rejected_readback_submit
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(rejected_readback_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const ReadbackResult rejected_readback_result =
            rejected_readback.Get();
        if (readback_retry_submission.ResolveReadbackAcceptance() ||
            readback_retry_submission.ReadbackOutcome() != Outcome::Rejected ||
            rejected_readback_result.status != ReadbackStatus::Error ||
            !rejected_readback_result.Bytes().empty() ||
            readback_retry_submission.DispatchEncoder([&] {
                encoder_dispatches.fetch_add(
                    1, std::memory_order_relaxed
                );
            })) {
            throw std::runtime_error(
                "rejected production readback dispatched the export encoder"
            );
        }

        CommandList readback_retry_tail(EQueueType::Graphics);
        readback_retry_tail.CopyFrom(
            OwnedBytes(std::array<uint32, 4>{73, 79, 83, 89}),
            readback_retry_tail_target->GetView(),
            "RaytracingExportReadbackRetryTail"
        );
        readback_retry_tail.AddCallback([&] {
            readback_retry_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        readback_retry_tail.AddSuccessCallback([&] {
            readback_retry_success_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit readback_retry_tail_submit =
            readback_retry_tail.Submit();
        readback_retry_submission.AttachDependentWait(
            readback_retry_tail_submit
        );
        readback_retry_tail_submit.Signal(
            readback_retry_tail_receipt.Get(), 1
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(readback_retry_tail_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const Outcome readback_retry_tail_outcome =
            ExportSubmissionTransaction::ResolveReceiptOutcome(
                readback_retry_tail_receipt.Get(), 1
            );
        const auto readback_retry_decision =
            readback_retry_submission.ClassifyFrameAcceptance(
                readback_retry_tail_outcome,
                true,
                false
            );
        if (!readback_retry_decision.frame_accepted ||
            readback_retry_decision.export_consumed ||
            !readback_retry_decision.retry_requested ||
            readback_retry_decision.retryable_frame_rejection ||
            readback_retry_decision.latch_renderer ||
            !readback_retry_decision.readback_attempted ||
            readback_retry_decision.readback_accepted ||
            readback_retry_decision.encoder_dispatched ||
            encoder_dispatches.load(std::memory_order_acquire) != 0) {
            throw std::runtime_error(
                "production readback rejection did not preserve an accepted "
                "frame and pending export request"
            );
        }
        for (size_t index = 0; index < readback_retry_callbacks.size();
             ++index) {
            const uint32 expected_success = index == 1 ? 0u : 1u;
            if (readback_retry_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                readback_retry_success_callbacks[index].load(
                    std::memory_order_acquire
                ) != expected_success) {
                throw std::runtime_error(
                    "raytracing readback-retry callbacks retired incorrectly"
                );
            }
        }
        if (readback_retry_submission.GetReceiptFence()->GetRefCount() != 1 ||
            readback_retry_submission.GetReadbackReceiptFence()->GetRefCount() !=
                1 ||
            native_capture.Overflowed() || native_capture.Count() != 2 ||
            !native_capture.Event(0).outcome.WasSubmitted() ||
            !native_capture.Event(1).outcome.WasSubmitted()) {
            throw std::runtime_error(
                "raytracing readback rejection reached native submit or "
                "retained a terminal receipt"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        for (size_t index = 0; index < readback_retry_callbacks.size();
             ++index) {
            const uint32 expected_success = index == 1 ? 0u : 1u;
            if (readback_retry_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                readback_retry_success_callbacks[index].load(
                    std::memory_order_acquire
                ) != expected_success) {
                throw std::runtime_error(
                    "a second Sync replayed raytracing readback-retry work"
                );
            }
        }
        if (native_capture.Count() != 2) {
            throw std::runtime_error(
                "a second Sync replayed raytracing readback-retry native work"
            );
        }

        CommandList recovery_prefix(EQueueType::Graphics);
        recovery_prefix.CopyFrom(
            OwnedBytes(recovery_expected),
            recovery_source->GetView(),
            "RaytracingExportRecoveryPrefix"
        );
        recovery_prefix.AddCallback([&] {
            recovery_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        recovery_prefix.AddSuccessCallback([&] {
            recovery_success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit recovery_prefix_submit = recovery_prefix.Submit();
        recovery_submission.AttachSignal(recovery_prefix_submit);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(recovery_prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        recovery_submission.BeginReadback(device.CreateFence());
        CommandList recovery_readback_commands(EQueueType::Graphics);
        ReadbackFuture recovered_readback =
            recovery_readback_commands.Readback(
            recovery_source->GetView(),
            "RaytracingExportRecoveryReadback"
        );
        recovery_readback_commands.AddCallback([&] {
            recovery_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        recovery_readback_commands.AddSuccessCallback([&] {
            recovery_success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit recovery_readback_submit =
            recovery_readback_commands.Submit();
        recovery_submission.AttachReadbackSignal(
            recovery_readback_submit
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(recovery_readback_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const ReadbackResult recovered_readback_result =
            recovered_readback.Get();
        const auto recovered_values =
            recovered_readback_result.CopyAs<uint32>();
        if (recovered_readback_result.status !=
                ReadbackStatus::Ready ||
            !recovered_values.has_value() ||
            !recovery_submission.DispatchEncoder([&] {
                encoder_dispatches.fetch_add(
                    1, std::memory_order_relaxed
                );
            }) ||
            recovery_submission.DispatchEncoder([&] {
                encoder_dispatches.fetch_add(
                    100, std::memory_order_relaxed
                );
            })) {
            throw std::runtime_error(
                "accepted production readback did not dispatch the encoder "
                "exactly once"
            );
        }

        CommandList recovery_tail(EQueueType::Graphics);
        recovery_tail.CopyFrom(
            OwnedBytes(std::array<uint32, 4>{97, 101, 103, 107}),
            recovery_tail_target->GetView(),
            "RaytracingExportRecoveryTail"
        );
        recovery_tail.AddCallback([&] {
            recovery_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        recovery_tail.AddSuccessCallback([&] {
            recovery_success_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit recovery_tail_submit = recovery_tail.Submit();
        recovery_submission.AttachDependentWait(recovery_tail_submit);
        recovery_tail_submit.Signal(recovery_tail_receipt.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(recovery_tail_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const Outcome recovery_tail_outcome =
            ExportSubmissionTransaction::ResolveReceiptOutcome(
                recovery_tail_receipt.Get(), 1
            );
        const auto recovery_decision =
            recovery_submission.ClassifyFrameAcceptance(
                recovery_tail_outcome,
                true,
                false
            );
        if (!recovery_decision.frame_accepted ||
            !recovery_decision.export_consumed ||
            recovery_decision.retry_requested ||
            recovery_decision.retryable_frame_rejection ||
            recovery_decision.latch_renderer ||
            !recovery_decision.readback_attempted ||
            !recovery_decision.readback_accepted ||
            !recovery_decision.encoder_dispatched ||
            encoder_dispatches.load(std::memory_order_acquire) != 1 ||
            *recovered_values !=
                Array<uint32>(
                    recovery_expected.begin(),
                    recovery_expected.end()
                )) {
            throw std::runtime_error(
                "accepted raytracing export recovery did not consume the "
                "request exactly once"
            );
        }
        for (size_t index = 0; index < recovery_callbacks.size(); ++index) {
            if (recovery_callbacks[index].load(std::memory_order_acquire) != 1 ||
                recovery_success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "accepted raytracing export recovery callbacks retired "
                    "incorrectly"
                );
            }
        }
        if (recovery_submission.GetReceiptFence()->GetRefCount() != 1 ||
            recovery_submission.GetReadbackReceiptFence()->GetRefCount() != 1 ||
            native_capture.Overflowed() || native_capture.Count() != 5) {
            throw std::runtime_error(
                "accepted raytracing export recovery had unexpected native "
                "submissions or receipt lifetime"
            );
        }
        for (size_t index = 0; index < native_capture.Count(); ++index) {
            if (!native_capture.Event(index).outcome.WasSubmitted()) {
                throw std::runtime_error(
                    "raytracing export test observed a rejected native submit"
                );
            }
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        for (size_t index = 0; index < recovery_callbacks.size(); ++index) {
            if (recovery_callbacks[index].load(std::memory_order_acquire) != 1 ||
                recovery_success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "a second Sync replayed accepted raytracing export recovery"
                );
            }
        }
        if (native_capture.Count() != 5 ||
            encoder_dispatches.load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "a second Sync replayed raytracing export recovery work"
            );
        }
    } catch (...) {
        release_guard.Release();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        throw;
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=RaytracingExportAcceptanceRejection "
        "attempt1=prefix_rejected attempt1_tail=dependency_rejected "
        "attempt1_latch=false request_pending=true "
        "readback_retry=frame_accepted recovery_consumed=true encoder=once "
        "decision_table=verified native_rejected=0 callbacks=exactly_once "
        "keepalive=terminal replay=0"
    );
}

void RunRaytracingAcceptedReadbackMaterializationFailure() {
    auto& device = RenderDevice::Get();
    using Outcome = EExportSubmissionOutcome;

    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>(
        "rt_export_materialization_failure_source", 4, usage
    );
    ExportSubmissionTransaction submission(device.CreateFence());

    CommandList source_commands(EQueueType::Graphics);
    source_commands.CopyFrom(
        OwnedBytes(std::array<uint32, 4>{109, 113, 127, 131}),
        source->GetView(),
        "RaytracingExportMaterializationFailureSource"
    );
    CmdSubmit source_submit = source_commands.Submit();
    submission.AttachSignal(source_submit);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(source_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    if (submission.SourceOutcome() != Outcome::Accepted) {
        throw std::runtime_error(
            "materialization-failure source was not accepted"
        );
    }

    submission.BeginReadback(device.CreateFence());
    CommandList readback_commands(EQueueType::Graphics);
    ReadbackFuture readback = readback_commands.Readback(
        source->GetView(),
        "RaytracingExportInjectedMaterializationFailure"
    );
    CmdSubmit readback_submit = readback_commands.Submit();
    if (readback_submit.cmds.size() != 1 ||
        readback_submit.cmds.front()->Type() !=
            Command::EType::CopyBackBuffer) {
        throw std::runtime_error(
            "materialization-failure readback command was not recorded"
        );
    }
    const ReadbackToken backend_token =
        static_cast<const CopyBackBufferCmd&>(
            *readback_submit.cmds.front()
        ).OwningReadback();
    if (ReadbackBackendAccess::MaterializePayload(
            backend_token,
            nullptr,
            [](void*, std::span<Moer::byte>) {
                throw std::runtime_error(
                    "injected accepted readback materialization failure"
                );
            }
        )) {
        throw std::runtime_error(
            "throwing readback writer unexpectedly materialized a payload"
        );
    }

    submission.AttachReadbackSignal(readback_submit);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(readback_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    if (!submission.ResolveReadbackAcceptance()) {
        throw std::runtime_error(
            "materialization-failure readback was not natively accepted"
        );
    }

    const ReadbackResult result = readback.Get();
    if (result.status != ReadbackStatus::Error ||
        result.error_reason.empty() || !result.Bytes().empty()) {
        throw std::runtime_error(
            "accepted materialization failure did not resolve Error"
        );
    }
    submission.MarkAcceptedReadbackFailed();
    if (submission.ReadbackOutcome() != Outcome::Failed ||
        submission.DispatchEncoder([] {})) {
        throw std::runtime_error(
            "accepted materialization failure dispatched the encoder"
        );
    }

    const auto decision = submission.ClassifyFrameAcceptance(
        Outcome::Accepted, true, false
    );
    if (decision.frame_accepted || decision.export_consumed ||
        !decision.retry_requested ||
        !decision.independent_source_failed ||
        !decision.latch_renderer ||
        !decision.readback_attempted ||
        decision.readback_accepted ||
        decision.encoder_dispatched) {
        throw std::runtime_error(
            "accepted materialization failure did not latch frame policy"
        );
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=RaytracingAcceptedReadbackMaterializationFailure "
        "native=accepted payload=error encoder=0 latch=true"
    );
}

void RunRecoverableCopyDependencyRejection() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();

    FenceRef dependency      = device.CreateFence();
    FenceRef shared_done     = device.CreateFence();
    FenceRef stale_wait_done = device.CreateFence();
    dependency->Reject(1);

    BufferRef destination = device.CreateBuffer<uint32>(
        "recoverable_copy_destination",
        4,
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST
    );
    const std::array<uint32, 4> expected{17, 29, 43, 71};
    const std::array<uint32, 4> stale_values{101, 103, 107, 109};
    std::array<uint32, 4>       readback{};
    std::atomic<uint32>         blocker_callbacks{0};
    std::atomic<uint32>         blocker_success_callbacks{0};
    std::atomic<uint32>         rejected_callbacks{0};
    std::atomic<uint32>         rejected_success_callbacks{0};
    std::atomic<uint32>         recovered_callbacks{0};
    std::atomic<uint32>         recovered_success_callbacks{0};
    std::atomic<uint32>         stale_wait_callbacks{0};
    std::atomic<uint32>         stale_wait_success_callbacks{0};
    std::atomic<uint32>         dependent_callbacks{0};
    std::atomic<uint32>         dependent_success_callbacks{0};
    std::binary_semaphore       blocker_entered{0};
    std::binary_semaphore       release_blocker{0};
    OneShotSemaphoreRelease     release_guard(release_blocker);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    try {
        CommandList blocker(EQueueType::Copy);
        blocker.AddCallback([&] {
            blocker_callbacks.fetch_add(1, std::memory_order_relaxed);
            blocker_entered.release();
            release_blocker.acquire();
        });
        blocker.AddSuccessCallback([&] {
            blocker_success_callbacks.fetch_add(1, std::memory_order_relaxed);
        });
        RHIExecutor::Get().Submit(
            EQueueType::Copy,
            blocker.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        if (!blocker_entered.try_acquire_for(5s)) {
            throw std::runtime_error(
                "recoverable rejection test could not block Copy Completion"
            );
        }

    CommandList rejected(EQueueType::Copy);
    rejected.CopyFrom(
        OwnedBytes(std::array<uint32, 4>{1, 2, 3, 4}),
        destination->GetView(),
        "RecoverableCopyRejected"
    );
    rejected.AddCallback([&] {
        rejected_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    rejected.AddSuccessCallback([&] {
        rejected_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit rejected_submit = rejected.Submit();
    rejected_submit.Wait(dependency.Get(), 1).Signal(shared_done.Get(), 1);

    CommandList recovered(EQueueType::Copy);
    recovered.CopyFrom(
        OwnedBytes(expected),
        destination->GetView(),
        "RecoverableCopyUpload"
    );
    recovered.AddCallback([&] {
        recovered_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    recovered.AddSuccessCallback([&] {
        recovered_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit recovered_submit = recovered.Submit();
    recovered_submit.Signal(shared_done.Get(), 2);

    CommandList stale_wait(EQueueType::Copy);
    stale_wait.CopyFrom(
        OwnedBytes(stale_values),
        destination->GetView(),
        "RejectedStaleTimelineWait"
    );
    stale_wait.AddCallback([&] {
        stale_wait_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    stale_wait.AddSuccessCallback([&] {
        stale_wait_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit stale_wait_submit = stale_wait.Submit();
    stale_wait_submit.Wait(shared_done.Get(), 1).Signal(
        stale_wait_done.Get(), 1
    );

    CommandList dependent(EQueueType::Copy);
    dependent.CopyFrom(
        destination->GetView(),
        WritableBytes(readback),
        "RecoveredTimelineDependencyReadback"
    );
    dependent.AddCallback([&] {
        dependent_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    dependent.AddSuccessCallback([&] {
        dependent_success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit dependent_submit = dependent.Submit();
    dependent_submit.Wait(shared_done.Get(), 2);

    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        std::move(rejected_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        std::move(recovered_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        std::move(stale_wait_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        std::move(dependent_submit),
        ERHIExecSubmitFlags::FlushGPU
    );

    auto* shared_vk_fence = ResourceCast(shared_done.Get());
    auto* stale_vk_fence  = ResourceCast(stale_wait_done.Get());
    std::atomic_bool continue_waiting{true};
    std::jthread publication_deadline(
        [&continue_waiting](std::stop_token _stop) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!_stop.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(10ms);
            }
            if (!_stop.stop_requested()) {
                continue_waiting.store(false, std::memory_order_release);
            }
        }
    );
    const bool stale_wait_submitted =
        stale_vk_fence->WaitSubmitted(1, &continue_waiting);
    publication_deadline.request_stop();
    publication_deadline.join();
    if (stale_wait_submitted || !stale_vk_fence->IsRejected(1)) {
        throw std::runtime_error(
            "Submission did not publish a rejected stale-value dependency "
            "before Completion"
        );
    }
    if (!shared_vk_fence->IsRejected(1) ||
        !shared_vk_fence->WaitSubmitted(2) ||
        shared_vk_fence->HostWait(2) != VK_SUCCESS ||
        shared_vk_fence->HostWait(1) != VK_ERROR_UNKNOWN) {
        throw std::runtime_error(
            "reused external fence lost its exact rejection or accepted value"
        );
    }

    release_guard.Release();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!shared_vk_fence->IsRejected(1) ||
        shared_vk_fence->IsRejected(2) ||
        shared_vk_fence->IsFailed() ||
        !stale_vk_fence->IsRejected(1) ||
        stale_vk_fence->IsFailed()) {
        throw std::runtime_error(
            "recoverable Copy rejection poisoned the reusable external fence"
        );
    }
    if (shared_vk_fence->WaitSubmitted(1) ||
        !shared_vk_fence->WaitSubmitted(2) ||
        shared_done->GetValue() < 2 ||
        readback != expected) {
        throw std::runtime_error(
            "reused external fence did not recover at its next timeline value"
        );
    }
    if (blocker_callbacks.load(std::memory_order_acquire) != 1 ||
        blocker_success_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_success_callbacks.load(std::memory_order_acquire) != 0 ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1 ||
        stale_wait_callbacks.load(std::memory_order_acquire) != 1 ||
        stale_wait_success_callbacks.load(std::memory_order_acquire) != 0 ||
        dependent_callbacks.load(std::memory_order_acquire) != 1 ||
        dependent_success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error(
            "recoverable Copy rejection callbacks retired incorrectly"
        );
    }
    if (native_capture.Overflowed() || native_capture.Count() != 3) {
        throw std::runtime_error(
            "rejected Copy packets reached native submit or accepted packets "
            "were lost"
        );
    }
    const uint32 submission_thread_id =
        native_capture.Event(0).thread_id;
    for (size_t index = 0; index < native_capture.Count(); ++index) {
        const VulkanNativeSubmissionEvent& event =
            native_capture.Event(index);
        if (event.queue != EQueueType::Copy ||
            event.thread_role != ERHIThreadRole::Submission ||
            event.thread_id != submission_thread_id ||
            !event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "accepted Copy recovery packets escaped the sole native "
                "Submission owner"
            );
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 3 ||
        blocker_callbacks.load(std::memory_order_acquire) != 1 ||
        blocker_success_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_callbacks.load(std::memory_order_acquire) != 1 ||
        rejected_success_callbacks.load(std::memory_order_acquire) != 0 ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1 ||
        stale_wait_callbacks.load(std::memory_order_acquire) != 1 ||
        stale_wait_success_callbacks.load(std::memory_order_acquire) != 0 ||
        dependent_callbacks.load(std::memory_order_acquire) != 1 ||
        dependent_success_callbacks.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error(
            "a second Sync replayed recoverable Copy retirement"
        );
    }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        release_guard.Release();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=RecoverableCopyDependencyRejection "
        "fence=reused rejected=N recovered=N+1 stale_wait=rejected "
        "publication=Submission native_rejected=0 native_accepted=3 "
        "native_owner=verified runtime=recovered replay=0"
    );
}

void RunDirectCopyExecuteRejected() {
    auto& device = RenderDevice::Get();
    FenceRef done = device.CreateFence();
    std::atomic<uint32> ordinary_callbacks{0};
    std::atomic<uint32> success_callbacks{0};

    CommandList direct(EQueueType::Copy);
    direct.AddCallback([&] {
        ordinary_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    direct.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit submit = direct.Submit();
    submit.Signal(done.Get(), 1);

    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    device.GetCopyQueue().Execute(std::move(submit));
    auto* vk_done = ResourceCast(done.Get());
    if (!vk_done->IsRejected(1) || vk_done->IsFailed()) {
        throw std::runtime_error(
            "direct Copy Execute did not fail closed at the Runtime ownership "
            "boundary"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 0 ||
        ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        success_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "direct Copy rejection reached native submit or retired callbacks "
            "incorrectly"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (native_capture.Count() != 0 ||
        ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        success_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "direct Copy rejection replayed on a second Sync"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=DirectCopyExecuteRejected "
        "runtime_claim=exclusive native_submit=0 callbacks=exactly_once "
        "signal=rejected replay=0"
    );
}

void RunVulkanStorageUnifiedCopySubmission() {
    auto& device = RenderDevice::Get();
    auto* vulkan_device =
        static_cast<VulkanDevice*>(device.GetImpl());
    auto& copy_queue =
        static_cast<VkCopyQueue&>(device.GetCopyQueue());

    constexpr size_t element_count = 16;
    constexpr size_t byte_size = sizeof(uint32) * element_count;
    std::array<uint32, element_count> first_values{};
    std::array<uint32, element_count> expected{};
    std::array<uint32, element_count> readback{};
    for (uint32 index = 0; index < expected.size(); ++index) {
        first_values[index] = 0x15A00000u + index * 19u + 3u;
        expected[index]     = 0x15B00000u + index * 37u + 9u;
    }

    BufferRef destination = device.CreateBuffer<uint32>(
        "phase15b_io_copy_destination",
        element_count,
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );
    FenceRef done = device.CreateFence();
    NativeSubmissionCapture native_capture{};
    {
        ScopedNativeSubmissionObserver native_observer(native_capture);
        VulkanStorage storage(*vulkan_device, copy_queue);

        storage.Enqueue(
            first_values.data(),
            0,
            ResourceCast(destination.Get()),
            0,
            byte_size
        );
        storage.EnqueueSignal(done, 1);
        storage.Commit();
        if (ResourceCast(done.Get())->HostWait(1) != VK_SUCCESS) {
            throw std::runtime_error(
                "first Vulkan storage Copy commit did not signal completion"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        // The first session is now terminal. A second Enqueue must recycle it
        // without dropping the newly staged command or signal.
        storage.Enqueue(
            expected.data(),
            0,
            ResourceCast(destination.Get()),
            0,
            byte_size
        );
        storage.EnqueueSignal(done, 2);
        storage.Commit();
        if (ResourceCast(done.Get())->HostWait(2) != VK_SUCCESS) {
            throw std::runtime_error(
                "second Vulkan storage Copy commit did not signal completion"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }

    if (native_capture.Overflowed() || native_capture.Count() != 2) {
        throw std::runtime_error(
            "Vulkan storage commits did not issue exactly two native submits"
        );
    }
    const uint32 submission_thread_id =
        native_capture.Event(0).thread_id;
    for (size_t index = 0; index < native_capture.Count(); ++index) {
        const VulkanNativeSubmissionEvent& event =
            native_capture.Event(index);
        if (event.queue != EQueueType::Copy ||
            event.thread_role != ERHIThreadRole::Submission ||
            event.thread_id != submission_thread_id ||
            event.thread_id == Platform::GetCurrentThreadID() ||
            !event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "Vulkan storage Copy commit escaped the unified "
                "Submission owner"
            );
        }
    }
    if (ResourceCast(done.Get())->IsRejected(1) ||
        ResourceCast(done.Get())->IsRejected(2) ||
        ResourceCast(done.Get())->IsFailed()) {
        throw std::runtime_error(
            "Vulkan storage Copy signals were terminalized incorrectly"
        );
    }

    CommandList verify(EQueueType::Copy);
    verify.CopyFrom(
        destination->GetView(),
        WritableBytes(readback),
        "Phase15BIOCopyReadback"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        verify.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (readback != expected) {
        throw std::runtime_error(
            "Vulkan storage unified Copy submission readback mismatch"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=VulkanStorageUnifiedCopySubmission "
        "commits=2 signals=2 native_submit=2 native_owner=Submission "
        "session_recycle=verified readback=verified"
    );
}

void RunRuntimeRejectCompletionOwnership() {
    using namespace std::chrono_literals;

    auto&    device        = RenderDevice::Get();
    FenceRef dependency    = device.CreateFence();
    FenceRef graphics_done = device.CreateFence();
    FenceRef copy_done     = device.CreateFence();
    BufferRef rejected_destination = device.CreateBuffer<uint32>(
        "runtime_reject_destination",
        4,
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST
    );
    ResourceCast(dependency.Get())->Fail(VK_ERROR_UNKNOWN);

    std::atomic<uint32> ordinary_callbacks{0};
    std::atomic<uint32> success_callbacks{0};
    std::atomic<uint32> wrong_owner_callbacks{0};
    std::atomic<uint32> signals_not_failed_before_callback{0};
    std::atomic<uint32> completion_sync_guard_returns{0};
    std::atomic<bool>   sync_started{false};
    std::atomic<bool>   sync_returned{false};
    std::atomic<bool>   returned_before_release{false};
    std::atomic<bool>   helper_timed_out{false};
    std::binary_semaphore copy_callback_entered{0};
    std::binary_semaphore release_copy_callback{0};
    std::binary_semaphore copy_callback_finished{0};

    auto validate_completion_owner = [&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            wrong_owner_callbacks.fetch_add(1, std::memory_order_relaxed);
        }
    };

    CommandList graphics(EQueueType::Graphics);
    graphics.CopyFrom(
        OwnedBytes(std::array<uint32, 4>{1, 2, 3, 4}),
        rejected_destination->GetView(),
        "RuntimeRejectRecordedCopy"
    );
    graphics.AddCallback([&] {
        validate_completion_owner();
        device.GetCopyQueue().Sync(0);
        completion_sync_guard_returns.fetch_add(1, std::memory_order_relaxed);
        if (!ResourceCast(graphics_done.Get())->IsRejected(1)) {
            signals_not_failed_before_callback.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    graphics.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.Wait(dependency.Get(), 1).Signal(graphics_done.Get(), 1);

    CommandList copy(EQueueType::Copy);
    copy.AddCallback([&] {
        validate_completion_owner();
        device.GetCommandQueue(EQueueType::Graphics).Sync();
        completion_sync_guard_returns.fetch_add(1, std::memory_order_relaxed);
        if (!ResourceCast(copy_done.Get())->IsRejected(1)) {
            signals_not_failed_before_callback.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        copy_callback_entered.release();
        release_copy_callback.acquire();
        ordinary_callbacks.fetch_add(1, std::memory_order_relaxed);
        copy_callback_finished.release();
    });
    copy.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit copy_submit = copy.Submit();
    copy_submit.Signal(copy_done.Get(), 1);

    std::jthread callback_gate([&] {
        if (!copy_callback_entered.try_acquire_for(5s)) {
            helper_timed_out.store(true, std::memory_order_release);
            release_copy_callback.release();
            return;
        }
        const auto sync_deadline = std::chrono::steady_clock::now() + 5s;
        while (!sync_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < sync_deadline) {
            std::this_thread::yield();
        }
        if (!sync_started.load(std::memory_order_acquire)) {
            helper_timed_out.store(true, std::memory_order_release);
        }
        std::this_thread::sleep_for(250ms);
        returned_before_release.store(
            sync_returned.load(std::memory_order_acquire),
            std::memory_order_release
        );
        release_copy_callback.release();
        if (!copy_callback_finished.try_acquire_for(5s)) {
            helper_timed_out.store(true, std::memory_order_release);
        }
    });

    Array<RHIBackendSubmissionBatchEntry> submits{};
    submits.emplace_back(EQueueType::Graphics, std::move(graphics_submit));
    submits.emplace_back(EQueueType::Copy, std::move(copy_submit));
    RHIExecutor::Get().Submit(
        std::move(submits), ERHIExecSubmitFlags::FlushGPU
    );

    sync_started.store(true, std::memory_order_release);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    sync_returned.store(true, std::memory_order_release);
    callback_gate.join();

    if (helper_timed_out.load(std::memory_order_acquire)) {
        throw std::runtime_error("runtime rejection Completion callback timed out");
    }
    if (returned_before_release.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "RHI Sync returned before a rejected Copy packet retired on Completion"
        );
    }
    if (ordinary_callbacks.load(std::memory_order_acquire) != 2 ||
        success_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "runtime rejection callbacks did not retire exactly once"
        );
    }
    if (wrong_owner_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "runtime rejection callback ran outside the Completion owner"
        );
    }
    if (completion_sync_guard_returns.load(std::memory_order_acquire) != 2) {
        throw std::runtime_error(
            "Completion owner cross-queue Sync did not return without blocking"
        );
    }
    if (signals_not_failed_before_callback.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "runtime rejection callback ran before its signal fence failed"
        );
    }
    if (!ResourceCast(graphics_done.Get())->IsRejected(1) ||
        !ResourceCast(copy_done.Get())->IsRejected(1) ||
        ResourceCast(graphics_done.Get())->IsFailed() ||
        ResourceCast(copy_done.Get())->IsFailed()) {
        throw std::runtime_error(
            "runtime rejection did not terminalize the exact external signal value"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (ordinary_callbacks.load(std::memory_order_acquire) != 2 ||
        success_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "a second Sync replayed runtime rejection callbacks"
        );
    }
    LOG_INFO(
        "[TESTCASE][PASS] name=RuntimeRejectCompletionOwnership "
        "sources=2 queues=Graphics,Copy callback_owner=Completion "
        "cross_queue_sync=guarded sync_retirement=serial"
    );
}

void RunBoundedCrossBatchSubmissionPipeline(uint32 _batch_window) {
    using namespace std::chrono_literals;

    if (_batch_window != 1 && _batch_window != 2) {
        throw std::invalid_argument(
            "bounded cross-batch pipeline test requires window 1 or 2"
        );
    }

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available || !topology.compute.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=BoundedCrossBatchSubmissionPipeline "
            "window={} reason=queue_unavailable "
            "graphics_available={} compute_available={} "
            "graphics_native={} compute_native={}",
            _batch_window,
            topology.graphics.available,
            topology.compute.available,
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id
        );
        return;
    }

    constexpr uint64 async_queue_scope = 0x5048313544504950ull;
    const bool distinct_native =
        topology.graphics.native_queue_id != topology.compute.native_queue_id;
    const bool runtime_native_alias =
        RHISubmissionPipelinePolicy::HasAvailableNativeLaneAlias(
            topology
        );
    const uint32 main_thread_id = Platform::GetCurrentThreadID();

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          graphics_wait_capture{EQueueType::Graphics};
    ScopedDependencyWaitObserver   wait_observer(graphics_wait_capture);

    FenceRef dependency    = device.CreateFence();
    FenceRef graphics_done = device.CreateFence();
    FenceRef compute_done  = device.CreateFence();

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::array<std::atomic<uint32>, 2> translate_counts{};
    std::binary_semaphore              graphics_translated{0};
    std::binary_semaphore              compute_translated{0};
    std::atomic<bool>                  dependency_release_started{false};
    std::atomic<bool>                  graphics_observed_release{true};
    std::atomic<bool>                  compute_observed_release{false};
    bool                               dependency_released{false};

    const auto release_dependency = [&] {
        if (dependency_released) {
            return;
        }
        dependency_release_started.store(true, std::memory_order_release);
        ResourceCast(dependency.Get())->SignalHost(1);
        dependency_released = true;
    };

    try {
        CommandList graphics(EQueueType::Graphics);
        graphics.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics.AddCustomCommand(
            MakeUnique<PipelineTranslateProbeCommand>(
                EQueueType::Graphics,
                &graphics_translated,
                &translate_counts[0],
                &dependency_release_started,
                &graphics_observed_release
            ),
            "PipelineWindowGraphicsTranslateProbe"
        );
        graphics.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        graphics.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit graphics_submit = graphics.Submit();
        graphics_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        graphics_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_submit.async_queue_scope = async_queue_scope;
        graphics_submit.Wait(dependency.Get(), 1)
            .Signal(graphics_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(graphics_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        if (!graphics_wait_capture.entered.try_acquire_for(5s) ||
            graphics_wait_capture.count.load(std::memory_order_acquire) != 1 ||
            graphics_wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            native_capture.Count() != 0 ||
            source_capture.Count() != 0 ||
            !graphics_translated.try_acquire()) {
            throw std::runtime_error(
                "batch 0 did not block its translated Graphics packet on "
                "the sole Submission owner"
            );
        }
        if (translate_counts[0].load(std::memory_order_acquire) != 1 ||
            graphics_observed_release.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "batch 0 Translate did not precede dependency release exactly once"
            );
        }

        CommandList compute(EQueueType::Compute);
        compute.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        compute.AddCustomCommand(
            MakeUnique<PipelineTranslateProbeCommand>(
                EQueueType::Compute,
                &compute_translated,
                &translate_counts[1],
                &dependency_release_started,
                &compute_observed_release
            ),
            "PipelineWindowComputeTranslateProbe"
        );
        compute.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        compute.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit compute_submit = compute.Submit();
        compute_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        compute_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        compute_submit.async_queue_scope = async_queue_scope;
        compute_submit.Signal(compute_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Compute,
            std::move(compute_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        bool compute_translate_observed = false;
        if (_batch_window == 2 && !runtime_native_alias) {
            compute_translate_observed =
                compute_translated.try_acquire_for(5s);
            if (!compute_translate_observed ||
                compute_observed_release.load(std::memory_order_acquire)) {
                throw std::runtime_error(
                    "window 2 did not Translate batch 1 while batch 0 "
                    "Submission was blocked"
                );
            }
        } else if (_batch_window == 1) {
            compute_translate_observed =
                compute_translated.try_acquire_for(250ms);
            if (compute_translate_observed) {
                throw std::runtime_error(
                    "window 1 admitted batch 1 Translate before batch 0 "
                    "Submission reached a terminal state"
                );
            }
        } else {
            compute_translate_observed =
                compute_translated.try_acquire_for(250ms);
            if (compute_translate_observed) {
                throw std::runtime_error(
                    "runtime native queue alias did not reduce the "
                    "effective window to 1"
                );
            }
            LOG_INFO(
                "[TESTCASE][PASS] "
                "name=BoundedCrossBatchSubmissionPipelineOverlap "
                "requested_window=2 effective_window=1 "
                "reason=native_queue_alias graphics_native={} "
                "compute_native={} copy_native={} admission=blocked",
                topology.graphics.native_queue_id,
                topology.compute.native_queue_id,
                topology.copy.native_queue_id
            );
        }

        release_dependency();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (!compute_translate_observed &&
            !compute_translated.try_acquire()) {
            throw std::runtime_error(
                "batch 1 Translate did not complete after dependency release"
            );
        }
        if (translate_counts[0].load(std::memory_order_acquire) != 1 ||
            translate_counts[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "cross-batch Translate probes did not execute exactly once"
            );
        }
        if ((_batch_window == 1 || runtime_native_alias) &&
            !compute_observed_release.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "effective window 1 batch 1 Translate started before "
                "dependency release"
            );
        }

        if (source_capture.Overflowed() || source_capture.Count() != 2) {
            throw std::runtime_error(
                "cross-batch pipeline did not report exactly two source submissions"
            );
        }
        const VulkanSourceSubmissionEvent& graphics_source =
            source_capture.Event(0);
        const VulkanSourceSubmissionEvent& compute_source =
            source_capture.Event(1);
        if (graphics_source.batch_sequence == 0 ||
            compute_source.batch_sequence !=
                graphics_source.batch_sequence + 1 ||
            graphics_source.source_index != 0 ||
            compute_source.source_index != 0 ||
            graphics_source.original_source_index != 0 ||
            compute_source.original_source_index != 0 ||
            graphics_source.queue != EQueueType::Graphics ||
            compute_source.queue != EQueueType::Compute ||
            graphics_source.async_queue_scope != async_queue_scope ||
            compute_source.async_queue_scope != async_queue_scope) {
            throw std::runtime_error(
                "Submission owner did not preserve stable Graphics-to-Compute "
                "source order across consecutive batches"
            );
        }

        if (native_capture.Overflowed() || native_capture.Count() != 2) {
            throw std::runtime_error(
                "cross-batch pipeline did not issue exactly two native submits"
            );
        }
        constexpr std::array expected_queues{
            EQueueType::Graphics,
            EQueueType::Compute,
        };
        const uint32 submission_thread_id =
            native_capture.Event(0).thread_id;
        for (size_t index = 0; index < expected_queues.size(); ++index) {
            const VulkanNativeSubmissionEvent& event =
                native_capture.Event(index);
            if (event.queue != expected_queues[index] ||
                event.thread_role != ERHIThreadRole::Submission ||
                event.thread_id != submission_thread_id ||
                event.thread_id == main_thread_id ||
                !event.outcome.WasSubmitted()) {
                throw std::runtime_error(
                    "native submission escaped the sole Submission owner "
                    "or changed cross-batch source order"
                );
            }
        }
        if ((native_capture.Event(0).native_queue_handle ==
             native_capture.Event(1).native_queue_handle) !=
            !distinct_native) {
            throw std::runtime_error(
                "cross-batch native observer disagrees with queue topology"
            );
        }

        if (ResourceCast(graphics_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(compute_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(graphics_done.Get())->IsRejected(1) ||
            ResourceCast(compute_done.Get())->IsRejected(1) ||
            ResourceCast(graphics_done.Get())->IsFailed() ||
            ResourceCast(compute_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "cross-batch pipeline signals did not complete successfully"
            );
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
                success_callbacks[index].load(std::memory_order_acquire) != 1) {
                throw std::runtime_error(
                    "cross-batch callbacks did not retire exactly once"
                );
            }
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (source_capture.Count() != 2 || native_capture.Count() != 2 ||
            translate_counts[0].load(std::memory_order_acquire) != 1 ||
            translate_counts[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "a second Sync replayed cross-batch Translate or submission"
            );
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
                success_callbacks[index].load(std::memory_order_acquire) != 1) {
                throw std::runtime_error(
                    "a second Sync replayed cross-batch callbacks"
                );
            }
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        release_dependency();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=BoundedCrossBatchSubmissionPipeline "
        "window={} native_alias={} overlap_assertion={} batches=2 "
        "queues=Graphics,Compute source_order=G,C "
        "native_owner=Submission signals=success callbacks=exactly_once replay=0",
        _batch_window,
        runtime_native_alias,
        _batch_window == 2 && !runtime_native_alias ?
            "verified" :
            "blocked"
    );
}

void RunBoundedCrossBatchMalformedPreflightOrdering() {
    using namespace std::chrono_literals;

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "reason=graphics_queue_unavailable"
        );
        return;
    }

    constexpr uint64 async_queue_scope = 0x50483135444D414Cull;
    const uint32     main_thread_id    = Platform::GetCurrentThreadID();

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          wait_capture{EQueueType::Graphics};
    ScopedDependencyWaitObserver   wait_observer(wait_capture);
    BackendSyncWaitCapture         sync_wait_capture{};
    ScopedBackendSyncWaitObserver  sync_wait_observer(sync_wait_capture);
    BatchPreflightRejectionCapture preflight_capture{};
    ScopedBatchPreflightRejectionObserver preflight_observer(
        preflight_capture
    );

    FenceRef dependency     = device.CreateFence();
    FenceRef prefix_done    = device.CreateFence();
    FenceRef malformed_done = device.CreateFence();

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::binary_semaphore              prefix_translated{0};
    std::binary_semaphore              malformed_translated{0};
    std::binary_semaphore              malformed_callback_retired{0};
    std::mutex                         callback_order_mutex{};
    std::vector<uint32>                callback_order{};
    std::atomic<bool>                  sync_returned{false};
    std::jthread                       sync_waiter{};
    bool                               dependency_released{false};

    const auto release_dependency = [&] {
        if (dependency_released) {
            return;
        }
        ResourceCast(dependency.Get())->SignalHost(1);
        dependency_released = true;
    };
    const auto record_callback = [&](size_t _index) {
        {
            std::lock_guard lock(callback_order_mutex);
            callback_order.emplace_back(static_cast<uint32>(_index));
        }
        ordinary_callbacks[_index].fetch_add(
            1, std::memory_order_relaxed
        );
        if (_index == 1) {
            malformed_callback_retired.release();
        }
    };

    try {
        CommandList prefix(EQueueType::Graphics);
        prefix.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &prefix_translated, EQueueType::Graphics
            ),
            "PipelineMalformedPrefixTranslateProbe"
        );
        prefix.AddCallback([&] { record_callback(0); });
        prefix.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit prefix_submit = prefix.Submit();
        prefix_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        prefix_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix_submit.async_queue_scope = async_queue_scope;
        prefix_submit.Wait(dependency.Get(), 1)
            .Signal(prefix_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        if (!wait_capture.entered.try_acquire_for(5s) ||
            wait_capture.count.load(std::memory_order_acquire) != 1 ||
            wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            !prefix_translated.try_acquire() ||
            source_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "malformed-ordering prefix did not block on the sole "
                "Submission owner"
            );
        }

        CommandList malformed(EQueueType::Graphics);
        malformed.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        malformed.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &malformed_translated, EQueueType::Graphics
            ),
            "PipelineMalformedRejectedTranslateProbe"
        );
        malformed.AddCallback([&] { record_callback(1); });
        malformed.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit malformed_submit = malformed.Submit();
        malformed_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        malformed_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        malformed_submit.async_queue_scope = async_queue_scope;
        malformed_submit.Signal(malformed_done.Get(), 1);
        malformed_submit.segments = {RHISubmitSegment{
            .queue = EQueueType::Graphics,
            .begin = 1,
            .end   = 0,
        }};

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(malformed_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        sync_waiter = std::jthread([&] {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            sync_returned.store(true, std::memory_order_release);
        });
        if (!sync_wait_capture.entered.try_acquire_for(5s) ||
            sync_wait_capture.count.load(std::memory_order_acquire) != 1 ||
            sync_returned.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "malformed-ordering Sync was not published behind both "
                "batches"
            );
        }
        if (!preflight_capture.entered.try_acquire_for(5s) ||
            preflight_capture.count.load(std::memory_order_acquire) != 1 ||
            preflight_capture.batch_sequence.load(
                std::memory_order_acquire
            ) == 0 ||
            preflight_capture.thread_id.load(
                std::memory_order_acquire
            ) == main_thread_id ||
            preflight_capture.wrong_owner.load(
                std::memory_order_acquire
            ) ||
            preflight_capture.executable_preflight.load(
                std::memory_order_acquire
            )) {
            throw std::runtime_error(
                "malformed batch did not reach initial preflight rejection "
                "on the Translate owner"
            );
        }

        if (malformed_callback_retired.try_acquire_for(500ms) ||
            malformed_translated.try_acquire() ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 0 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 0 ||
            success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            success_callbacks[1].load(std::memory_order_acquire) != 0 ||
            source_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "malformed batch retired before the accepted pipeline "
                "prefix committed"
            );
        }

        release_dependency();
        sync_waiter.join();
        if (!sync_returned.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "malformed-ordering Sync did not drain after prefix release"
            );
        }

        if (ResourceCast(prefix_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(prefix_done.Get())->IsRejected(1) ||
            ResourceCast(prefix_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "malformed-ordering prefix did not complete successfully"
            );
        }
        if (ResourceCast(malformed_done.Get())->HostWait(1) !=
                VK_ERROR_UNKNOWN ||
            !ResourceCast(malformed_done.Get())->IsRejected(1) ||
            ResourceCast(malformed_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "malformed batch did not reject its exact signal value"
            );
        }
        if (ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 0) {
            throw std::runtime_error(
                "malformed-ordering callbacks did not retire exactly once"
            );
        }
        {
            std::lock_guard lock(callback_order_mutex);
            if (callback_order != std::vector<uint32>{0, 1}) {
                throw std::runtime_error(
                    "malformed batch callback retired before its accepted "
                    "pipeline prefix"
                );
            }
        }
        if (source_capture.Overflowed() || source_capture.Count() != 1 ||
            source_capture.Event(0).queue != EQueueType::Graphics ||
            source_capture.Event(0).async_queue_scope !=
                async_queue_scope) {
            throw std::runtime_error(
                "malformed batch reached source submission or the prefix "
                "was not observed exactly once"
            );
        }
        if (native_capture.Overflowed() || native_capture.Count() != 1 ||
            native_capture.Event(0).queue != EQueueType::Graphics ||
            native_capture.Event(0).thread_role !=
                ERHIThreadRole::Submission ||
            native_capture.Event(0).thread_id == main_thread_id ||
            !native_capture.Event(0).outcome.WasSubmitted()) {
            throw std::runtime_error(
                "malformed-ordering prefix escaped stable Submission "
                "ownership"
            );
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (source_capture.Count() != 1 || native_capture.Count() != 1 ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 0 ||
            malformed_translated.try_acquire()) {
            throw std::runtime_error(
                "a second Sync replayed malformed-ordering retirement"
            );
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        release_dependency();
        try {
            if (sync_waiter.joinable()) {
                sync_waiter.join();
            } else {
                RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            }
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    // Invalid topology intentionally hard-latches this backend instance.
    // Restart it before the remaining focused window-2 lifecycle contracts.
    RHIExecutor::ShutDown();
    RHIExecutor::StartUp(2);

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=BoundedCrossBatchMalformedPreflightOrdering "
        "window=2 batches=2 queue=Graphics prefix=committed "
        "malformed=rejected callback_order=prefix,malformed "
        "signals=success,rejected native_owner=Submission replay=0 "
        "malformed_translate=0 runtime=restarted"
    );
}

void RunBoundedCrossBatchRecoverableRejection() {
    using namespace std::chrono_literals;

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available || !topology.copy.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available={} copy_available={} "
            "graphics_native={} copy_native={}",
            topology.graphics.available,
            topology.copy.available,
            topology.graphics.native_queue_id,
            topology.copy.native_queue_id
        );
        return;
    }

    constexpr uint64 async_queue_scope =
        0x504831354552454Aull;
    const bool runtime_native_alias =
        RHISubmissionPipelinePolicy::HasAvailableNativeLaneAlias(
            topology
        );
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    SourceTranslationCapture        translation_capture(
        async_queue_scope, EQueueType::Copy
    );
    ScopedSourceTranslationObserver translation_observer(
        translation_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture graphics_wait_capture{
        EQueueType::Graphics
    };
    ScopedDependencyWaitObserver wait_observer(
        graphics_wait_capture
    );

    FenceRef dependency    = device.CreateFence();
    FenceRef graphics_done = device.CreateFence();
    FenceRef copy_done     = device.CreateFence();

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::binary_semaphore graphics_translated{0};
    bool                  dependency_rejected{false};
    constexpr std::array<uint32, 4> expected{
        0x15E10001u,
        0x15E10002u,
        0x15E10003u,
        0x15E10004u,
    };
    std::array<uint32, expected.size()> readback{};
    BufferRef destination = device.CreateBuffer<uint32>(
        "phase15e_cross_batch_copy_recovery",
        expected.size(),
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );

    const auto reject_dependency = [&] {
        if (dependency_rejected) {
            return;
        }
        dependency->Reject(1);
        dependency_rejected = true;
    };

    try {
        CommandList graphics(EQueueType::Graphics);
        graphics.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &graphics_translated, EQueueType::Graphics
            ),
            "PipelineRecoverableGraphicsTranslateProbe"
        );
        graphics.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        graphics.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit graphics_submit = graphics.Submit();
        graphics_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        graphics_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_submit.async_queue_scope = async_queue_scope;
        graphics_submit.Wait(dependency.Get(), 1)
            .Signal(graphics_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(graphics_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        if (!graphics_wait_capture.entered.try_acquire_for(5s) ||
            graphics_wait_capture.count.load(std::memory_order_acquire) != 1 ||
            graphics_wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            !graphics_translated.try_acquire() ||
            source_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "recoverable batch 0 did not block its translated Graphics "
                "packet on the sole Submission owner"
            );
        }

        CommandList copy(EQueueType::Copy);
        copy.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy.CopyFrom(
            OwnedBytes(expected),
            destination->GetView(),
            "PipelineRecoverableCopyUpload"
        );
        copy.CopyFrom(
            destination->GetView(),
            WritableBytes(readback),
            "PipelineRecoverableCopyReadback"
        );
        copy.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        copy.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit copy_submit = copy.Submit();
        copy_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        copy_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy_submit.async_queue_scope = async_queue_scope;
        copy_submit.Signal(copy_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Copy,
            std::move(copy_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        bool copy_translate_observed = false;
        if (!runtime_native_alias) {
            copy_translate_observed =
                translation_capture.copy_recorded.try_acquire_for(
                    5s
                );
            if (!copy_translate_observed) {
                throw std::runtime_error(
                    "window 2 did not record Copy batch 1 while batch 0 "
                    "Submission was blocked"
                );
            }
        } else {
            LOG_INFO(
                "[TESTCASE][PASS] "
                "name=BoundedCrossBatchRecoverableRejectionOverlap "
                "requested_window=2 effective_window=1 "
                "reason=native_queue_alias graphics_native={} "
                "compute_native={} copy_native={} admission=blocked "
                "cpu_seam=RHISubmissionPipelinePolicy",
                topology.graphics.native_queue_id,
                topology.compute.native_queue_id,
                topology.copy.native_queue_id
            );
        }

        if (source_capture.Count() != 0 || native_capture.Count() != 0) {
            throw std::runtime_error(
                "a cross-batch packet overtook the blocked Submission owner"
            );
        }

        reject_dependency();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (!translation_capture.Seen(
                0,
                EVulkanSourceTranslationPhase::Recorded
            )) {
            throw std::runtime_error(
                "recoverable Copy batch 1 never reached recorded state"
            );
        }
        const VulkanSourceTranslationEvent& translation_event =
            translation_capture.Event(
                0,
                EVulkanSourceTranslationPhase::Recorded
            );
        if (!translation_capture.IsValid() ||
            translation_event.queue != EQueueType::Copy ||
            translation_event.native_queue_id !=
                topology.copy.native_queue_id ||
            translation_event.thread_role !=
                ERHIThreadRole::Translate) {
            throw std::runtime_error(
                "recoverable Copy translation lost queue identity or "
                "Translate ownership"
            );
        }
        if (ResourceCast(graphics_done.Get())->HostWait(1) !=
                VK_ERROR_UNKNOWN ||
            !ResourceCast(graphics_done.Get())->IsRejected(1) ||
            ResourceCast(graphics_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "recoverable batch 0 did not reject its exact signal value"
            );
        }
        if (ResourceCast(copy_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(copy_done.Get())->IsRejected(1) ||
            ResourceCast(copy_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "recoverable batch 0 rejection contaminated batch 1"
            );
        }
        if (readback != expected) {
            throw std::runtime_error(
                "recoverable Copy batch 1 readback mismatch"
            );
        }
        if (ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "cross-batch recoverable callbacks did not retire exactly once"
            );
        }

        if (source_capture.Overflowed() || source_capture.Count() != 1) {
            throw std::runtime_error(
                "rejected batch 0 reached source submission or recovered "
                "batch 1 was not observed exactly once"
            );
        }
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(0);
        if (source_event.batch_sequence == 0 ||
            source_event.source_index != 0 ||
            source_event.original_source_index != 0 ||
            source_event.queue != EQueueType::Copy ||
            source_event.async_queue_scope != async_queue_scope) {
            throw std::runtime_error(
                "recovered Copy batch 1 lost stable source identity"
            );
        }

        if (native_capture.Overflowed() || native_capture.Count() != 1) {
            throw std::runtime_error(
                "rejected batch 0 reached native submit or recovered batch 1 "
                "did not submit exactly once"
            );
        }
        const VulkanNativeSubmissionEvent& native_event =
            native_capture.Event(0);
        if (native_event.queue != EQueueType::Copy ||
            native_event.thread_role != ERHIThreadRole::Submission ||
            native_event.thread_id == main_thread_id ||
            !native_event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "recovered Copy batch 1 escaped stable Submission ownership"
            );
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (source_capture.Count() != 1 || native_capture.Count() != 1 ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "a second Sync replayed cross-batch recoverable retirement"
            );
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        reject_dependency();
        try {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        } catch (...) {
            std::terminate();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=BoundedCrossBatchRecoverableRejection "
        "window=2 native_alias={} overlap_assertion={} batches=2 "
        "queues=Graphics,Copy "
        "batch0_native_submit=0 batch0_callbacks=ordinary1_success0 "
        "batch0_signal=rejected batch1_native_submit=1 "
        "batch1_callbacks=ordinary1_success1 batch1_signal=success "
        "copy_translate_owner=Translate native_owner=Submission "
        "readback=verified replay=0",
        runtime_native_alias,
        runtime_native_alias ? "blocked" : "verified"
    );
}

void RunBoundedPipelineShutdownCancellation() {
    using namespace std::chrono_literals;

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available || !topology.copy.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=BoundedPipelineShutdownCancellation "
            "window=2 reason=queue_unavailable "
            "graphics_available={} copy_available={} "
            "graphics_native={} copy_native={}",
            topology.graphics.available,
            topology.copy.available,
            topology.graphics.native_queue_id,
            topology.copy.native_queue_id
        );
        return;
    }

    constexpr uint64 async_queue_scope =
        0x5048313545534844ull;
    const bool runtime_native_alias =
        RHISubmissionPipelinePolicy::HasAvailableNativeLaneAlias(
            topology
        );
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    SourceTranslationCapture        translation_capture(
        async_queue_scope, EQueueType::Copy
    );
    ScopedSourceTranslationObserver translation_observer(
        translation_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          graphics_wait_capture{EQueueType::Graphics};
    ScopedDependencyWaitObserver   dependency_wait_observer(
        graphics_wait_capture
    );
    BackendSyncWaitCapture        sync_wait_capture{};
    ScopedBackendSyncWaitObserver sync_wait_observer(sync_wait_capture);

    FenceRef dependency    = device.CreateFence();
    FenceRef graphics_done = device.CreateFence();
    FenceRef copy_done     = device.CreateFence();

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::binary_semaphore graphics_translated{0};
    std::atomic<bool>     sync_returned{false};
    std::jthread          sync_waiter{};
    bool                  runtime_stopped{false};
    constexpr std::array<uint32, 4> expected{
        0x15E20001u,
        0x15E20002u,
        0x15E20003u,
        0x15E20004u,
    };
    std::array<uint32, expected.size()> readback{};
    BufferRef destination = device.CreateBuffer<uint32>(
        "phase15e_shutdown_copy_suffix",
        expected.size(),
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );

    const auto stop_runtime = [&] {
        if (runtime_stopped) {
            return;
        }
        RHIExecutor::ShutDown();
        runtime_stopped = true;
    };

    try {
        CommandList graphics(EQueueType::Graphics);
        graphics.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &graphics_translated, EQueueType::Graphics
            ),
            "PipelineShutdownGraphicsTranslateProbe"
        );
        graphics.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        graphics.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit graphics_submit = graphics.Submit();
        graphics_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        graphics_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        graphics_submit.async_queue_scope = async_queue_scope;
        graphics_submit.Wait(dependency.Get(), 1)
            .Signal(graphics_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(graphics_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        if (!graphics_wait_capture.entered.try_acquire_for(5s) ||
            graphics_wait_capture.count.load(std::memory_order_acquire) != 1 ||
            graphics_wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            !graphics_translated.try_acquire() ||
            source_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "shutdown batch 0 did not block its translated Graphics "
                "packet on the sole Submission owner"
            );
        }

        CommandList copy(EQueueType::Copy);
        copy.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy.CopyFrom(
            OwnedBytes(expected),
            destination->GetView(),
            "PipelineShutdownCopyUpload"
        );
        copy.CopyFrom(
            destination->GetView(),
            WritableBytes(readback),
            "PipelineShutdownCopyReadback"
        );
        copy.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        copy.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit copy_submit = copy.Submit();
        copy_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        copy_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        copy_submit.async_queue_scope = async_queue_scope;
        copy_submit.Signal(copy_done.Get(), 1);

        RHIExecutor::Get().Submit(
            EQueueType::Copy,
            std::move(copy_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        bool copy_translate_observed = false;
        if (!runtime_native_alias) {
            copy_translate_observed =
                translation_capture.copy_recorded.try_acquire_for(
                    5s
                );
            if (!copy_translate_observed) {
                throw std::runtime_error(
                    "window 2 did not record shutdown Copy batch 1 while "
                    "batch 0 Submission was blocked"
                );
            }
        } else {
            LOG_INFO(
                "[TESTCASE][PASS] "
                "name=BoundedPipelineShutdownCancellationOverlap "
                "requested_window=2 effective_window=1 "
                "reason=native_queue_alias graphics_native={} "
                "compute_native={} copy_native={} admission=blocked "
                "cpu_seam=RHISubmissionPipelinePolicy",
                topology.graphics.native_queue_id,
                topology.compute.native_queue_id,
                topology.copy.native_queue_id
            );
        }

        if (source_capture.Count() != 0 || native_capture.Count() != 0) {
            throw std::runtime_error(
                "shutdown batch 1 overtook the blocked Submission owner"
            );
        }

        sync_waiter = std::jthread([&] {
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            sync_returned.store(true, std::memory_order_release);
        });
        if (!sync_wait_capture.entered.try_acquire_for(5s) ||
            sync_wait_capture.count.load(std::memory_order_acquire) != 1 ||
            sync_returned.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "concurrent Sync was not registered behind the bounded "
                "pipeline dependency wait"
            );
        }

        stop_runtime();
        sync_waiter.join();

        if (!translation_capture.Seen(
                0,
                EVulkanSourceTranslationPhase::Recorded
            )) {
            throw std::runtime_error(
                "shutdown Copy batch 1 did not record while draining "
                "the pipeline"
            );
        }
        const VulkanSourceTranslationEvent& translation_event =
            translation_capture.Event(
                0,
                EVulkanSourceTranslationPhase::Recorded
            );
        if (!translation_capture.IsValid() ||
            translation_event.queue != EQueueType::Copy ||
            translation_event.native_queue_id !=
                topology.copy.native_queue_id ||
            translation_event.thread_role !=
                ERHIThreadRole::Translate) {
            throw std::runtime_error(
                "shutdown Copy batch lost Translate ownership or queue "
                "identity"
            );
        }
        if (!sync_returned.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "pipeline shutdown did not release the concurrent Sync"
            );
        }
        if (ResourceCast(graphics_done.Get())->HostWait(1) !=
                VK_ERROR_UNKNOWN ||
            !ResourceCast(graphics_done.Get())->IsRejected(1) ||
            ResourceCast(graphics_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "pipeline shutdown did not reject batch 0's exact signal value"
            );
        }
        if (ResourceCast(copy_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(copy_done.Get())->IsRejected(1) ||
            ResourceCast(copy_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "pipeline shutdown did not drain batch 1 successfully"
            );
        }
        if (readback != expected) {
            throw std::runtime_error(
                "pipeline shutdown Copy batch readback mismatch"
            );
        }
        if (ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "pipeline shutdown callbacks did not retire exactly once"
            );
        }

        if (source_capture.Overflowed() || source_capture.Count() != 1) {
            throw std::runtime_error(
                "shutdown-cancelled batch 0 reached source submission or "
                "batch 1 was not observed exactly once"
            );
        }
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(0);
        if (source_event.batch_sequence == 0 ||
            source_event.source_index != 0 ||
            source_event.original_source_index != 0 ||
            source_event.queue != EQueueType::Copy ||
            source_event.async_queue_scope != async_queue_scope) {
            throw std::runtime_error(
                "pipeline shutdown lost drained batch 1 source identity"
            );
        }

        if (native_capture.Overflowed() || native_capture.Count() != 1) {
            throw std::runtime_error(
                "shutdown-cancelled batch 0 reached native submit or "
                "batch 1 did not submit exactly once"
            );
        }
        const VulkanNativeSubmissionEvent& native_event =
            native_capture.Event(0);
        if (native_event.queue != EQueueType::Copy ||
            native_event.thread_role != ERHIThreadRole::Submission ||
            native_event.thread_id == main_thread_id ||
            !native_event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "pipeline shutdown drained batch 1 outside the Submission owner"
            );
        }

        // Shutdown is the terminal lifecycle boundary for this focused mode.
        // Its return proves the Executor, Translate, Submission, and Completion
        // owners have drained and joined; the stable counters below therefore
        // also prove there is no deferred callback or submission replay.
        if (source_capture.Count() != 1 || native_capture.Count() != 1 ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            success_callbacks[0].load(std::memory_order_acquire) != 0 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
            success_callbacks[1].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "pipeline shutdown left replayable work after owner join"
            );
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        stop_runtime();
        if (sync_waiter.joinable()) {
            sync_waiter.join();
        }
        std::rethrow_exception(failure);
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=BoundedPipelineShutdownCancellation "
        "window=2 native_alias={} overlap_assertion={} "
        "batch0_native_submit=0 batch1_native_submit=1 "
        "batch0_signal=rejected batch1_signal=success "
        "queues=Graphics,Copy copy_translate_owner=Translate "
        "native_owner=Submission readback=verified "
        "callbacks=exactly_once concurrent_sync=drained owners=stopped",
        runtime_native_alias,
        runtime_native_alias ? "blocked" : "verified"
    );
}

void RunShutdownDependencyCancellation() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();

    FenceRef graphics_dependency = device.CreateFence();
    FenceRef copy_dependency     = device.CreateFence();
    FenceRef graphics_done       = device.CreateFence();
    FenceRef copy_done           = device.CreateFence();
    std::atomic<uint32> callbacks{0};
    std::atomic<uint32> success_callbacks{0};
    std::atomic<bool>   sync_returned{false};
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          copy_wait_capture{EQueueType::Copy};
    ScopedDependencyWaitObserver   wait_observer(copy_wait_capture);
    BackendSyncWaitCapture         sync_wait_capture{};
    ScopedBackendSyncWaitObserver  sync_wait_observer(sync_wait_capture);

    CommandList graphics(EQueueType::Graphics);
    graphics.AddCallback([&] {
        callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    graphics.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.Wait(graphics_dependency.Get(), 1)
        .Signal(graphics_done.Get(), 1);

    CommandList copy(EQueueType::Copy);
    copy.AddCallback([&] {
        callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    copy.AddSuccessCallback([&] {
        success_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit copy_submit = copy.Submit();
    copy_submit.Wait(copy_dependency.Get(), 1).Signal(copy_done.Get(), 1);

    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        std::move(copy_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    if (!copy_wait_capture.entered.try_acquire_for(5s) ||
        copy_wait_capture.count.load(std::memory_order_acquire) != 1 ||
        copy_wait_capture.wrong_owner.load(std::memory_order_acquire) ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "Copy did not enter its unpublished dependency wait on the sole "
            "Submission owner"
        );
    }
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(graphics_submit),
        ERHIExecSubmitFlags::FlushGPU
    );

    std::jthread sync_waiter([&] {
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        sync_returned.store(true, std::memory_order_release);
    });
    if (!sync_wait_capture.entered.try_acquire_for(5s) ||
        sync_wait_capture.count.load(std::memory_order_acquire) != 1 ||
        sync_returned.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "concurrent Sync was not registered behind the unpublished "
            "dependency wait"
        );
    }

    // Both dependency values are intentionally never published. Shutdown must
    // cancel Submission-owner host waits, terminalize both packets on their
    // Completion owners, release a concurrent Sync, and join every service
    // thread without a Shutdown <-> Sync dependency cycle.
    RHIExecutor::ShutDown();
    sync_waiter.join();

    if (callbacks.load(std::memory_order_acquire) != 2 ||
        success_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "shutdown dependency cancellation retired callbacks incorrectly"
        );
    }
    if (!ResourceCast(graphics_done.Get())->IsRejected(1) ||
        !ResourceCast(copy_done.Get())->IsRejected(1) ||
        ResourceCast(graphics_done.Get())->IsFailed() ||
        ResourceCast(copy_done.Get())->IsFailed()) {
        throw std::runtime_error(
            "shutdown dependency cancellation did not reject external signal values"
        );
    }
    if (!sync_returned.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "shutdown dependency cancellation did not release concurrent Sync"
        );
    }
    if (native_capture.Count() != 0) {
        throw std::runtime_error(
            "shutdown-cancelled dependencies reached native submit"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=ShutdownDependencyCancellation "
        "queues=Copy,Graphics copy_wait=entered "
        "dependency=unpublished native_submit=0 sync_wait=registered "
        "concurrent_sync=drained"
    );
}

void RunSerialControlPipelineBoundary() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();
    constexpr uint64 async_queue_scope =
        0x5048313546534552ull;
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    SourceSubmissionCapture source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    SubmissionBoundaryCapture boundary_capture{};
    ScopedSubmissionBoundaryObserver boundary_observer(
        boundary_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          wait_capture{EQueueType::Graphics};
    ScopedDependencyWaitObserver   wait_observer(wait_capture);

    constexpr size_t element_count = 16;
    constexpr std::array<uint32, element_count> expected{
        0x15F10001u,
        0x15F10002u,
        0x15F10003u,
        0x15F10004u,
        0x15F10005u,
        0x15F10006u,
        0x15F10007u,
        0x15F10008u,
        0x15F10009u,
        0x15F1000Au,
        0x15F1000Bu,
        0x15F1000Cu,
        0x15F1000Du,
        0x15F1000Eu,
        0x15F1000Fu,
        0x15F10010u,
    };
    std::array<uint32, element_count> readback{};
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>(
        "phase15f_serial_control_source",
        element_count,
        usage
    );
    BufferRef destination = device.CreateBuffer<uint32>(
        "phase15f_serial_control_destination",
        element_count,
        usage
    );
    if (!source.IsValid() || !destination.IsValid()) {
        throw std::runtime_error(
            "Phase15F SerialControl resources were not created"
        );
    }

    FenceRef dependency  = device.CreateFence();
    FenceRef prefix_done = device.CreateFence();
    FenceRef control_done = device.CreateFence();
    FenceRef later_done  = device.CreateFence();
    std::array<std::atomic<uint32>, 3> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 3> success_callbacks{};
    std::binary_semaphore later_translated{0};
    bool dependency_released = false;

    const auto release_dependency = [&] {
        if (dependency_released) {
            return;
        }
        ResourceCast(dependency.Get())->SignalHost(1);
        dependency_released = true;
    };

    try {
        CommandList prefix(EQueueType::Graphics);
        prefix.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix.CopyFrom(
            OwnedBytes(expected),
            source->GetView(),
            "SerialControlBoundaryBlockedPrefix"
        );
        prefix.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        prefix.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit prefix_submit = prefix.Submit();
        prefix_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        prefix_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix_submit.async_queue_scope = async_queue_scope;
        prefix_submit.Wait(dependency.Get(), 1)
            .Signal(prefix_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        if (!wait_capture.entered.try_acquire_for(5s) ||
            wait_capture.count.load(std::memory_order_acquire) != 1 ||
            wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            source_capture.Count() != 0 ||
            boundary_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "SerialControl prefix did not block on the sole "
                "Submission owner"
            );
        }

        CommandList control(EQueueType::Graphics);
        control.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::SerialControl
        );
        control.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        control.CopyFrom(
            source->GetView(),
            destination->GetView(),
            "SerialControlBoundaryCopy"
        );
        control.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        control.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit control_submit = control.Submit();
        control_submit.Signal(control_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(control_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        CommandList later(EQueueType::Graphics);
        later.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        later.CopyFrom(
            destination->GetView(),
            WritableBytes(readback),
            "SerialControlBoundaryLaterReadback"
        );
        later.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &later_translated,
                EQueueType::Graphics
            ),
            "SerialControlBoundaryLaterParallelSource"
        );
        later.AddCallback([&] {
            ordinary_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        later.AddSuccessCallback([&] {
            success_callbacks[2].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit later_submit = later.Submit();
        later_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        later_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        later_submit.async_queue_scope = async_queue_scope;
        later_submit.Signal(later_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(later_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        bool callback_advanced = false;
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            callback_advanced =
                callback_advanced ||
                ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 0 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 0;
        }
        if (later_translated.try_acquire_for(250ms) ||
            callback_advanced ||
            source_capture.Count() != 0 ||
            boundary_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "SerialControl or a later source overtook the blocked "
                "pipeline prefix"
            );
        }

        release_dependency();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

        if (!later_translated.try_acquire() ||
            readback != expected) {
            throw std::runtime_error(
                "SerialControl did not preserve prefix/control/later "
                "GPU ordering"
            );
        }
        if (ResourceCast(prefix_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(control_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(later_done.Get())->HostWait(1) != VK_SUCCESS) {
            throw std::runtime_error(
                "SerialControl boundary signals did not succeed"
            );
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "SerialControl boundary callbacks were not retired "
                    "exactly once"
                );
            }
        }

        if (source_capture.Overflowed() ||
            source_capture.Count() != 2) {
            throw std::runtime_error(
                "SerialControl boundary lost a parallel source submission"
            );
        }
        const VulkanSourceSubmissionEvent& prefix_source =
            source_capture.Event(0);
        const VulkanSourceSubmissionEvent& later_source =
            source_capture.Event(1);
        if (prefix_source.batch_sequence == 0 ||
            later_source.batch_sequence !=
                prefix_source.batch_sequence + 2 ||
            prefix_source.source_index != 0 ||
            later_source.source_index != 0 ||
            prefix_source.queue != EQueueType::Graphics ||
            later_source.queue != EQueueType::Graphics) {
            throw std::runtime_error(
                "SerialControl did not remain between its prefix and "
                "later source"
            );
        }

        if (native_capture.Overflowed() ||
            native_capture.Count() != 3) {
            throw std::runtime_error(
                "SerialControl boundary emitted the wrong native "
                "submission count"
            );
        }
        const uint32 submission_thread_id =
            native_capture.Event(0).thread_id;
        for (size_t index = 0; index < native_capture.Count(); ++index) {
            const VulkanNativeSubmissionEvent& event =
                native_capture.Event(index);
            if (event.queue != EQueueType::Graphics ||
                event.thread_role != ERHIThreadRole::Submission ||
                event.thread_id != submission_thread_id ||
                event.thread_id == main_thread_id ||
                !event.outcome.WasSubmitted()) {
                throw std::runtime_error(
                    "SerialControl native submission order or owner changed"
                );
            }
        }

        if (!boundary_capture.IsValid() ||
            boundary_capture.Count() != 2) {
            throw std::runtime_error(
                "SerialControl emitted the wrong boundary event count"
            );
        }
        const VulkanSubmissionBoundaryEvent& dispatch =
            boundary_capture.Event(0);
        const VulkanSubmissionBoundaryEvent& terminal =
            boundary_capture.Event(1);
        const uint64 control_batch_sequence =
            prefix_source.batch_sequence + 1;
        if (dispatch.batch_sequence != control_batch_sequence ||
            terminal.batch_sequence != control_batch_sequence ||
            dispatch.operation_index != 0 ||
            terminal.operation_index != 0 ||
            dispatch.kind !=
                EVulkanSubmissionBoundaryKind::SerialControl ||
            terminal.kind !=
                EVulkanSubmissionBoundaryKind::SerialControl ||
            dispatch.queue != EQueueType::Graphics ||
            terminal.queue != EQueueType::Graphics ||
            dispatch.phase !=
                EVulkanSubmissionBoundaryPhase::Dispatch ||
            terminal.phase !=
                EVulkanSubmissionBoundaryPhase::Terminal ||
            dispatch.outcome_valid ||
            !terminal.outcome_valid ||
            !terminal.gpu_submitted ||
            !terminal.outcome.Succeeded() ||
            dispatch.dependency_wait_count != 0 ||
            terminal.dependency_wait_count != 0 ||
            dispatch.present_receipt_resolution_attempts != 0 ||
            terminal.present_receipt_resolution_attempts != 0 ||
            dispatch.thread_id != submission_thread_id ||
            terminal.thread_id != submission_thread_id) {
            throw std::runtime_error(
                "SerialControl boundary identity, outcome, or owner changed"
            );
        }

        const size_t sources_before_second_sync =
            source_capture.Count();
        const size_t boundaries_before_second_sync =
            boundary_capture.Count();
        const size_t native_before_second_sync =
            native_capture.Count();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        bool callback_replayed = false;
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            callback_replayed =
                callback_replayed ||
                ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1;
        }
        if (source_capture.Count() != sources_before_second_sync ||
            boundary_capture.Count() !=
                boundaries_before_second_sync ||
            native_capture.Count() != native_before_second_sync ||
            callback_replayed) {
            throw std::runtime_error(
                "a second RHI Sync replayed SerialControl boundary work"
            );
        }

        LOG_INFO(
            "[TESTCASE][PASS] name=SerialControlPipelineBoundary "
            "order=Prefix,SerialControl,Later owner=Submission "
            "later_translate=blocked readback=verified signals=success "
            "boundary_events=2 native_submits=3 replay=0"
        );
    } catch (...) {
        release_dependency();
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        throw;
    }
}

void RunPresentSourceContractRejection() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();
    constexpr ETextureUsageFlags present_source_usage =
        ETextureUsageFlags::PRESENTATION_SOURCE |
        ETextureUsageFlags::TRANSFER_SRC |
        ETextureUsageFlags::TRANSFER_DST;
    TextureRef valid_source = device.CreateTexture(
        "present_source_contract_valid",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef rejected_export_source = device.CreateTexture(
        "present_source_contract_rejected_export",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef wrong_queue_source = device.CreateTexture(
        "present_source_contract_wrong_queue_export",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef backend_tracked_source = device.CreateTexture(
        "present_source_contract_backend_tracked",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef rejected_backend_tracked_source =
        device.CreateTexture(
            "present_source_contract_rejected_backend_tracked",
            Extent3D(4, 4, 1),
            PF_R8G8B8A8_UNORM,
            present_source_usage
        );
    TextureRef backend_nonterminal_source =
        device.CreateTexture(
            "present_source_contract_backend_nonterminal",
            Extent3D(4, 4, 1),
            PF_R8G8B8A8_UNORM,
            present_source_usage |
                ETextureUsageFlags::COLOR_ATTACHMENT
        );
    TextureRef rejected_suffix_source = device.CreateTexture(
        "present_source_contract_rejected_suffix",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef copy_commit_source = device.CreateTexture(
        "present_source_contract_copy_commit",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        present_source_usage
    );
    TextureRef mutation_copy_source = device.CreateTexture(
        "present_source_contract_mutation_copy_source",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST |
            ETextureUsageFlags::COLOR_ATTACHMENT
    );
    TextureRef missing_usage_source = device.CreateTexture(
        "present_source_contract_missing_usage",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
    TextureRef missing_transfer_source = device.CreateTexture(
        "present_source_contract_missing_transfer_src",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_DST
    );
    TextureRef multisampled_source = device.CreateTexture(
        "present_source_contract_multisampled",
        TextureInfo{
            ETextureDimension::TEX_2D,
            present_source_usage,
            PF_R8G8B8A8_UNORM,
            EClearAttachment{},
            Extent3D(4, 4, 1),
            1,
            1,
            4
        }
    );
    TextureRef incompatible_format_source = device.CreateTexture(
        "present_source_contract_incompatible_format",
        Extent3D(4, 4, 1),
        PF_R16G16B16A16_SFLOAT,
        present_source_usage
    );
    TextureRef compressed_source = device.CreateTexture(
        "present_source_contract_compressed",
        Extent3D(4, 4, 1),
        PF_BC1_RGBA_UNORM_BLOCK,
        present_source_usage
    );
    SwapchainRef scripted_swapchain{
        MoerNew(HeadlessScriptedSwapchain)()
    };
    SwapchainRef compressed_stride_swapchain{
        MoerNew(HeadlessScriptedSwapchain)()
    };
    compressed_stride_swapchain->format =
        PF_R16G16B16A16_SFLOAT;
    if (!valid_source.IsValid() ||
        !rejected_export_source.IsValid() ||
        !wrong_queue_source.IsValid() ||
        !backend_tracked_source.IsValid() ||
        !rejected_backend_tracked_source.IsValid() ||
        !backend_nonterminal_source.IsValid() ||
        !rejected_suffix_source.IsValid() ||
        !copy_commit_source.IsValid() ||
        !mutation_copy_source.IsValid() ||
        !missing_usage_source.IsValid() ||
        !missing_transfer_source.IsValid() ||
        !multisampled_source.IsValid() ||
        !incompatible_format_source.IsValid() ||
        !compressed_source.IsValid() ||
        !scripted_swapchain.IsValid() ||
        !compressed_stride_swapchain.IsValid()) {
        throw std::runtime_error(
            "Present source-contract resources were not created"
        );
    }

    ScriptedPresentCapture scripted_capture(
        VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Recreate,
                VK_ERROR_OUT_OF_DATE_KHR,
            },
        }
    );
    ScopedScriptedPresentOverride scripted_override(
        scripted_capture,
        true
    );

    uint32 rejected_count       = 0;
    bool   copy_commit_verified = false;
    const auto expect_rejected =
        [&](
            TextureView       _source,
            std::string_view  _case_name,
            const SwapchainRef& _swapchain
        ) {
            const uint32 scripted_before =
                scripted_capture.count.load(
                    std::memory_order_acquire
                );
            PresentReceiptRef receipt = MakeShared<PresentReceipt>();
            RHIExecutor::Get().Present(
                RHIPresentRequest(
                    _swapchain,
                    _source,
                    receipt
                ),
                true
            );
            const PresentReceiptResult result =
                receipt->WaitForSubmission(5s);
            if (!result.resolved || result.submitted ||
                result.recreate_swapchain ||
                receipt->ResolutionAttemptCount() != 1 ||
                scripted_capture.count.load(
                    std::memory_order_acquire
                ) != scripted_before) {
                throw std::runtime_error(
                    "Present source contract did not reject " +
                    std::string(_case_name) +
                    " before the scripted override"
                );
            }
            ++rejected_count;
        };

    expect_rejected(
        TextureView{},
        "null source texture",
        scripted_swapchain
    );

    expect_rejected(
        missing_usage_source->GetView(),
        "missing PRESENTATION_SOURCE usage",
        scripted_swapchain
    );

    expect_rejected(
        missing_transfer_source->GetView(),
        "missing TRANSFER_SRC usage",
        scripted_swapchain
    );

    expect_rejected(
        multisampled_source->GetView(),
        "multisampled source",
        scripted_swapchain
    );

    expect_rejected(
        incompatible_format_source->GetView(),
        "size-incompatible format",
        scripted_swapchain
    );

    expect_rejected(
        compressed_source->GetView(),
        "compressed format with matching block stride",
        compressed_stride_swapchain
    );

    TextureView invalid_mip = valid_source->GetView();
    invalid_mip.mip_level   = 1;
    expect_rejected(
        invalid_mip,
        "nonzero mip",
        scripted_swapchain
    );

    TextureView invalid_layer = valid_source->GetView();
    invalid_layer.array_layer = 1;
    expect_rejected(
        invalid_layer,
        "nonzero array layer",
        scripted_swapchain
    );

    TextureView invalid_offset = valid_source->GetView();
    invalid_offset.offset.x    = 1;
    expect_rejected(
        invalid_offset,
        "nonzero offset",
        scripted_swapchain
    );

    TextureView invalid_extent = valid_source->GetView();
    --invalid_extent.extent.x;
    expect_rejected(
        invalid_extent,
        "partial extent",
        scripted_swapchain
    );

    expect_rejected(
        valid_source->GetView(),
        "fresh source without an accepted producer export",
        scripted_swapchain
    );

    const auto make_backend_tracked_publication =
        [&](const TextureRef& _texture,
            const FenceRef&   _signal,
            uint64            _signal_value) {
            CommandList producer(EQueueType::Graphics);
            producer.ClearResource(
                _texture->GetView(),
                float4(0.f, 0.f, 0.f, 1.f)
            );
            Array<ReadTexture> publications{
                ReadTexture{
                    _texture->GetView(
                        0,
                        static_cast<uint8>(
                            _texture->GetNumMips()
                        )
                    ),
                    ETextureState::TRANSFER,
                    true,
                },
            };
            producer.TextureBarriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Copy,
                std::move(publications),
                {}
            );
            CmdSubmit submit = producer.Submit();
            if (submit.HasExplicitResourceStateOwnership()) {
                throw std::runtime_error(
                    "linear Present publication changed to Explicit ownership"
                );
            }
            submit.Signal(_signal.Get(), _signal_value);
            return submit;
        };

    const auto create_bindless_present_source =
        [&](std::string_view _name) {
            return device.CreateTexture(
                _name,
                Extent3D(4, 4, 1),
                PF_R8G8B8A8_UNORM,
                present_source_usage |
                    ETextureUsageFlags::SAMPLED
            );
        };
    TextureRef bindless_access_then_add =
        create_bindless_present_source(
            "present_bindless_access_then_add"
        );
    TextureRef bindless_add_then_access =
        create_bindless_present_source(
            "present_bindless_add_then_access"
        );
    TextureRef bindless_duplicate =
        create_bindless_present_source(
            "present_bindless_duplicate"
        );
    TextureRef bindless_rejected_add =
        create_bindless_present_source(
            "present_bindless_rejected_add"
        );
    TextureRef bindless_rejected_free =
        create_bindless_present_source(
            "present_bindless_rejected_free"
        );
    TextureRef bindless_future_allocation =
        create_bindless_present_source(
            "present_bindless_future_allocation"
        );
    TextureRef bindless_segment_access_add =
        create_bindless_present_source(
            "present_bindless_segment_access_add"
        );
    TextureRef bindless_segment_add_access =
        create_bindless_present_source(
            "present_bindless_segment_add_access"
        );
    TextureRef bindless_accepted_add_prefix =
        create_bindless_present_source(
            "present_bindless_accepted_add_prefix"
        );
    TextureRef bindless_accepted_free_prefix =
        create_bindless_present_source(
            "present_bindless_accepted_free_prefix"
        );
    TextureRef bindless_cross_queue =
        create_bindless_present_source(
            "present_bindless_cross_queue"
        );
    TextureRef bindless_parallel_record =
        create_bindless_present_source(
            "present_bindless_parallel_record"
        );
    BindlessArrayRef bindless =
        device.CreateBindlessArray(32);
    BindlessArrayRef future_bindless =
        device.CreateBindlessArray(8);
    if (!bindless_access_then_add ||
        !bindless_add_then_access ||
        !bindless_duplicate ||
        !bindless_rejected_add ||
        !bindless_rejected_free ||
        !bindless_future_allocation ||
        !bindless_segment_access_add ||
        !bindless_segment_add_access ||
        !bindless_accepted_add_prefix ||
        !bindless_accepted_free_prefix ||
        !bindless_cross_queue ||
        !bindless_parallel_record ||
        !bindless ||
        !future_bindless) {
        throw std::runtime_error(
            "bindless Present contract resources were not created"
        );
    }

    const Sampler bindless_sampler(
        SF_LINEAR, SAM_CLAMP_TO_EDGE
    );
    const auto add_bindless_access =
        [&](CommandList&          _commands,
            const BindlessArrayRef& _bindless,
            std::string_view      _name,
            EQueueType _queue = EQueueType::Graphics) {
            _commands.AddCustomCommand(
                MakeUnique<
                    BindlessPresentationAccessProbeCommand>(
                    _bindless, _queue
                ),
                _name
            );
        };
    const auto submit_accepted_commands =
        [&](CommandList& _commands, std::string_view _name) {
            FenceRef accepted = device.CreateFence();
            if (!accepted) {
                throw std::runtime_error(
                    "failed to create bindless Present acceptance fence"
                );
            }
            CmdSubmit submit = _commands.Submit();
            submit.Signal(accepted.Get(), 1);
            RHIExecutor::Get().Submit(
                _commands.GetQueueType(),
                std::move(submit),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            auto* vk_accepted = ResourceCast(accepted.Get());
            if (!vk_accepted->WaitSubmitted(1) ||
                vk_accepted->IsRejected(1) ||
                vk_accepted->IsFailed()) {
                throw std::runtime_error(
                    std::string(_name) +
                    " did not reach native submission"
                );
            }
        };
    const auto submit_dependency_rejected_commands =
        [&](CommandList& _commands, std::string_view _name) {
            FenceRef dependency = device.CreateFence();
            FenceRef rejected   = device.CreateFence();
            if (!dependency || !rejected) {
                throw std::runtime_error(
                    "failed to create bindless Present rejection fences"
                );
            }
            dependency->Reject(1);
            CmdSubmit submit = _commands.Submit();
            submit.Wait(dependency.Get(), 1);
            submit.Signal(rejected.Get(), 1);
            RHIExecutor::Get().Submit(
                _commands.GetQueueType(),
                std::move(submit),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            auto* vk_rejected = ResourceCast(rejected.Get());
            if (!vk_rejected->IsRejected(1) ||
                vk_rejected->IsFailed()) {
                throw std::runtime_error(
                    std::string(_name) +
                    " was not recoverably rejected"
                );
            }
        };
    const auto publish_bindless_source =
        [&](const TextureRef& _texture,
            std::string_view  _name) {
            FenceRef accepted = device.CreateFence();
            if (!accepted) {
                throw std::runtime_error(
                    "failed to create bindless publication fence"
                );
            }
            RHIExecutor::Get().Submit(
                EQueueType::Graphics,
                make_backend_tracked_publication(
                    _texture, accepted, 1
                ),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            if (!ResourceCast(accepted.Get())->WaitSubmitted(1) ||
                !ResourceCast(_texture.Get())
                     ->IsPresentationSourceReady()) {
                throw std::runtime_error(
                    std::string(_name) +
                    " failed to publish Present readiness"
                );
            }
        };
    const auto require_bindless_ready =
        [&](const TextureRef& _texture,
            bool              _expected,
            std::string_view  _case_name) {
            const bool ready =
                ResourceCast(_texture.Get())
                    ->IsPresentationSourceReady();
            if (ready != _expected) {
                throw std::runtime_error(
                    std::string(_case_name) +
                    (_expected ?
                         " unexpectedly lost Present readiness" :
                         " left stale Present readiness")
                );
            }
        };

    // Access -> Add: the access sees the accepted entry membership, not the
    // frontend allocation created later in this same command source.
    publish_bindless_source(
        bindless_access_then_add, "bindless Access->Add"
    );
    CommandList access_then_add(EQueueType::Graphics);
    add_bindless_access(
        access_then_add,
        bindless,
        "BindlessPresentAccessThenAdd"
    );
    const uint access_then_add_slot =
        bindless->AllocateTexture(
            bindless_access_then_add->GetView(),
            bindless_sampler
        );
    access_then_add.UpdateBindlessArray(bindless);
    submit_accepted_commands(
        access_then_add, "bindless Access->Add"
    );
    require_bindless_ready(
        bindless_access_then_add,
        true,
        "bindless Access->Add"
    );

    // Access -> Free still observes the old accepted slot before the update.
    CommandList access_then_free(EQueueType::Graphics);
    add_bindless_access(
        access_then_free,
        bindless,
        "BindlessPresentAccessThenFree"
    );
    bindless->UnbindTexture(access_then_add_slot);
    access_then_free.UpdateBindlessArray(bindless);
    submit_accepted_commands(
        access_then_free, "bindless Access->Free"
    );
    require_bindless_ready(
        bindless_access_then_add,
        false,
        "bindless Access->Free"
    );

    // Add -> Access sees the new descriptor membership from the preceding
    // update command in the same source.
    publish_bindless_source(
        bindless_add_then_access, "bindless Add->Access"
    );
    const uint add_then_access_slot =
        bindless->AllocateTexture(
            bindless_add_then_access->GetView(),
            bindless_sampler
        );
    CommandList add_then_access(EQueueType::Graphics);
    add_then_access.UpdateBindlessArray(bindless);
    add_bindless_access(
        add_then_access,
        bindless,
        "BindlessPresentAddThenAccess"
    );
    submit_accepted_commands(
        add_then_access, "bindless Add->Access"
    );
    require_bindless_ready(
        bindless_add_then_access,
        false,
        "bindless Add->Access"
    );

    // Free -> Access no longer sees the removed accepted slot.
    publish_bindless_source(
        bindless_add_then_access, "bindless Free->Access"
    );
    bindless->UnbindTexture(add_then_access_slot);
    CommandList free_then_access(EQueueType::Graphics);
    free_then_access.UpdateBindlessArray(bindless);
    add_bindless_access(
        free_then_access,
        bindless,
        "BindlessPresentFreeThenAccess"
    );
    submit_accepted_commands(
        free_then_access, "bindless Free->Access"
    );
    require_bindless_ready(
        bindless_add_then_access,
        true,
        "bindless Free->Access"
    );

    // Two descriptor slots can reference one texture. Removing one slot must
    // retain membership; removing the final slot must close it.
    const uint duplicate_slot_a =
        bindless->AllocateTexture(
            bindless_duplicate->GetView(), bindless_sampler
        );
    const uint duplicate_slot_b =
        bindless->AllocateTexture(
            bindless_duplicate->GetView(), bindless_sampler
        );
    CommandList add_duplicate(EQueueType::Graphics);
    add_duplicate.UpdateBindlessArray(bindless);
    submit_accepted_commands(
        add_duplicate, "bindless duplicate add"
    );
    publish_bindless_source(
        bindless_duplicate, "bindless duplicate first free"
    );
    bindless->UnbindTexture(duplicate_slot_a);
    CommandList free_one_duplicate(EQueueType::Graphics);
    free_one_duplicate.UpdateBindlessArray(bindless);
    add_bindless_access(
        free_one_duplicate,
        bindless,
        "BindlessPresentFreeOneDuplicate"
    );
    submit_accepted_commands(
        free_one_duplicate, "bindless duplicate first free"
    );
    require_bindless_ready(
        bindless_duplicate,
        false,
        "bindless duplicate first free"
    );
    publish_bindless_source(
        bindless_duplicate, "bindless duplicate final free"
    );
    bindless->UnbindTexture(duplicate_slot_b);
    CommandList free_last_duplicate(EQueueType::Graphics);
    free_last_duplicate.UpdateBindlessArray(bindless);
    add_bindless_access(
        free_last_duplicate,
        bindless,
        "BindlessPresentFreeLastDuplicate"
    );
    submit_accepted_commands(
        free_last_duplicate, "bindless duplicate final free"
    );
    require_bindless_ready(
        bindless_duplicate,
        true,
        "bindless duplicate final free"
    );

    // A rejected add never enters accepted descriptor membership.
    publish_bindless_source(
        bindless_rejected_add, "bindless rejected add"
    );
    (void)bindless->AllocateTexture(
        bindless_rejected_add->GetView(), bindless_sampler
    );
    CommandList rejected_add(EQueueType::Graphics);
    rejected_add.UpdateBindlessArray(bindless);
    submit_dependency_rejected_commands(
        rejected_add, "bindless rejected add"
    );
    CommandList access_after_rejected_add(
        EQueueType::Graphics
    );
    add_bindless_access(
        access_after_rejected_add,
        bindless,
        "BindlessPresentAccessAfterRejectedAdd"
    );
    submit_accepted_commands(
        access_after_rejected_add,
        "bindless access after rejected add"
    );
    require_bindless_ready(
        bindless_rejected_add,
        true,
        "bindless access after rejected add"
    );

    // Conversely, a rejected free preserves the last accepted membership.
    const uint rejected_free_slot =
        bindless->AllocateTexture(
            bindless_rejected_free->GetView(),
            bindless_sampler
        );
    CommandList accepted_before_rejected_free(
        EQueueType::Graphics
    );
    accepted_before_rejected_free.UpdateBindlessArray(bindless);
    submit_accepted_commands(
        accepted_before_rejected_free,
        "bindless accepted add before rejected free"
    );
    publish_bindless_source(
        bindless_rejected_free, "bindless rejected free"
    );
    bindless->UnbindTexture(rejected_free_slot);
    CommandList rejected_free(EQueueType::Graphics);
    rejected_free.UpdateBindlessArray(bindless);
    submit_dependency_rejected_commands(
        rejected_free, "bindless rejected free"
    );
    CommandList access_after_rejected_free(
        EQueueType::Graphics
    );
    add_bindless_access(
        access_after_rejected_free,
        bindless,
        "BindlessPresentAccessAfterRejectedFree"
    );
    submit_accepted_commands(
        access_after_rejected_free,
        "bindless access after rejected free"
    );
    require_bindless_ready(
        bindless_rejected_free,
        false,
        "bindless access after rejected free"
    );

    // Seal S0 before a later frontend allocation. Even if Allocate happens
    // before S0 reaches Translate, S0 must read accepted (empty) membership.
    publish_bindless_source(
        bindless_future_allocation,
        "bindless future allocation"
    );
    CommandList future_access_commands(EQueueType::Graphics);
    add_bindless_access(
        future_access_commands,
        future_bindless,
        "BindlessPresentFutureAllocationAccess"
    );
    CmdSubmit sealed_future_access =
        future_access_commands.Submit();
    (void)future_bindless->AllocateTexture(
        bindless_future_allocation->GetView(),
        bindless_sampler
    );
    CommandList rejected_future_update(
        EQueueType::Graphics
    );
    rejected_future_update.UpdateBindlessArray(
        future_bindless
    );
    FenceRef future_access_done = device.CreateFence();
    sealed_future_access.Signal(
        future_access_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(sealed_future_access),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(future_access_done.Get())
             ->WaitSubmitted(1)) {
        throw std::runtime_error(
            "sealed bindless access was not submitted"
        );
    }
    require_bindless_ready(
        bindless_future_allocation,
        true,
        "sealed bindless access with a future allocation"
    );
    submit_dependency_rejected_commands(
        rejected_future_update,
        "future bindless update"
    );
    CommandList access_after_rejected_future(
        EQueueType::Graphics
    );
    add_bindless_access(
        access_after_rejected_future,
        future_bindless,
        "BindlessPresentAccessAfterRejectedFutureUpdate"
    );
    submit_accepted_commands(
        access_after_rejected_future,
        "access after rejected future bindless update"
    );
    require_bindless_ready(
        bindless_future_allocation,
        true,
        "access after rejected future bindless update"
    );

    constexpr uint64 bindless_segment_scope =
        0x5042494e444c4553ull;
    publish_bindless_source(
        bindless_segment_access_add,
        "segmented bindless Access->Add"
    );
    CommandList segmented_access_add(EQueueType::Graphics);
    segmented_access_add.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    add_bindless_access(
        segmented_access_add,
        bindless,
        "SegmentedBindlessAccess"
    );
    (void)bindless->AllocateTexture(
        bindless_segment_access_add->GetView(),
        bindless_sampler
    );
    segmented_access_add.UpdateBindlessArray(bindless);
    CmdSubmit segmented_access_add_submit =
        segmented_access_add.Submit();
    segmented_access_add_submit.async_queue_scope =
        bindless_segment_scope;
    segmented_access_add_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    FenceRef segmented_access_add_done =
        device.CreateFence();
    segmented_access_add_submit.Signal(
        segmented_access_add_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(segmented_access_add_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(segmented_access_add_done.Get())
             ->WaitSubmitted(1)) {
        throw std::runtime_error(
            "segmented bindless Access->Add was not submitted"
        );
    }
    require_bindless_ready(
        bindless_segment_access_add,
        true,
        "segmented bindless Access->Add"
    );

    publish_bindless_source(
        bindless_segment_add_access,
        "segmented bindless Add->Access"
    );
    (void)bindless->AllocateTexture(
        bindless_segment_add_access->GetView(),
        bindless_sampler
    );
    CommandList segmented_add_access(EQueueType::Graphics);
    segmented_add_access.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    segmented_add_access.UpdateBindlessArray(bindless);
    add_bindless_access(
        segmented_add_access,
        bindless,
        "SegmentedBindlessAccessAfterAdd"
    );
    CmdSubmit segmented_add_access_submit =
        segmented_add_access.Submit();
    segmented_add_access_submit.async_queue_scope =
        bindless_segment_scope + 1;
    segmented_add_access_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    FenceRef segmented_add_access_done =
        device.CreateFence();
    segmented_add_access_submit.Signal(
        segmented_add_access_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(segmented_add_access_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(segmented_add_access_done.Get())
             ->WaitSubmitted(1)) {
        throw std::runtime_error(
            "segmented bindless Add->Access was not submitted"
        );
    }
    require_bindless_ready(
        bindless_segment_add_access,
        false,
        "segmented bindless Add->Access"
    );

    // A materialized accepted membership prefix survives an opaque rejected
    // suffix and is visible to the next independently accepted source.
    publish_bindless_source(
        bindless_accepted_add_prefix,
        "bindless accepted add prefix"
    );
    (void)bindless->AllocateTexture(
        bindless_accepted_add_prefix->GetView(),
        bindless_sampler
    );
    CommandList accepted_add_prefix(EQueueType::Graphics);
    accepted_add_prefix.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    accepted_add_prefix.UpdateBindlessArray(bindless);
    accepted_add_prefix.AddCustomCommand(
        MakeUnique<OpaquePresentationStateProbeCommand>(),
        "RejectedAfterAcceptedBindlessAdd"
    );
    CmdSubmit accepted_add_prefix_submit =
        accepted_add_prefix.Submit();
    accepted_add_prefix_submit.async_queue_scope =
        bindless_segment_scope + 2;
    accepted_add_prefix_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    FenceRef accepted_add_prefix_done =
        device.CreateFence();
    accepted_add_prefix_submit.Signal(
        accepted_add_prefix_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(accepted_add_prefix_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(accepted_add_prefix_done.Get())
             ->IsRejected(1) ||
        ResourceCast(accepted_add_prefix_done.Get())
            ->IsFailed()) {
        throw std::runtime_error(
            "bindless accepted add prefix suffix was not "
            "recoverably rejected"
        );
    }
    CommandList access_after_add_prefix(
        EQueueType::Graphics
    );
    add_bindless_access(
        access_after_add_prefix,
        bindless,
        "BindlessPresentAccessAfterAcceptedAddPrefix"
    );
    submit_accepted_commands(
        access_after_add_prefix,
        "access after accepted bindless add prefix"
    );
    require_bindless_ready(
        bindless_accepted_add_prefix,
        false,
        "access after accepted bindless add prefix"
    );

    const uint accepted_free_prefix_slot =
        bindless->AllocateTexture(
            bindless_accepted_free_prefix->GetView(),
            bindless_sampler
        );
    CommandList add_before_free_prefix(
        EQueueType::Graphics
    );
    add_before_free_prefix.UpdateBindlessArray(bindless);
    submit_accepted_commands(
        add_before_free_prefix,
        "add before accepted bindless free prefix"
    );
    publish_bindless_source(
        bindless_accepted_free_prefix,
        "bindless accepted free prefix"
    );
    bindless->UnbindTexture(accepted_free_prefix_slot);
    CommandList accepted_free_prefix(
        EQueueType::Graphics
    );
    accepted_free_prefix.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    accepted_free_prefix.UpdateBindlessArray(bindless);
    accepted_free_prefix.AddCustomCommand(
        MakeUnique<OpaquePresentationStateProbeCommand>(),
        "RejectedAfterAcceptedBindlessFree"
    );
    CmdSubmit accepted_free_prefix_submit =
        accepted_free_prefix.Submit();
    accepted_free_prefix_submit.async_queue_scope =
        bindless_segment_scope + 3;
    accepted_free_prefix_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    FenceRef accepted_free_prefix_done =
        device.CreateFence();
    accepted_free_prefix_submit.Signal(
        accepted_free_prefix_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(accepted_free_prefix_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(accepted_free_prefix_done.Get())
             ->IsRejected(1) ||
        ResourceCast(accepted_free_prefix_done.Get())
            ->IsFailed()) {
        throw std::runtime_error(
            "bindless accepted free prefix suffix was not "
            "recoverably rejected"
        );
    }
    CommandList access_after_free_prefix(
        EQueueType::Graphics
    );
    add_bindless_access(
        access_after_free_prefix,
        bindless,
        "BindlessPresentAccessAfterAcceptedFreePrefix"
    );
    submit_accepted_commands(
        access_after_free_prefix,
        "access after accepted bindless free prefix"
    );
    require_bindless_ready(
        bindless_accepted_free_prefix,
        true,
        "access after accepted bindless free prefix"
    );

    if (device.GetQueueTopology().compute.available) {
        (void)bindless->AllocateTexture(
            bindless_cross_queue->GetView(),
            bindless_sampler
        );
        CommandList cross_queue_add(EQueueType::Graphics);
        cross_queue_add.UpdateBindlessArray(bindless);
        submit_accepted_commands(
            cross_queue_add,
            "Graphics bindless add before Compute access"
        );
        publish_bindless_source(
            bindless_cross_queue,
            "Graphics bindless add before Compute access"
        );
        CommandList compute_access(EQueueType::Compute);
        add_bindless_access(
            compute_access,
            bindless,
            "BindlessPresentComputeAccess",
            EQueueType::Compute
        );
        submit_accepted_commands(
            compute_access,
            "Compute bindless access after Graphics add"
        );
        require_bindless_ready(
            bindless_cross_queue,
            false,
            "Compute bindless access after Graphics add"
        );
    }

    // Carry the source-order program through an actually parallel inner
    // recorder, not just the serial fallback. Four independent copies form a
    // worker-eligible layer while the bindless update/access remain ordered
    // serial islands in the same immutable source.
    constexpr uint64 bindless_parallel_scope =
        bindless_segment_scope + 4;
    constexpr size_t bindless_parallel_copy_count = 4;
    const EBufferUsageFlags bindless_parallel_buffer_usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;
    std::array<BufferRef, bindless_parallel_copy_count>
        bindless_parallel_sources{};
    std::array<BufferRef, bindless_parallel_copy_count>
        bindless_parallel_destinations{};
    for (size_t index = 0;
         index < bindless_parallel_copy_count;
         ++index) {
        bindless_parallel_sources[index] =
            device.CreateBuffer<uint32>(
                "bindless_parallel_source_" +
                    std::to_string(index),
                kElementCount,
                bindless_parallel_buffer_usage
            );
        bindless_parallel_destinations[index] =
            device.CreateBuffer<uint32>(
                "bindless_parallel_destination_" +
                    std::to_string(index),
                kElementCount,
                bindless_parallel_buffer_usage
            );
        if (!bindless_parallel_sources[index] ||
            !bindless_parallel_destinations[index]) {
            throw std::runtime_error(
                "failed to create bindless parallel-record buffers"
            );
        }
    }

    publish_bindless_source(
        bindless_parallel_record,
        "parallel-record bindless Add->Access"
    );
    (void)bindless->AllocateTexture(
        bindless_parallel_record->GetView(),
        bindless_sampler
    );
    CommandList parallel_bindless_access(EQueueType::Graphics);
    parallel_bindless_access.UpdateBindlessArray(bindless);
    add_bindless_access(
        parallel_bindless_access,
        bindless,
        "BindlessPresentParallelRecordAccess"
    );
    for (size_t index = 0;
         index < bindless_parallel_copy_count;
         ++index) {
        parallel_bindless_access.CopyFrom(
            bindless_parallel_sources[index]->GetView(),
            bindless_parallel_destinations[index]->GetView(),
            "BindlessParallelRecordCopy"
        );
    }
    FenceRef parallel_bindless_done = device.CreateFence();
    CmdSubmit parallel_bindless_submit =
        parallel_bindless_access.Submit();
    parallel_bindless_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    parallel_bindless_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    parallel_bindless_submit.async_queue_scope =
        bindless_parallel_scope;
    parallel_bindless_submit.Signal(
        parallel_bindless_done.Get(), 1
    );
    SourceTranslationCapture parallel_bindless_capture(
        bindless_parallel_scope,
        EQueueType::Graphics
    );
    {
        ScopedSourceTranslationObserver translation_observer(
            parallel_bindless_capture
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(parallel_bindless_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }
    auto* vk_parallel_bindless_done =
        ResourceCast(parallel_bindless_done.Get());
    if (!vk_parallel_bindless_done->WaitSubmitted(1) ||
        vk_parallel_bindless_done->IsRejected(1) ||
        vk_parallel_bindless_done->IsFailed()) {
        throw std::runtime_error(
            "parallel bindless source did not reach native submission"
        );
    }
    if (!parallel_bindless_capture.IsValid() ||
        !parallel_bindless_capture.Seen(
            0,
            EVulkanSourceTranslationPhase::Recorded
        )) {
        throw std::runtime_error(
            "parallel bindless source translation was not observed"
        );
    }
    const VulkanSourceTranslationEvent&
        parallel_bindless_translation =
            parallel_bindless_capture.Event(
                0,
                EVulkanSourceTranslationPhase::Recorded
            );
    if (parallel_bindless_translation.async_queue_scope !=
        bindless_parallel_scope) {
        throw std::runtime_error(
            "parallel bindless translation lost its async queue scope"
        );
    }
    if (!parallel_bindless_translation
             .parallel_record_requested ||
        !parallel_bindless_translation
             .parallel_record_planned ||
        !parallel_bindless_translation
             .parallel_record_effective) {
        throw std::runtime_error(
            "bindless source-order program did not traverse the "
            "effective parallel recorder"
        );
    }
    require_bindless_ready(
        bindless_parallel_record,
        false,
        "parallel-record bindless Add->Access"
    );

    FenceRef backend_tracked_done = device.CreateFence();
    PresentReceiptRef backend_tracked_receipt =
        MakeShared<PresentReceipt>();
    RHIPresentRequest backend_tracked_present(
        scripted_swapchain,
        backend_tracked_source->GetView(),
        backend_tracked_receipt
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        make_backend_tracked_publication(
            backend_tracked_source,
            backend_tracked_done,
            1
        ),
        ERHIExecSubmitFlags::FlushGPU,
        &backend_tracked_present
    );
    const PresentReceiptResult backend_tracked_result =
        backend_tracked_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!ResourceCast(backend_tracked_done.Get())
             ->WaitSubmitted(1) ||
        !ResourceCast(backend_tracked_source.Get())
             ->IsPresentationSourceReady() ||
        !backend_tracked_result.resolved ||
        backend_tracked_result.submitted ||
        !backend_tracked_result.recreate_swapchain ||
        backend_tracked_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(
            std::memory_order_acquire
        ) != 1) {
        throw std::runtime_error(
            "accepted BackendTracked producer did not publish readiness "
            "before its same-batch Present"
        );
    }

    // An independently accepted mutation must close the publication from the
    // previous packet even when no trailing BarrierCmd describes that touch.
    FenceRef accepted_mutation_done = device.CreateFence();
    CommandList accepted_mutation(EQueueType::Graphics);
    accepted_mutation.ClearResource(
        backend_tracked_source->GetView(),
        float4(0.25f, 0.f, 0.f, 1.f)
    );
    CmdSubmit accepted_mutation_submit =
        accepted_mutation.Submit();
    accepted_mutation_submit.Signal(
        accepted_mutation_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(accepted_mutation_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(accepted_mutation_done.Get())
             ->WaitSubmitted(1) ||
        ResourceCast(backend_tracked_source.Get())
            ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "an accepted standalone Clear left stale Present readiness"
        );
    }
    expect_rejected(
        backend_tracked_source->GetView(),
        "source cleared by a later accepted packet",
        scripted_swapchain
    );

    // Re-publish, then reject a later mutation before native acceptance. The
    // rejected packet must not speculatively clear the accepted readiness.
    FenceRef republished_done = device.CreateFence();
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        make_backend_tracked_publication(
            backend_tracked_source,
            republished_done,
            1
        ),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(republished_done.Get())->WaitSubmitted(1) ||
        !ResourceCast(backend_tracked_source.Get())
             ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "accepted re-publication did not restore Present readiness"
        );
    }

    FenceRef rejected_mutation_dependency = device.CreateFence();
    FenceRef rejected_mutation_done       = device.CreateFence();
    rejected_mutation_dependency->Reject(1);
    CommandList rejected_mutation(EQueueType::Graphics);
    rejected_mutation.ClearResource(
        backend_tracked_source->GetView(),
        float4(0.f, 0.25f, 0.f, 1.f)
    );
    CmdSubmit rejected_mutation_submit =
        rejected_mutation.Submit();
    rejected_mutation_submit.Wait(
        rejected_mutation_dependency.Get(), 1
    );
    rejected_mutation_submit.Signal(
        rejected_mutation_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(rejected_mutation_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(rejected_mutation_done.Get())
             ->IsRejected(1) ||
        ResourceCast(rejected_mutation_done.Get())->IsFailed() ||
        !ResourceCast(backend_tracked_source.Get())
             ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "a rejected standalone Clear polluted accepted Present readiness"
        );
    }

    const uint32 scripted_before_rejected_mutation_present =
        scripted_capture.count.load(std::memory_order_acquire);
    PresentReceiptRef rejected_mutation_receipt =
        MakeShared<PresentReceipt>();
    RHIExecutor::Get().Present(
        RHIPresentRequest(
            scripted_swapchain,
            backend_tracked_source->GetView(),
            rejected_mutation_receipt
        ),
        true
    );
    const PresentReceiptResult rejected_mutation_result =
        rejected_mutation_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!rejected_mutation_result.resolved ||
        rejected_mutation_result.submitted ||
        !rejected_mutation_result.recreate_swapchain ||
        rejected_mutation_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(std::memory_order_acquire) !=
            scripted_before_rejected_mutation_present + 1) {
        throw std::runtime_error(
            "a rejected standalone Clear hid the last accepted publication"
        );
    }

    // Keep the fixture on Graphics: the source was published there, and this
    // test targets state-ledger invalidation rather than queue-family transfer
    // ownership (covered separately by the explicit multi-queue tests).
    constexpr EQueueType mutation_queue = EQueueType::Graphics;
    FenceRef accepted_copy_mutation_done = device.CreateFence();
    CommandList accepted_copy_mutation(mutation_queue);
    accepted_copy_mutation.CopyFrom(
        mutation_copy_source->GetView(),
        backend_tracked_source->GetView(),
        "PresentSourceAcceptedCopyMutation"
    );
    CmdSubmit accepted_copy_mutation_submit =
        accepted_copy_mutation.Submit();
    accepted_copy_mutation_submit.Signal(
        accepted_copy_mutation_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        mutation_queue,
        std::move(accepted_copy_mutation_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(accepted_copy_mutation_done.Get())
             ->WaitSubmitted(1) ||
        ResourceCast(backend_tracked_source.Get())
            ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "an accepted TextureToTexture copy left stale Present readiness"
        );
    }
    expect_rejected(
        backend_tracked_source->GetView(),
        "source overwritten by an accepted texture copy",
        scripted_swapchain
    );

    FenceRef rejected_backend_dependency = device.CreateFence();
    FenceRef rejected_backend_done = device.CreateFence();
    rejected_backend_dependency->Reject(1);
    CmdSubmit rejected_backend_submit =
        make_backend_tracked_publication(
            rejected_backend_tracked_source,
            rejected_backend_done,
            1
        );
    rejected_backend_submit.Wait(
        rejected_backend_dependency.Get(),
        1
    );
    PresentReceiptRef rejected_backend_receipt =
        MakeShared<PresentReceipt>();
    RHIPresentRequest rejected_backend_present(
        scripted_swapchain,
        rejected_backend_tracked_source->GetView(),
        rejected_backend_receipt
    );
    const uint32 scripted_before_rejected_backend =
        scripted_capture.count.load(std::memory_order_acquire);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(rejected_backend_submit),
        ERHIExecSubmitFlags::FlushGPU,
        &rejected_backend_present
    );
    const PresentReceiptResult rejected_backend_result =
        rejected_backend_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!ResourceCast(rejected_backend_done.Get())
             ->IsRejected(1) ||
        ResourceCast(rejected_backend_done.Get())->IsFailed() ||
        ResourceCast(rejected_backend_tracked_source.Get())
            ->IsPresentationSourceReady() ||
        !rejected_backend_result.resolved ||
        rejected_backend_result.submitted ||
        rejected_backend_result.recreate_swapchain ||
        rejected_backend_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(
            std::memory_order_acquire
        ) != scripted_before_rejected_backend) {
        throw std::runtime_error(
            "rejected BackendTracked producer published readiness or "
            "reached its same-batch Present"
        );
    }

    FenceRef backend_nonterminal_done = device.CreateFence();
    CommandList backend_nonterminal_producer(
        EQueueType::Graphics
    );
    backend_nonterminal_producer.ClearResource(
        backend_nonterminal_source->GetView(),
        float4(0.f, 0.f, 0.f, 1.f)
    );
    backend_nonterminal_producer.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Copy,
        Array<ReadTexture>{
            ReadTexture{
                backend_nonterminal_source->GetView(),
                ETextureState::TRANSFER,
                true,
            },
        },
        {}
    );
    backend_nonterminal_producer.ClearResource(
        backend_nonterminal_source->GetView(),
        float4(0.f, 0.f, 0.25f, 1.f)
    );
    CmdSubmit backend_nonterminal_submit =
        backend_nonterminal_producer.Submit();
    backend_nonterminal_submit.Signal(
        backend_nonterminal_done.Get(), 1
    );
    PresentReceiptRef backend_nonterminal_receipt =
        MakeShared<PresentReceipt>();
    RHIPresentRequest backend_nonterminal_present(
        scripted_swapchain,
        backend_nonterminal_source->GetView(),
        backend_nonterminal_receipt
    );
    const uint32 scripted_before_backend_nonterminal =
        scripted_capture.count.load(std::memory_order_acquire);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(backend_nonterminal_submit),
        ERHIExecSubmitFlags::FlushGPU,
        &backend_nonterminal_present
    );
    const PresentReceiptResult backend_nonterminal_result =
        backend_nonterminal_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!ResourceCast(backend_nonterminal_done.Get())
             ->WaitSubmitted(1) ||
        ResourceCast(backend_nonterminal_source.Get())
            ->IsPresentationSourceReady() ||
        !backend_nonterminal_result.resolved ||
        backend_nonterminal_result.submitted ||
        backend_nonterminal_result.recreate_swapchain ||
        backend_nonterminal_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(
            std::memory_order_acquire
        ) != scripted_before_backend_nonterminal) {
        throw std::runtime_error(
            "a BackendTracked publication followed by a terminal write "
            "remained ready or reached Present"
        );
    }

    const auto submit_source_state =
        [&](const TextureRef& _texture,
            EQueueType        _queue,
            BarrierState      _source,
            BarrierState _destination,
            bool         _publish_external_state,
            uint64       _signal_value) {
            FenceRef accepted = device.CreateFence();
            BarrierCreateInfo transition =
                BarrierCreateInfo::Transition(
                    _texture->GetView(),
                    _source,
                    _destination,
                    ETextureAspectFlags::COLOR
                );
            transition.publish_external_state =
                _publish_external_state;

            CommandList producer(_queue);
            producer.SetResourceStateOwnership(
                ERHIResourceStateOwnership::Explicit
            );
            producer.Barriers({transition});
            CmdSubmit submit = producer.Submit();
            submit.SetResourceStateOwnership(
                ERHIResourceStateOwnership::Explicit
            );
            submit.Signal(accepted.Get(), _signal_value);
            RHIExecutor::Get().Submit(
                _queue,
                std::move(submit),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            auto* vk_accepted = ResourceCast(accepted.Get());
            if (vk_accepted->IsFailed() ||
                vk_accepted->IsRejected(_signal_value)) {
                throw std::runtime_error(
                    "Present source state publication was not natively accepted"
                );
            }
        };

    const BarrierState undefined_state = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_NONE,
        ERHIAccessFlags::UNDEFINED,
        ETextureLayout::TEXTURE_LAYOUT_UNDEFINED
    );
    const BarrierState presentation_state = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_READ,
        ETextureLayout::TEXTURE_LAYOUT_COMMON
    );
    const BarrierState transfer_write_state = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_WRITE,
        ETextureLayout::TEXTURE_LAYOUT_COMMON
    );
    const RHIQueueTopology topology = device.GetQueueTopology();

    // The materializer commits each accepted segment independently. Verify
    // that a publication accepted by the first segment is closed by a later
    // accepted mutation before the submit-level Present is resolved.
    constexpr uint64 segmented_present_scope =
        0x5048313850524553ull;
    FenceRef segmented_present_done = device.CreateFence();
    CommandList segmented_present_producer(
        EQueueType::Graphics
    );
    segmented_present_producer.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    BarrierCreateInfo segmented_publication =
        BarrierCreateInfo::Transition(
            backend_nonterminal_source->GetView(),
            undefined_state,
            presentation_state,
            ETextureAspectFlags::COLOR
        );
    segmented_publication.publish_external_state = true;
    segmented_present_producer.Barriers(
        {segmented_publication}
    );
    segmented_present_producer.Barriers(
        {BarrierCreateInfo::Transition(
            backend_nonterminal_source->GetView(),
            presentation_state,
            transfer_write_state,
            ETextureAspectFlags::COLOR
        )}
    );
    CmdSubmit segmented_present_submit =
        segmented_present_producer.Submit();
    segmented_present_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    segmented_present_submit.async_queue_scope =
        segmented_present_scope;
    segmented_present_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    segmented_present_submit.Signal(
        segmented_present_done.Get(), 1
    );
    PresentReceiptRef segmented_present_receipt =
        MakeShared<PresentReceipt>();
    RHIPresentRequest segmented_present(
        scripted_swapchain,
        backend_nonterminal_source->GetView(),
        segmented_present_receipt
    );
    const uint32 scripted_before_segmented_present =
        scripted_capture.count.load(std::memory_order_acquire);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(segmented_present_submit),
        ERHIExecSubmitFlags::FlushGPU,
        &segmented_present
    );
    const PresentReceiptResult segmented_present_result =
        segmented_present_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!ResourceCast(segmented_present_done.Get())
             ->WaitSubmitted(1) ||
        ResourceCast(backend_nonterminal_source.Get())
            ->IsPresentationSourceReady() ||
        !segmented_present_result.resolved ||
        segmented_present_result.submitted ||
        segmented_present_result.recreate_swapchain ||
        segmented_present_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(
            std::memory_order_acquire
        ) != scripted_before_segmented_present) {
        throw std::runtime_error(
            "an accepted later segment did not close an earlier "
            "presentation-source publication"
        );
    }

    // A later materialized segment can reject before native recording without
    // rolling back an earlier segment that Vulkan already accepted. The
    // accepted publication remains the externally visible final state.
    constexpr uint64 rejected_suffix_scope =
        0x5048313852535546ull;
    FenceRef rejected_suffix_done = device.CreateFence();
    CommandList rejected_suffix_producer(
        EQueueType::Graphics
    );
    rejected_suffix_producer.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    BarrierCreateInfo rejected_suffix_publication =
        BarrierCreateInfo::Transition(
            rejected_suffix_source->GetView(),
            undefined_state,
            presentation_state,
            ETextureAspectFlags::COLOR
        );
    rejected_suffix_publication.publish_external_state = true;
    rejected_suffix_producer.Barriers(
        {rejected_suffix_publication}
    );
    rejected_suffix_producer.AddCustomCommand(
        MakeUnique<OpaquePresentationStateProbeCommand>(),
        "RejectedPresentationStateSuffix"
    );
    CmdSubmit rejected_suffix_submit =
        rejected_suffix_producer.Submit();
    rejected_suffix_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    rejected_suffix_submit.async_queue_scope =
        rejected_suffix_scope;
    rejected_suffix_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    rejected_suffix_submit.Signal(
        rejected_suffix_done.Get(), 1
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(rejected_suffix_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(rejected_suffix_done.Get())
             ->IsRejected(1) ||
        ResourceCast(rejected_suffix_done.Get())->IsFailed() ||
        !ResourceCast(rejected_suffix_source.Get())
             ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "a rejected materialized suffix rolled back or hid its "
            "accepted presentation-source prefix"
        );
    }

    const uint32 scripted_before_rejected_suffix_present =
        scripted_capture.count.load(std::memory_order_acquire);
    PresentReceiptRef rejected_suffix_receipt =
        MakeShared<PresentReceipt>();
    RHIExecutor::Get().Present(
        RHIPresentRequest(
            scripted_swapchain,
            rejected_suffix_source->GetView(),
            rejected_suffix_receipt
        ),
        true
    );
    const PresentReceiptResult rejected_suffix_result =
        rejected_suffix_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!rejected_suffix_result.resolved ||
        rejected_suffix_result.submitted ||
        !rejected_suffix_result.recreate_swapchain ||
        rejected_suffix_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(std::memory_order_acquire) !=
            scripted_before_rejected_suffix_present + 1) {
        throw std::runtime_error(
            "an accepted presentation-source prefix was not visible after "
            "its recoverably rejected suffix"
        );
    }

    if (topology.copy.available) {
        // White-box seed isolates the Copy SubmitRecorded commit point from
        // queue-family transfer ownership. The accepted explicit barrier is
        // real queue work; if Copy omits its accepted delta commit, the seed
        // remains observable here. Cross-queue ownership is covered by the
        // dedicated explicit multi-queue fixtures.
        auto* vk_copy_commit_source =
            ResourceCast(copy_commit_source.Get());
        vk_copy_commit_source->PublishPresentationSourceReady(true);
        submit_source_state(
            copy_commit_source,
            EQueueType::Copy,
            undefined_state,
            transfer_write_state,
            false,
            1
        );
        if (vk_copy_commit_source->IsPresentationSourceReady()) {
            throw std::runtime_error(
                "an accepted Copy queue packet did not commit its "
                "presentation-source invalidation"
            );
        }
        expect_rejected(
            copy_commit_source->GetView(),
            "source invalidated by an accepted Copy queue packet",
            scripted_swapchain
        );
        copy_commit_verified = true;
    }

    submit_source_state(
        valid_source,
        EQueueType::Graphics,
        undefined_state,
        presentation_state,
        true,
        1
    );
    auto* vk_valid_source = ResourceCast(valid_source.Get());
    if (!vk_valid_source->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "accepted producer export did not publish Present readiness"
        );
    }

    submit_source_state(
        valid_source,
        EQueueType::Graphics,
        presentation_state,
        transfer_write_state,
        false,
        2
    );
    if (vk_valid_source->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "a later accepted non-export state left stale Present readiness"
        );
    }
    expect_rejected(
        valid_source->GetView(),
        "source moved away from its published presentation state",
        scripted_swapchain
    );

    submit_source_state(
        valid_source,
        EQueueType::Graphics,
        transfer_write_state,
        presentation_state,
        true,
        3
    );
    if (!vk_valid_source->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "accepted producer re-export did not restore Present readiness"
        );
    }

    if (topology.compute.available) {
        submit_source_state(
            wrong_queue_source,
            EQueueType::Compute,
            undefined_state,
            presentation_state,
            true,
            1
        );
        if (ResourceCast(wrong_queue_source.Get())
                ->IsPresentationSourceReady()) {
            throw std::runtime_error(
                "a non-Graphics export published Present readiness"
            );
        }
        expect_rejected(
            wrong_queue_source->GetView(),
            "source exported by a non-Graphics queue",
            scripted_swapchain
        );
    }

    FenceRef rejected_dependency = device.CreateFence();
    FenceRef rejected_export_done = device.CreateFence();
    rejected_dependency->Reject(1);
    BarrierCreateInfo rejected_export =
        BarrierCreateInfo::Transition(
            rejected_export_source->GetView(),
            undefined_state,
            presentation_state,
            ETextureAspectFlags::COLOR
        );
    rejected_export.publish_external_state = true;
    CommandList rejected_producer(EQueueType::Graphics);
    rejected_producer.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    rejected_producer.Barriers({rejected_export});
    CmdSubmit rejected_submit = rejected_producer.Submit();
    rejected_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    rejected_submit.Wait(rejected_dependency.Get(), 1);
    rejected_submit.Signal(rejected_export_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(rejected_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!ResourceCast(rejected_export_done.Get())->IsRejected(1) ||
        ResourceCast(rejected_export_done.Get())->IsFailed() ||
        ResourceCast(rejected_export_source.Get())
            ->IsPresentationSourceReady()) {
        throw std::runtime_error(
            "a rejected producer export published Present readiness"
        );
    }
    expect_rejected(
        rejected_export_source->GetView(),
        "source whose producer export was rejected",
        scripted_swapchain
    );

    PresentReceiptRef valid_receipt = MakeShared<PresentReceipt>();
    RHIExecutor::Get().Present(
        RHIPresentRequest(
            scripted_swapchain,
            valid_source->GetView(),
            valid_receipt
        ),
        true
    );
    const PresentReceiptResult valid_result =
        valid_receipt->WaitForSubmission(5s);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (!valid_result.resolved || valid_result.submitted ||
        !valid_result.recreate_swapchain ||
        valid_receipt->ResolutionAttemptCount() != 1 ||
        scripted_capture.count.load(
            std::memory_order_acquire
        ) != 4 ||
        scripted_capture.timeline.load(
            std::memory_order_acquire
        ) == 0 ||
        scripted_capture.wrong_owner.load(
            std::memory_order_acquire
        )) {
        throw std::runtime_error(
            "a valid Present source did not reach the scripted override"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=PresentSourceContractRejection "
        "rejected={} null=true usage=true transfer_src=true samples=true "
        "format=true compressed=true mip=true layer=true offset=true extent=true "
        "fresh=true accepted_export=true stale_clear=true "
        "backend_tracked_same_batch=true backend_rejected=true "
        "marker_then_clear=true accepted_mutation_clear=true "
        "rejected_mutation_preserves=true accepted_copy_mutation=true "
        "marker_then_segmented_state_change=true "
        "accepted_prefix_rejected_suffix=true copy_commit={} "
        "wrong_queue_clear=true rejected_export_clear=true "
        "bindless_source_order=true bindless_refcount=true "
        "bindless_rejected_update=true bindless_segments=true "
        "bindless_cross_queue={} bindless_parallel_record=true "
        "valid_override=4 owner=Submission",
        rejected_count,
        copy_commit_verified ? "verified" : "skipped",
        topology.compute.available ? "verified" : "skipped"
    );
}

void RunPresentPipelineBoundary() {
    using namespace std::chrono_literals;

    auto&                  device   = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    const EQueueType prefix_queue =
        topology.copy.available ?
            EQueueType::Copy :
            EQueueType::Graphics;
    const uint32 prefix_native =
        prefix_queue == EQueueType::Copy ?
            topology.copy.native_queue_id :
            topology.graphics.native_queue_id;
    const bool bridge_required =
        prefix_queue != EQueueType::Graphics;
    const uint32 bridge_dependency_wait_count =
        bridge_required &&
                prefix_native != topology.graphics.native_queue_id ?
            1u :
            0u;
    constexpr uint64 async_queue_scope =
        0x5048313546524545ull;
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    SourceSubmissionCapture source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    SubmissionBoundaryCapture boundary_capture{};
    ScopedSubmissionBoundaryObserver boundary_observer(
        boundary_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          wait_capture{prefix_queue};
    ScopedDependencyWaitObserver   wait_observer(wait_capture);
    ScriptedPresentCapture scripted_capture(
        VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Recreate,
                VK_ERROR_OUT_OF_DATE_KHR,
            },
        }
    );
    ScopedScriptedPresentOverride scripted_override(
        scripted_capture
    );

    TextureRef present_source = device.CreateTexture(
        "phase15f_present_source",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
    SwapchainRef scripted_swapchain{
        MoerNew(HeadlessScriptedSwapchain)()
    };
    BufferRef transfer_buffer = device.CreateBuffer<uint32>(
        "phase15f_present_bridge_buffer",
        4,
        EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );
    if (!present_source.IsValid() || !scripted_swapchain.IsValid() ||
        !transfer_buffer.IsValid()) {
        throw std::runtime_error(
            "Phase15F Present boundary resources were not created"
        );
    }

    constexpr std::array<uint32, 4> expected{
        0x15F00001u,
        0x15F00002u,
        0x15F00003u,
        0x15F00004u,
    };
    std::array<uint32, expected.size()> readback{};
    FenceRef dependency = device.CreateFence();
    FenceRef prefix_done = device.CreateFence();
    FenceRef later_done  = device.CreateFence();
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::binary_semaphore later_translated{0};
    bool dependency_released = false;

    const auto release_dependency = [&] {
        if (dependency_released) {
            return;
        }
        ResourceCast(dependency.Get())->SignalHost(1);
        dependency_released = true;
    };

    try {
        CommandList prefix(prefix_queue);
        prefix.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix.CopyFrom(
            OwnedBytes(expected),
            transfer_buffer->GetView(),
            "PresentBoundaryBlockedPrefix"
        );
        prefix.AddCallback([&] {
            ordinary_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        prefix.AddSuccessCallback([&] {
            success_callbacks[0].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit prefix_submit = prefix.Submit();
        prefix_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        prefix_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix_submit.async_queue_scope = async_queue_scope;
        prefix_submit.Wait(dependency.Get(), 1)
            .Signal(prefix_done.Get(), 1);
        RHIExecutor::Get().Submit(
            prefix_queue,
            std::move(prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        if (!wait_capture.entered.try_acquire_for(5s) ||
            wait_capture.count.load(std::memory_order_acquire) != 1 ||
            wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            source_capture.Count() != 0 ||
            boundary_capture.Count() != 0 ||
            scripted_capture.count.load(std::memory_order_acquire) != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "Present boundary prefix did not block on the sole "
                "Submission owner"
            );
        }

        PresentReceiptRef receipt = MakeShared<PresentReceipt>();
        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                present_source->GetView(),
                receipt
            ),
            true
        );

        CommandList later(EQueueType::Graphics);
        later.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        later.CopyFrom(
            transfer_buffer->GetView(),
            WritableBytes(readback),
            "PresentBoundaryLaterReadback"
        );
        later.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(
                &later_translated,
                EQueueType::Graphics
            ),
            "PresentBoundaryLaterParallelSource"
        );
        later.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        later.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit later_submit = later.Submit();
        later_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        later_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        later_submit.async_queue_scope = async_queue_scope;
        later_submit.Signal(later_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(later_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        if (later_translated.try_acquire_for(250ms) ||
            scripted_capture.count.load(std::memory_order_acquire) != 0 ||
            source_capture.Count() != 0 ||
            boundary_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "Present or a later source overtook the blocked "
                "pipeline prefix"
            );
        }

        release_dependency();
        const PresentReceiptResult receipt_result =
            receipt->WaitForSubmission(5s);
        if (!receipt_result.resolved ||
            receipt_result.submitted ||
            !receipt_result.recreate_swapchain ||
            receipt_result.status != EPresentStatus::OutOfDate ||
            receipt_result.stage != EPresentStage::Present) {
            throw std::runtime_error(
                "scripted Recreate Present resolved the wrong receipt state"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);

        if (!later_translated.try_acquire() ||
            receipt->ResolutionAttemptCount() != 1 ||
            scripted_capture.count.load(std::memory_order_acquire) != 1 ||
            scripted_capture.timeline.load(std::memory_order_acquire) == 0 ||
            scripted_capture.wrong_owner.load(std::memory_order_acquire) ||
            readback != expected) {
            throw std::runtime_error(
                "scripted Present did not preserve bridge ordering, "
                "readback, or exactly-once receipt ownership"
            );
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1 ||
                success_callbacks[index].load(
                    std::memory_order_acquire
                ) != 1) {
                throw std::runtime_error(
                    "Present boundary source callbacks were not retired "
                    "exactly once"
                );
            }
        }
        if (ResourceCast(prefix_done.Get())->HostWait(1) != VK_SUCCESS ||
            ResourceCast(later_done.Get())->HostWait(1) != VK_SUCCESS) {
            throw std::runtime_error(
                "Present boundary source signals did not succeed"
            );
        }
        if (source_capture.Overflowed() ||
            source_capture.Count() != 2) {
            throw std::runtime_error(
                "Present boundary lost a source submission"
            );
        }
        const VulkanSourceSubmissionEvent& prefix_source =
            source_capture.Event(0);
        const VulkanSourceSubmissionEvent& later_source =
            source_capture.Event(1);
        if (prefix_source.batch_sequence == 0 ||
            later_source.batch_sequence !=
                prefix_source.batch_sequence + 2 ||
            prefix_source.source_index != 0 ||
            later_source.source_index != 0 ||
            prefix_source.queue != prefix_queue ||
            later_source.queue != EQueueType::Graphics) {
            throw std::runtime_error(
                "Present did not remain between its prefix and later source"
            );
        }

        const size_t expected_native_count =
            bridge_required ? 3 : 2;
        if (native_capture.Overflowed() ||
            native_capture.Count() != expected_native_count) {
            throw std::runtime_error(
                "scripted Present emitted or lost a native submission"
            );
        }
        const std::array<EQueueType, 3> expected_native_queues{
            prefix_queue,
            EQueueType::Graphics,
            EQueueType::Graphics,
        };
        const uint32 submission_thread_id =
            native_capture.Event(0).thread_id;
        for (size_t index = 0; index < expected_native_count; ++index) {
            const size_t queue_index =
                !bridge_required && index == 1 ? 2 : index;
            const VulkanNativeSubmissionEvent& event =
                native_capture.Event(index);
            if (event.queue != expected_native_queues[queue_index] ||
                event.thread_role != ERHIThreadRole::Submission ||
                event.thread_id != submission_thread_id ||
                event.thread_id == main_thread_id ||
                !event.outcome.WasSubmitted()) {
                throw std::runtime_error(
                    "Present boundary native submission order or owner changed"
                );
            }
        }

        const size_t first_present_event_count =
            bridge_required ? 4 : 2;
        if (!boundary_capture.IsValid() ||
            boundary_capture.Count() !=
                first_present_event_count) {
            throw std::runtime_error(
                "Present boundary emitted the wrong boundary event count"
            );
        }
        const auto expect_pair =
            [&](size_t index,
                uint64 batch_sequence,
                uint32 operation_index,
                EVulkanSubmissionBoundaryKind kind,
                uint32 dependency_wait_count) {
                const VulkanSubmissionBoundaryEvent& dispatch =
                    boundary_capture.Event(index);
                const VulkanSubmissionBoundaryEvent& terminal =
                    boundary_capture.Event(index + 1);
                if (dispatch.batch_sequence != batch_sequence ||
                    terminal.batch_sequence != batch_sequence ||
                    dispatch.operation_index != operation_index ||
                    terminal.operation_index != operation_index ||
                    dispatch.kind != kind || terminal.kind != kind ||
                    dispatch.queue != EQueueType::Graphics ||
                    terminal.queue != EQueueType::Graphics ||
                    dispatch.phase !=
                        EVulkanSubmissionBoundaryPhase::Dispatch ||
                    terminal.phase !=
                        EVulkanSubmissionBoundaryPhase::Terminal ||
                    dispatch.outcome_valid ||
                    !terminal.outcome_valid ||
                    dispatch.dependency_wait_count !=
                        dependency_wait_count ||
                    terminal.dependency_wait_count !=
                        dependency_wait_count ||
                    dispatch.thread_id != submission_thread_id ||
                    terminal.thread_id != submission_thread_id) {
                    throw std::runtime_error(
                        "submission-boundary identity or owner changed"
                    );
                }
            };

        size_t boundary_cursor = 0;
        const uint64 present_batch_sequence =
            prefix_source.batch_sequence + 1;
        if (bridge_required) {
            expect_pair(
                boundary_cursor,
                present_batch_sequence,
                0,
                EVulkanSubmissionBoundaryKind::PresentBridge,
                bridge_dependency_wait_count
            );
            const VulkanSubmissionBoundaryEvent& bridge_terminal =
                boundary_capture.Event(boundary_cursor + 1);
            if (!bridge_terminal.gpu_submitted ||
                !bridge_terminal.outcome.Succeeded()) {
                throw std::runtime_error(
                    "Present bridge did not submit its cross-queue wait"
                );
            }
            boundary_cursor += 2;
        }
        expect_pair(
            boundary_cursor,
            present_batch_sequence,
            1,
            EVulkanSubmissionBoundaryKind::Present,
            0
        );
        const VulkanSubmissionBoundaryEvent& present_terminal =
            boundary_capture.Event(boundary_cursor + 1);
        if (present_terminal.outcome.status !=
                EVulkanOperationStatus::Recreate ||
            present_terminal.gpu_submitted ||
            present_terminal.recoverable_rejection ||
            present_terminal.present_receipt_resolution_attempts != 1) {
            throw std::runtime_error(
                "Present terminal diagnostics lost Recreate state"
            );
        }

        PresentReceiptRef present_only_receipt =
            MakeShared<PresentReceipt>();
        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                present_source->GetView(),
                present_only_receipt
            ),
            true
        );
        const PresentReceiptResult present_only_result =
            present_only_receipt->WaitForSubmission(5s);
        if (!present_only_result.resolved ||
            present_only_result.submitted ||
            !present_only_result.recreate_swapchain ||
            present_only_result.status != EPresentStatus::OutOfDate ||
            present_only_result.stage != EPresentStage::Present) {
            throw std::runtime_error(
                "Present-only batch resolved the wrong receipt state"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        if (present_only_receipt->ResolutionAttemptCount() != 1 ||
            scripted_capture.count.load(std::memory_order_acquire) != 2 ||
            boundary_capture.Count() !=
                first_present_event_count + 2 ||
            native_capture.Count() != expected_native_count) {
            throw std::runtime_error(
                "Present-only batch emitted an unexpected bridge, "
                "native submit, or receipt replay"
            );
        }
        expect_pair(
            first_present_event_count,
            present_batch_sequence + 2,
            1,
            EVulkanSubmissionBoundaryKind::Present,
            0
        );

        const size_t boundaries_before_second_sync =
            boundary_capture.Count();
        const size_t native_before_second_sync =
            native_capture.Count();
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        if (boundary_capture.Count() !=
                boundaries_before_second_sync ||
            native_capture.Count() != native_before_second_sync ||
            scripted_capture.count.load(std::memory_order_acquire) != 2) {
            throw std::runtime_error(
                "a second Present Sync replayed boundary work"
            );
        }

        const size_t surface_lost_boundary_index =
            boundary_capture.Count();
        scripted_capture.result = VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Recreate,
                VK_ERROR_SURFACE_LOST_KHR,
            },
        };
        PresentReceiptRef surface_lost_receipt =
            MakeShared<PresentReceipt>();
        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                present_source->GetView(),
                surface_lost_receipt
            ),
            true
        );
        const PresentReceiptResult surface_lost_result =
            surface_lost_receipt->WaitForSubmission(5s);
        if (!surface_lost_result.resolved ||
            surface_lost_result.submitted ||
            !surface_lost_result.recreate_swapchain ||
            surface_lost_result.status !=
                EPresentStatus::SurfaceLost ||
            surface_lost_result.stage != EPresentStage::Present) {
            throw std::runtime_error(
                "scripted SurfaceLost Present lost its typed receipt"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        if (surface_lost_receipt->ResolutionAttemptCount() != 1 ||
            scripted_capture.count.load(std::memory_order_acquire) != 3 ||
            boundary_capture.Count() !=
                surface_lost_boundary_index + 2 ||
            native_capture.Count() != expected_native_count) {
            throw std::runtime_error(
                "scripted SurfaceLost Present was replayed or submitted "
                "native work"
            );
        }
        expect_pair(
            surface_lost_boundary_index,
            present_batch_sequence + 3,
            1,
            EVulkanSubmissionBoundaryKind::Present,
            0
        );
        const VulkanSubmissionBoundaryEvent&
            surface_lost_terminal =
                boundary_capture.Event(surface_lost_boundary_index + 1);
        if (surface_lost_terminal.outcome.status !=
                EVulkanOperationStatus::Recreate ||
            surface_lost_terminal.outcome.result !=
                VK_ERROR_SURFACE_LOST_KHR) {
            throw std::runtime_error(
                "SurfaceLost terminal diagnostics lost the native result"
            );
        }

        // A faulted device would reject before reaching the scripted
        // callback. A typed Retry on the following request therefore proves
        // that SurfaceLost did not poison VulkanDevice fault state.
        scripted_capture.result = VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Retry,
                VK_NOT_READY,
            },
        };
        PresentReceiptRef post_surface_lost_receipt =
            MakeShared<PresentReceipt>();
        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                present_source->GetView(),
                post_surface_lost_receipt
            ),
            true
        );
        const PresentReceiptResult post_surface_lost_result =
            post_surface_lost_receipt->WaitForSubmission(5s);
        if (!post_surface_lost_result.resolved ||
            post_surface_lost_result.submitted ||
            post_surface_lost_result.recreate_swapchain ||
            post_surface_lost_result.status !=
                EPresentStatus::Retry ||
            post_surface_lost_result.stage != EPresentStage::Present) {
            throw std::runtime_error(
                "SurfaceLost polluted VulkanDevice fault state"
            );
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        if (post_surface_lost_receipt->ResolutionAttemptCount() != 1 ||
            scripted_capture.count.load(std::memory_order_acquire) != 4 ||
            boundary_capture.Count() !=
                surface_lost_boundary_index + 4 ||
            native_capture.Count() != expected_native_count ||
            scripted_capture.wrong_owner.load(
                std::memory_order_acquire
            )) {
            throw std::runtime_error(
                "post-SurfaceLost Retry did not remain on the Submission "
                "owner exactly once"
            );
        }
        expect_pair(
            surface_lost_boundary_index + 2,
            present_batch_sequence + 4,
            1,
            EVulkanSubmissionBoundaryKind::Present,
            0
        );

        LOG_INFO(
            "[TESTCASE][PASS] name=PresentPipelineBoundary "
            "outcome=Recreate order=Prefix,Bridge?,Present,Later "
            "owner=Submission receipt_attempts=1 submitted=false "
            "recreate=true completion=drained later_batch=success "
            "present_only=verified surface_lost=typed "
            "device_healthy=verified bridge={} graphics_native={} "
            "prefix_native={} readback=verified replay=0",
            bridge_required ? "required" : "elided",
            topology.graphics.native_queue_id,
            prefix_native
        );
    } catch (...) {
        release_dependency();
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        throw;
    }
}

void RunQueuedPresentShutdownBoundary() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();
    constexpr uint64 async_queue_scope =
        0x5048313546534844ull;
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();

    SourceSubmissionCapture source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    SubmissionBoundaryCapture boundary_capture{};
    ScopedSubmissionBoundaryObserver boundary_observer(
        boundary_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    DependencyWaitCapture          wait_capture{EQueueType::Graphics};
    ScopedDependencyWaitObserver   wait_observer(wait_capture);
    ScriptedPresentCapture scripted_capture(
        VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Retry,
                VK_NOT_READY,
            },
        }
    );
    ScopedScriptedPresentOverride scripted_override(
        scripted_capture
    );

    TextureRef present_source = device.CreateTexture(
        "phase15f_shutdown_present_source",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
    SwapchainRef scripted_swapchain{
        MoerNew(HeadlessScriptedSwapchain)()
    };
    if (!present_source.IsValid() || !scripted_swapchain.IsValid()) {
        throw std::runtime_error(
            "Phase15F shutdown Present resources were not created"
        );
    }

    FenceRef dependency = device.CreateFence();
    FenceRef prefix_done = device.CreateFence();
    PresentReceiptRef receipt = MakeShared<PresentReceipt>();
    std::atomic<uint32> ordinary_callbacks{0};
    std::atomic<uint32> success_callbacks{0};
    bool runtime_stopped = false;

    const auto stop_runtime = [&] {
        if (runtime_stopped) {
            return;
        }
        RHIExecutor::ShutDown();
        runtime_stopped = true;
    };

    try {
        CommandList prefix(EQueueType::Graphics);
        prefix.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix.AddCallback([&] {
            ordinary_callbacks.fetch_add(
                1, std::memory_order_relaxed
            );
        });
        prefix.AddSuccessCallback([&] {
            success_callbacks.fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit prefix_submit = prefix.Submit();
        prefix_submit.SetTranslateExecutionClass(
            ERHITranslateExecutionClass::Parallel
        );
        prefix_submit.SetResourceStateOwnership(
            ERHIResourceStateOwnership::Explicit
        );
        prefix_submit.async_queue_scope = async_queue_scope;
        prefix_submit.Wait(dependency.Get(), 1)
            .Signal(prefix_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(prefix_submit),
            ERHIExecSubmitFlags::FlushGPU
        );

        if (!wait_capture.entered.try_acquire_for(5s) ||
            wait_capture.count.load(std::memory_order_acquire) != 1 ||
            wait_capture.wrong_owner.load(std::memory_order_acquire) ||
            source_capture.Count() != 0 ||
            boundary_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "shutdown Present prefix did not block on the sole "
                "Submission owner"
            );
        }

        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                present_source->GetView(),
                receipt
            ),
            true
        );
        if (receipt->WaitForSubmission(250ms).resolved ||
            scripted_capture.count.load(std::memory_order_acquire) != 0 ||
            boundary_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "queued Present overtook the shutdown-blocked prefix"
            );
        }

        stop_runtime();

        const PresentReceiptResult result =
            receipt->WaitForSubmission(5s);
        if (!result.resolved ||
            result.submitted ||
            result.recreate_swapchain ||
            receipt->ResolutionAttemptCount() != 1) {
            throw std::runtime_error(
                "shutdown Retry Present resolved the wrong receipt state"
            );
        }
        if (ResourceCast(prefix_done.Get())->HostWait(1) !=
                VK_ERROR_UNKNOWN ||
            !ResourceCast(prefix_done.Get())->IsRejected(1) ||
            ResourceCast(prefix_done.Get())->IsFailed() ||
            ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
            success_callbacks.load(std::memory_order_acquire) != 0 ||
            source_capture.Count() != 0 ||
            native_capture.Count() != 0) {
            throw std::runtime_error(
                "shutdown did not reject the blocked prefix and retire "
                "the queued Present without native work"
            );
        }
        if (scripted_capture.count.load(
                std::memory_order_acquire
            ) != 1 ||
            scripted_capture.wrong_owner.load(
                std::memory_order_acquire
            ) ||
            scripted_capture.thread_id.load(
                std::memory_order_acquire
            ) == main_thread_id) {
            throw std::runtime_error(
                "shutdown Retry Present escaped the Submission owner"
            );
        }

        if (!boundary_capture.IsValid() ||
            boundary_capture.Count() != 2) {
            throw std::runtime_error(
                "shutdown Retry Present emitted the wrong boundary events"
            );
        }
        const VulkanSubmissionBoundaryEvent& dispatch =
            boundary_capture.Event(0);
        const VulkanSubmissionBoundaryEvent& terminal =
            boundary_capture.Event(1);
        if (dispatch.batch_sequence == 0 ||
            terminal.batch_sequence != dispatch.batch_sequence ||
            dispatch.operation_index != 1 ||
            terminal.operation_index != 1 ||
            dispatch.kind !=
                EVulkanSubmissionBoundaryKind::Present ||
            terminal.kind !=
                EVulkanSubmissionBoundaryKind::Present ||
            dispatch.phase !=
                EVulkanSubmissionBoundaryPhase::Dispatch ||
            terminal.phase !=
                EVulkanSubmissionBoundaryPhase::Terminal ||
            dispatch.outcome_valid ||
            !terminal.outcome_valid ||
            terminal.outcome.status !=
                EVulkanOperationStatus::Retry ||
            terminal.gpu_submitted ||
            terminal.recoverable_rejection ||
            terminal.present_receipt_resolution_attempts != 1 ||
            dispatch.thread_id != terminal.thread_id ||
            dispatch.thread_id != scripted_capture.thread_id.load(
                std::memory_order_acquire
            )) {
            throw std::runtime_error(
                "shutdown Retry Present lost identity, outcome, or owner"
            );
        }

        LOG_INFO(
            "[TESTCASE][PASS] name=QueuedPresentShutdownBoundary "
            "outcome=Retry prefix=rejected receipt_attempts=1 "
            "submitted=false recreate=false owner=Submission "
            "native_submit=0 owners=stopped replay=0"
        );
    } catch (...) {
        const std::exception_ptr failure =
            std::current_exception();
        stop_runtime();
        std::rethrow_exception(failure);
    }
}

void RunPresentHardFailureBoundary() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();
    const uint32 main_thread_id =
        Platform::GetCurrentThreadID();
    TextureRef invalid_present_source = device.CreateTexture(
        "phase15f_fault_priority_invalid_source",
        Extent3D(4, 4, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
    SwapchainRef scripted_swapchain{
        MoerNew(HeadlessScriptedSwapchain)()
    };
    if (!invalid_present_source.IsValid() ||
        !scripted_swapchain.IsValid()) {
        throw std::runtime_error(
            "Phase15F hard Present resources were not created"
        );
    }

    SubmissionBoundaryCapture boundary_capture{};
    ScopedSubmissionBoundaryObserver boundary_observer(
        boundary_capture
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    ScriptedPresentCapture scripted_capture(
        VulkanScriptedPresentResult{
            .outcome = {
                EVulkanOperationStatus::Rejected,
                VK_ERROR_DEVICE_LOST,
            },
        }
    );
    ScopedScriptedPresentOverride scripted_override(
        scripted_capture,
        false,
        true
    );

    PresentReceiptRef receipt = MakeShared<PresentReceipt>();
    FenceRef later_done = device.CreateFence();
    std::atomic<uint32> ordinary_callbacks{0};
    std::atomic<uint32> success_callbacks{0};

    try {
        RHIExecutor::Get().Present(
            RHIPresentRequest(
                scripted_swapchain,
                invalid_present_source->GetView(),
                receipt
            ),
            true
        );
        const PresentReceiptResult receipt_result =
            receipt->WaitForSubmission(5s);
        if (!receipt_result.resolved || receipt_result.submitted ||
            receipt_result.recreate_swapchain ||
            receipt->ResolutionAttemptCount() != 1) {
            throw std::runtime_error(
                "hard Present resolved the wrong receipt state"
            );
        }

        CommandList later(EQueueType::Graphics);
        later.AddCallback([&] {
            ordinary_callbacks.fetch_add(
                1, std::memory_order_relaxed
            );
        });
        later.AddSuccessCallback([&] {
            success_callbacks.fetch_add(
                1, std::memory_order_relaxed
            );
        });
        CmdSubmit later_submit = later.Submit();
        later_submit.Signal(later_done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(later_submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);

        if (scripted_capture.count.load(
                std::memory_order_acquire
            ) != 0 ||
            scripted_capture.wrong_owner.load(
                std::memory_order_acquire
            ) ||
            scripted_capture.source_rejection_hook_count.load(
                std::memory_order_acquire
            ) != 1 ||
            scripted_capture.source_rejection_hook_failed.load(
                std::memory_order_acquire
            ) ||
            boundary_capture.Count() != 2 ||
            !boundary_capture.IsValid() ||
            native_capture.Count() != 0 ||
            ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
            success_callbacks.load(std::memory_order_acquire) != 0 ||
            !ResourceCast(later_done.Get())->IsFailed()) {
            throw std::runtime_error(
                "hard Present did not latch, reject, and retire exactly once"
            );
        }
        const VulkanSubmissionBoundaryEvent& dispatch =
            boundary_capture.Event(0);
        const VulkanSubmissionBoundaryEvent& terminal =
            boundary_capture.Event(1);
        if (dispatch.kind !=
                EVulkanSubmissionBoundaryKind::Present ||
            terminal.kind !=
                EVulkanSubmissionBoundaryKind::Present ||
            dispatch.phase !=
                EVulkanSubmissionBoundaryPhase::Dispatch ||
            terminal.phase !=
                EVulkanSubmissionBoundaryPhase::Terminal ||
            dispatch.outcome_valid ||
            !terminal.outcome_valid ||
            dispatch.operation_index != 1 ||
            terminal.operation_index != 1 ||
            dispatch.thread_role != ERHIThreadRole::Submission ||
            terminal.thread_role != ERHIThreadRole::Submission ||
            dispatch.thread_id != terminal.thread_id ||
            dispatch.thread_id == main_thread_id ||
            terminal.outcome.status !=
                EVulkanOperationStatus::Rejected ||
            terminal.outcome.result != VK_ERROR_DEVICE_LOST ||
            terminal.gpu_submitted ||
            terminal.recoverable_rejection ||
            terminal.present_receipt_resolution_attempts != 1) {
            throw std::runtime_error(
                "hard Present boundary diagnostics were incomplete"
            );
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        if (scripted_capture.count.load(
                std::memory_order_acquire
            ) != 0 ||
            boundary_capture.Count() != 2 ||
            native_capture.Count() != 0 ||
            ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
            success_callbacks.load(std::memory_order_acquire) != 0) {
            throw std::runtime_error(
                "a second Sync replayed hard Present work"
            );
        }

        LOG_INFO(
            "[TESTCASE][PASS] name=PresentHardFailureBoundary "
            "outcome=Rejected owner=Submission receipt_attempts=1 "
            "submitted=false recreate=false later_batch=rejected "
            "native_after_present=0 device_fault_priority=true "
            "invalid_source=true concurrent_hard_latch=verified replay=0"
        );
    } catch (...) {
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);
        throw;
    }
}
void PrimeGlobalTransientPoolForDispose() {
    auto& pool = RenderGraphResourcePool::Global();
    pool.Reset();
    const RGTransientBufferDesc desc{
        .element_count = kElementCount,
        .stride        = sizeof(uint32),
        .usage         = EBufferUsageFlags::TRANSFER_SRC |
                         EBufferUsageFlags::TRANSFER_DST,
    };
    {
        BufferRef resource =
            pool.AcquireBuffer("dispose_transient_pool_probe", desc);
        if (!resource.IsValid()) {
            throw std::runtime_error(
                "global transient pool dispose probe allocation failed"
            );
        }
    }
    if (pool.BufferCount() != 1 || pool.AvailableBufferCount() != 1) {
        throw std::runtime_error(
            "global transient pool dispose probe did not become idle"
        );
    }
}

void RunGpuScopeStreamCompletionAndParallelIsolation() {
    auto& device = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] "
            "name=GpuScopeStreamCompletionAndParallelIsolation "
            "reason=graphics_queue_unavailable"
        );
        return;
    }

    constexpr uint64 frame_id = 0x5048323142324652ull;
    constexpr uint64 async_queue_scope = 0x5048323142325343ull;
    constexpr size_t sibling_copy_count = 8;
    constexpr uint32 inner_copy_count = 128;
    constexpr uint32 outer_copy_count = 16;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;

    std::array<uint32, kElementCount> values{};
    std::array<uint32, kElementCount> scope_readback{};
    std::array<
        std::array<uint32, kElementCount>,
        sibling_copy_count
    > sibling_readbacks{};
    for (uint32 index = 0; index < values.size(); ++index) {
        values[index] = 0x21B20000u + index * 37u;
    }

    BufferRef scope_ping = device.CreateBuffer<uint32>(
        "gpu_scope_stream_ping", kElementCount, usage
    );
    BufferRef scope_pong = device.CreateBuffer<uint32>(
        "gpu_scope_stream_pong", kElementCount, usage
    );
    std::array<BufferRef, sibling_copy_count> sibling_sources{};
    std::array<BufferRef, sibling_copy_count>
        sibling_destinations{};
    for (size_t index = 0; index < sibling_copy_count; ++index) {
        sibling_sources[index] = device.CreateBuffer<uint32>(
            "gpu_scope_parallel_source_" +
                std::to_string(index),
            kElementCount,
            usage
        );
        sibling_destinations[index] =
            device.CreateBuffer<uint32>(
                "gpu_scope_parallel_destination_" +
                    std::to_string(index),
                kElementCount,
                usage
            );
    }
    if (!scope_ping || !scope_pong ||
        std::any_of(
            sibling_sources.begin(),
            sibling_sources.end(),
            [](const BufferRef& _buffer) {
                return !_buffer;
            }
        ) ||
        std::any_of(
            sibling_destinations.begin(),
            sibling_destinations.end(),
            [](const BufferRef& _buffer) {
                return !_buffer;
            }
        )) {
        throw std::runtime_error(
            "failed to allocate GPU scope focused-test buffers"
        );
    }

    // Initialize every source outside the observed batch. The query-free
    // sibling can then consist solely of independent GPU copies, which is the
    // smallest real workload that proves it remains parallel-record eligible.
    CommandList initialize(EQueueType::Graphics);
    initialize.CopyFrom(
        OwnedBytes(values),
        scope_ping->GetView(),
        "GpuScopeInitializePing"
    );
    for (size_t index = 0; index < sibling_copy_count; ++index) {
        initialize.CopyFrom(
            OwnedBytes(values),
            sibling_sources[index]->GetView(),
            "GpuScopeInitializeParallelSource"
        );
    }
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        initialize.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    GpuScopeStream stream(GpuScopeStreamConfig{
        .max_resident_frames = 2,
        .max_pending_frames = 2,
        .max_resident_scopes = 16,
        .max_scopes_per_frame = 8,
        .max_sources_per_frame = 4,
        .max_scope_name_bytes = 128,
        .max_error_reason_bytes = 256,
    });
    GpuScopeFrameHandle frame = stream.BeginFrame(frame_id);
    if (!frame.Valid()) {
        throw std::runtime_error(
            "GPU scope focused frame was not admitted"
        );
    }

    std::atomic<uint32> callback_errors{0};
    std::atomic<uint32> completion_callbacks{0};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::array<QueryFuture, 2> scope_futures{};

    CommandList scoped(EQueueType::Graphics);
    scoped.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    scoped.SetGpuScopeRecorder(
        frame.CreateRecorder(topology.graphics, 0)
    );
    GpuCompletionFuture gpu_completion =
        scoped.TrackGpuCompletion(
            "Phase21B2GpuScopeCompletion"
        );
    gpu_completion.Then(
        [&](const GpuCompletionResult& _result) {
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                _result.status !=
                    GpuCompletionStatus::Ready) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );

    auto record_ping_pong =
        [&](CommandList& _commands, uint32 _count) {
            for (uint32 copy = 0; copy < _count; ++copy) {
                if ((copy & 1u) == 0) {
                    _commands.CopyFrom(
                        scope_ping->GetView(),
                        scope_pong->GetView(),
                        "GpuScopePingToPong"
                    );
                } else {
                    _commands.CopyFrom(
                        scope_pong->GetView(),
                        scope_ping->GetView(),
                        "GpuScopePongToPing"
                    );
                }
            }
        };

    scoped.PushScopeWithTimeScope("Phase21B2Outer");
    record_ping_pong(scoped, outer_copy_count);
    scoped.PushScopeWithTimeScope("Phase21B2Inner");
    record_ping_pong(scoped, inner_copy_count);
    scoped.PopScopeWithTimeScope();
    record_ping_pong(scoped, outer_copy_count);
    scoped.PopScopeWithTimeScope();
    scoped.CopyFrom(
        scope_ping->GetView(),
        WritableBytes(scope_readback),
        "GpuScopeFocusedReadback"
    );
    scoped.AddCallback([&] {
        const GpuScopeStreamStats stats =
            stream.GetStats();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            completion_callbacks.load(
                std::memory_order_acquire
            ) != 1 ||
            query_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            query_callbacks[1].load(
                std::memory_order_acquire
            ) != 1 ||
            stats.frames_ready != 1 ||
            stats.scopes_ready != 2) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    scoped.AddSuccessCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            ordinary_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        success_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit scoped_submit = scoped.Submit();
    if (scoped_submit.query_tokens.size() != 2) {
        throw std::runtime_error(
            "nested GPU scopes did not emit two timestamp queries"
        );
    }
    const size_t query_command_count = std::count_if(
        scoped_submit.cmds.begin(),
        scoped_submit.cmds.end(),
        [](const UniquePtr<Command>& _command) {
            return _command &&
                   _command->Type() == Command::EType::Query;
        }
    );
    if (query_command_count != 4) {
        throw std::runtime_error(
            "nested GPU scopes did not emit two complete query pairs"
        );
    }

    scope_futures[0] =
        scoped_submit.query_tokens[0].GetFuture();
    scope_futures[1] =
        scoped_submit.query_tokens[1].GetFuture();
    for (size_t index = 0; index < scope_futures.size();
         ++index) {
        scope_futures[index].Then(
            [&, index](const QueryResult& _result) {
                if (GetCurrentRHIThreadRole() !=
                        ERHIThreadRole::Completion ||
                    _result.status != QueryStatus::Ready ||
                    _result.kind != QueryKind::Timestamp ||
                    std::get_if<TimestampQueryResult>(
                        &_result.payload
                    ) == nullptr) {
                    callback_errors.fetch_add(
                        1, std::memory_order_relaxed
                    );
                }
                query_callbacks[index].fetch_add(
                    1, std::memory_order_release
                );
            }
        );
    }
    FenceRef scoped_done = device.CreateFence();
    scoped_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    scoped_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    scoped_submit.async_queue_scope = async_queue_scope;
    scoped_submit.Signal(scoped_done.Get(), 1);

    CommandList sibling(EQueueType::Graphics);
    sibling.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    for (size_t index = 0; index < sibling_copy_count; ++index) {
        sibling.CopyFrom(
            sibling_sources[index]->GetView(),
            sibling_destinations[index]->GetView(),
            "GpuScopeQueryFreeParallelCopy"
        );
    }
    sibling.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
            ERHIThreadRole::Completion) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    sibling.AddSuccessCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            ordinary_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        success_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    CmdSubmit sibling_submit = sibling.Submit();
    FenceRef sibling_done = device.CreateFence();
    sibling_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    sibling_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    sibling_submit.async_queue_scope = async_queue_scope;
    sibling_submit.Signal(sibling_done.Get(), 1);

    if (!stream.SealFrame(frame)) {
        throw std::runtime_error(
            "GPU scope focused frame could not be sealed"
        );
    }
    ResolvedGpuScopeFrame premature{};
    if (stream.TryPopFrame(premature)) {
        throw std::runtime_error(
            "GPU scope frame became visible before GPU Completion"
        );
    }

    SourceTranslationCapture translation_capture(
        async_queue_scope, EQueueType::Graphics
    );
    ScopedSourceTranslationObserver translation_observer(
        translation_capture
    );

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(scoped_submit)
    );
    batch.emplace_back(
        EQueueType::Graphics, std::move(sibling_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    // Verify the query-free source's actual GPU writes with a separate
    // SerialControl submission. Keeping readback out of the observed source
    // preserves the topology under test while preventing planner diagnostics
    // alone from masking dropped parallel-recorded copies.
    CommandList verify_sibling_copies(EQueueType::Graphics);
    verify_sibling_copies.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    for (size_t index = 0; index < sibling_copy_count; ++index) {
        verify_sibling_copies.CopyFrom(
            sibling_destinations[index]->GetView(),
            WritableBytes(sibling_readbacks[index]),
            "GpuScopeParallelSiblingReadback"
        );
    }
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        verify_sibling_copies.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const GpuCompletionResult completion_result =
        gpu_completion.Get();
    std::array<QueryResult, 2> query_results{
        scope_futures[0].Get(),
        scope_futures[1].Get(),
    };
    std::array<const TimestampQueryResult*, 2> timestamps{
        std::get_if<TimestampQueryResult>(
            &query_results[0].payload
        ),
        std::get_if<TimestampQueryResult>(
            &query_results[1].payload
        ),
    };
    if (completion_result.status !=
            GpuCompletionStatus::Ready ||
        timestamps[0] == nullptr ||
        timestamps[1] == nullptr ||
        query_results[0].status != QueryStatus::Ready ||
        query_results[1].status != QueryStatus::Ready ||
        timestamps[0]->valid_bits == 0 ||
        timestamps[0]->valid_bits > 64 ||
        timestamps[1]->valid_bits == 0 ||
        timestamps[1]->valid_bits > 64 ||
        timestamps[0]->tick_period_ns <= 0.0 ||
        timestamps[1]->tick_period_ns <= 0.0 ||
        timestamps[0]->duration_ns <= 0.0 ||
        timestamps[1]->duration_ns <= 0.0) {
        throw std::runtime_error(
            "real nested GPU scope queries did not resolve valid raw timestamps"
        );
    }

    auto signal_succeeded = [](const FenceRef& _fence) {
        auto* vk_fence = ResourceCast(_fence.Get());
        return vk_fence != nullptr &&
               vk_fence->WaitSubmitted(1) &&
               !_fence->IsRejected(1) &&
               !vk_fence->IsFailed();
    };
    if (!signal_succeeded(scoped_done) ||
        !signal_succeeded(sibling_done) ||
        scope_readback != values ||
        !std::all_of(
            sibling_readbacks.begin(),
            sibling_readbacks.end(),
            [&](const auto& _readback) {
                return _readback == values;
            }
        )) {
        throw std::runtime_error(
            "GPU scope focused batch signal, scope readback, or parallel sibling readback failed"
        );
    }

    ResolvedGpuScopeFrame resolved{};
    if (!stream.TryPopFrame(resolved) || !resolved.valid ||
        resolved.frame_id != frame_id ||
        resolved.admitted_scope_count != 2 ||
        resolved.dropped_scope_count != 0 ||
        resolved.error_scope_count != 0 ||
        resolved.queue_roots[0].size() != 1 ||
        !resolved.queue_roots[1].empty() ||
        !resolved.queue_roots[2].empty()) {
        throw std::runtime_error(
            "real GPU scope frame lost readiness, accounting, or queue topology"
        );
    }

    const GpuScopeNode& outer =
        resolved.queue_roots[0].front();
    if (outer.name != "Phase21B2Outer" ||
        outer.depth != 0 ||
        outer.parent_scope_id != 0 ||
        outer.children.size() != 1) {
        throw std::runtime_error(
            "real GPU scope outer hierarchy is invalid"
        );
    }
    const GpuScopeNode& inner = outer.children.front();
    if (inner.name != "Phase21B2Inner" ||
        inner.depth != 1 ||
        inner.parent_scope_id != outer.scope_id ||
        !inner.children.empty() ||
        outer.queue_binding != topology.graphics ||
        inner.queue_binding != topology.graphics ||
        outer.source_order != 0 ||
        inner.source_order != 0 ||
        outer.local_order >= inner.local_order ||
        outer.status != GpuScopeTerminalStatus::Ready ||
        inner.status != GpuScopeTerminalStatus::Ready) {
        throw std::runtime_error(
            "real GPU scope child hierarchy or timestamp domain is invalid"
        );
    }

    auto approximately_equal =
        [](double _actual, double _expected) {
        const double scale =
            std::max(1.0, std::abs(_expected));
        return std::abs(_actual - _expected) <=
               scale * 1.0e-9;
        };
    auto matches_timestamp =
        [&](const GpuScopeNode& _node,
            const QueryResult& _query,
            const TimestampQueryResult& _timestamp) {
            return _node.query_id == _query.query_id &&
                   _node.begin_tick == _timestamp.begin_tick &&
                   _node.end_tick == _timestamp.end_tick &&
                   _node.valid_bits == _timestamp.valid_bits &&
                   approximately_equal(
                       _node.tick_period_ns,
                       _timestamp.tick_period_ns
                   ) &&
                   approximately_equal(
                       _node.total_duration_ns,
                       _timestamp.duration_ns
                   );
        };
    if (!matches_timestamp(
            outer, query_results[0], *timestamps[0]
        ) ||
        !matches_timestamp(
            inner, query_results[1], *timestamps[1]
        ) ||
        !approximately_equal(
            inner.exclusive_duration_ns,
            inner.total_duration_ns
        ) ||
        !approximately_equal(
            outer.exclusive_duration_ns,
            std::max(
                0.0,
                outer.total_duration_ns -
                    inner.total_duration_ns
            )
        )) {
        throw std::runtime_error(
            "GPU scope raw ticks, duration, or exclusive time changed during materialization"
        );
    }

    if (!translation_capture.IsValid() ||
        !translation_capture.Seen(
            0, EVulkanSourceTranslationPhase::Begin
        ) ||
        !translation_capture.Seen(
            0, EVulkanSourceTranslationPhase::Recorded
        ) ||
        !translation_capture.Seen(
            1, EVulkanSourceTranslationPhase::Begin
        ) ||
        !translation_capture.Seen(
            1, EVulkanSourceTranslationPhase::Recorded
        ) ||
        translation_capture.Seen(
            0, EVulkanSourceTranslationPhase::Failed
        ) ||
        translation_capture.Seen(
            1, EVulkanSourceTranslationPhase::Failed
        )) {
        throw std::runtime_error(
            "GPU scope focused batch lost source translation diagnostics"
        );
    }
    const VulkanSourceTranslationEvent& query_source =
        translation_capture.Event(
            0, EVulkanSourceTranslationPhase::Recorded
        );
    const VulkanSourceTranslationEvent& parallel_sibling =
        translation_capture.Event(
            1, EVulkanSourceTranslationPhase::Recorded
        );
    if (query_source.batch_sequence == 0 ||
        query_source.batch_sequence !=
            parallel_sibling.batch_sequence ||
        query_source.source_index != 0 ||
        query_source.original_source_index != 0 ||
        query_source.source_segment_index != 0 ||
        query_source.source_segment_count != 1 ||
        query_source.queue != EQueueType::Graphics ||
        query_source.native_queue_id !=
            topology.graphics.native_queue_id ||
        query_source.async_queue_scope !=
            async_queue_scope ||
        !query_source.parallel_record_requested ||
        query_source.parallel_record_planned ||
        query_source.parallel_record_effective) {
        throw std::runtime_error(
            "timestamp-bearing GPU scope source did not remain a query serial island"
        );
    }
    if (parallel_sibling.source_index != 1 ||
        parallel_sibling.original_source_index != 1 ||
        parallel_sibling.source_segment_index != 0 ||
        parallel_sibling.source_segment_count != 1 ||
        parallel_sibling.queue != EQueueType::Graphics ||
        parallel_sibling.native_queue_id !=
            topology.graphics.native_queue_id ||
        parallel_sibling.async_queue_scope !=
            async_queue_scope ||
        !parallel_sibling.parallel_record_requested ||
        !parallel_sibling.parallel_record_planned ||
        !parallel_sibling.parallel_record_effective) {
        throw std::runtime_error(
            "query-free sibling lost effective parallel recording"
        );
    }

    if (callback_errors.load(std::memory_order_acquire) != 0 ||
        completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        query_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        query_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 1) {
        throw std::runtime_error(
            "GPU scope focused callbacks violated Completion ownership or exactly-once delivery"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    ResolvedGpuScopeFrame replay{};
    const GpuScopeStreamStats stats = stream.GetStats();
    if (stream.TryPopFrame(replay) ||
        !translation_capture.IsValid() ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        query_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        query_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        stats.frames_ready != 1 ||
        stats.frames_popped != 1 ||
        stats.scopes_ready != 2 ||
        stats.resident_frames != 0 ||
        stats.resident_scopes != 0) {
        throw std::runtime_error(
            "a second RHI Sync replayed GPU scope publication or callbacks"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=GpuScopeStreamCompletionAndParallelIsolation "
        "frame_id={} queue=Graphics scopes=2 hierarchy=nested "
        "query_source=query-serial-island "
        "query_free_sibling=parallel-effective "
        "raw_ticks=verified duration_ns={},{} "
        "exclusive_ns={},{} owner=Completion "
        "readback=verified sibling_readbacks=8/8 replay=0",
        frame_id,
        outer.total_duration_ns,
        inner.total_duration_ns,
        outer.exclusive_duration_ns,
        inner.exclusive_duration_ns
    );
}

void RunTimestampQueryCompletionOwnership() {
    auto& device = RenderDevice::Get();
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST;

    BufferRef source = device.CreateBuffer<uint32>(
        "timestamp_query_source", kElementCount, usage
    );
    BufferRef destination = device.CreateBuffer<uint32>(
        "timestamp_query_destination", kElementCount, usage
    );

    std::array<uint32, kElementCount> values{};
    std::array<uint32, kElementCount> readback{};
    for (uint32 index = 0; index < values.size(); ++index) {
        values[index] = 0x170A0000u + index * 29u;
    }

    std::atomic<uint32> callback_count{0};
    std::atomic<uint32> callback_errors{0};
    std::atomic<uint32> completion_callback_count{0};
    std::atomic<uint32> ordinary_callback_count{0};
    std::atomic<uint32> callback_stage{0};
    FenceRef            query_done = device.CreateFence();

    CommandList commands(EQueueType::Graphics);
    GpuCompletionFuture gpu_completion =
        commands.TrackGpuCompletion(
            "Phase19AGraphicsCompletion"
        );
    commands.CopyFrom(
        OwnedBytes(values),
        source->GetView(),
        "TimestampQueryInitialize"
    );

    QueryToken  query  = commands.BeginTimestampQuery("Phase17ATimestamp");
    QueryFuture future = query.GetFuture();
    gpu_completion.Then([&](const GpuCompletionResult& _result) {
        const auto query_result = future.TryGet();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != GpuCompletionStatus::Ready ||
            query_done.Get()->GetValue() < 1 ||
            query_done.Get()->IsRejected(1) ||
            ResourceCast(query_done.Get())->IsFailed() ||
            !query_result.has_value() ||
            query_result->status != QueryStatus::Ready ||
            callback_stage.load(std::memory_order_acquire) != 0) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        callback_stage.store(1, std::memory_order_release);
        completion_callback_count.fetch_add(
            1, std::memory_order_release
        );
    });
    future.Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            _result.kind != QueryKind::Timestamp ||
            std::get_if<TimestampQueryResult>(&_result.payload) == nullptr ||
            query_done.Get()->GetValue() < 1 ||
            query_done.Get()->IsRejected(1) ||
            ResourceCast(query_done.Get())->IsFailed() ||
            gpu_completion.Status() !=
                GpuCompletionStatus::Ready ||
            callback_stage.load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        callback_stage.store(2, std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_release);
    });

    for (uint32 copy = 0; copy < 64; ++copy) {
        if ((copy & 1u) == 0) {
            commands.CopyFrom(
                source->GetView(),
                destination->GetView(),
                "TimestampQueryForwardCopy"
            );
        } else {
            commands.CopyFrom(
                destination->GetView(),
                source->GetView(),
                "TimestampQueryReverseCopy"
            );
        }
    }
    commands.EndTimestampQuery(query);
    commands.CopyFrom(
        destination->GetView(),
        WritableBytes(readback),
        "TimestampQueryReadback"
    );
    commands.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            callback_stage.load(std::memory_order_acquire) != 2) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        callback_stage.store(3, std::memory_order_release);
        ordinary_callback_count.fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit first_submit = commands.Submit();
    first_submit.Signal(query_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(first_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const GpuCompletionResult completion_result =
        gpu_completion.Get();
    const QueryResult result = future.Get();
    const auto* timestamp = std::get_if<TimestampQueryResult>(&result.payload);
    if (result.status != QueryStatus::Ready ||
        result.kind != QueryKind::Timestamp ||
        timestamp == nullptr ||
        timestamp->valid_bits == 0 ||
        timestamp->valid_bits > 64 ||
        timestamp->tick_period_ns <= 0.0 ||
        timestamp->duration_ns < 0.0) {
        throw std::runtime_error(
            "Vulkan timestamp query did not produce a valid Ready payload"
        );
    }
    if (callback_count.load(std::memory_order_acquire) != 1 ||
        completion_callback_count.load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callback_count.load(
            std::memory_order_acquire
        ) != 1 ||
        callback_stage.load(std::memory_order_acquire) != 3 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        completion_result.status !=
            GpuCompletionStatus::Ready) {
        throw std::runtime_error(
            "Vulkan completion/query callbacks violated Completion ordering"
        );
    }
    if (readback != values) {
        throw std::runtime_error(
            "Vulkan timestamp query workload readback mismatch"
        );
    }

    // A second submission after full Completion is expected to reuse a pooled
    // allocator. Its timestamp cursor must have returned to slot zero and the
    // recycled pool slots must be reset before the new writes.
    std::array<uint32, kElementCount> reused_readback{};
    std::atomic<uint32>               reused_callback_count{0};
    std::atomic<uint32>               reused_callback_errors{0};

    CommandList reused_commands(EQueueType::Graphics);
    QueryToken reused_query =
        reused_commands.BeginTimestampQuery("Phase17ATimestampAllocatorReuse");
    QueryFuture reused_future = reused_query.GetFuture();
    reused_future.Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            _result.kind != QueryKind::Timestamp ||
            std::get_if<TimestampQueryResult>(&_result.payload) == nullptr) {
            reused_callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        reused_callback_count.fetch_add(1, std::memory_order_release);
    });
    for (uint32 copy = 0; copy < 8; ++copy) {
        if ((copy & 1u) == 0) {
            reused_commands.CopyFrom(
                source->GetView(),
                destination->GetView(),
                "TimestampQueryReuseForwardCopy"
            );
        } else {
            reused_commands.CopyFrom(
                destination->GetView(),
                source->GetView(),
                "TimestampQueryReuseReverseCopy"
            );
        }
    }
    reused_commands.EndTimestampQuery(reused_query);
    reused_commands.CopyFrom(
        destination->GetView(),
        WritableBytes(reused_readback),
        "TimestampQueryReuseReadback"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        reused_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult reused_result = reused_future.Get();
    const auto* reused_timestamp =
        std::get_if<TimestampQueryResult>(&reused_result.payload);
    if (reused_result.status != QueryStatus::Ready ||
        reused_result.kind != QueryKind::Timestamp ||
        reused_timestamp == nullptr ||
        reused_timestamp->valid_bits == 0 ||
        reused_timestamp->valid_bits > 64 ||
        reused_timestamp->tick_period_ns <= 0.0 ||
        reused_timestamp->duration_ns < 0.0 ||
        reused_callback_count.load(std::memory_order_acquire) != 1 ||
        reused_callback_errors.load(std::memory_order_acquire) != 0 ||
        reused_readback != values) {
        throw std::runtime_error(
            "recycled Vulkan timestamp query allocator did not reset and reuse safely"
        );
    }

    // The original fixed allocator pool held 1000 slots (500 begin/end
    // pairs). Exercise the first legal packet beyond that boundary. Pool
    // sizing must happen before native Begin(), and every Future must still
    // retire Ready without latching the Vulkan runtime.
    constexpr uint32 large_query_pair_count = 501;
    CommandList      large_query_commands(EQueueType::Graphics);
    Array<QueryFuture> large_query_futures{};
    large_query_futures.reserve(large_query_pair_count);
    for (uint32 index = 0; index < large_query_pair_count; ++index) {
        QueryToken token = large_query_commands.BeginTimestampQuery(
            "Phase17ALargeTimestampPacket"
        );
        large_query_futures.emplace_back(token.GetFuture());
        large_query_commands.EndTimestampQuery(token);
    }

    FenceRef large_query_done = device.CreateFence();
    CmdSubmit large_query_submit = large_query_commands.Submit();
    large_query_submit.Signal(large_query_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(large_query_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!large_query_done.Get()->WaitSubmitted(1) ||
        large_query_done.Get()->IsRejected(1) ||
        ResourceCast(large_query_done.Get())->IsFailed()) {
        throw std::runtime_error(
            "large Vulkan timestamp packet was rejected at the old pool boundary"
        );
    }
    for (const QueryFuture& large_future : large_query_futures) {
        const QueryResult large_result = large_future.Get();
        const auto* large_timestamp =
            std::get_if<TimestampQueryResult>(&large_result.payload);
        if (large_result.status != QueryStatus::Ready ||
            large_result.kind != QueryKind::Timestamp ||
            large_timestamp == nullptr ||
            large_timestamp->valid_bits == 0 ||
            large_timestamp->valid_bits > 64 ||
            large_timestamp->duration_ns < 0.0) {
            throw std::runtime_error(
                "large Vulkan timestamp packet did not resolve every Future"
            );
        }
    }

    FenceRef post_growth_done = device.CreateFence();
    CommandList post_growth_commands(EQueueType::Graphics);
    CmdSubmit post_growth_submit = post_growth_commands.Submit();
    post_growth_submit.Signal(post_growth_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(post_growth_submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!post_growth_done.Get()->WaitSubmitted(1) ||
        post_growth_done.Get()->IsRejected(1) ||
        ResourceCast(post_growth_done.Get())->IsFailed()) {
        throw std::runtime_error(
            "timestamp pool growth left the Vulkan runtime latched"
        );
    }

    std::atomic<uint32> late_callback_count{0};
    future.Then([&](const QueryResult& _result) {
        if (_result.status == QueryStatus::Ready) {
            late_callback_count.fetch_add(1, std::memory_order_release);
        }
    });
    if (late_callback_count.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error(
            "terminal Vulkan timestamp query did not invoke a late callback"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (callback_count.load(std::memory_order_acquire) != 1 ||
        reused_callback_count.load(std::memory_order_acquire) != 1 ||
        late_callback_count.load(std::memory_order_acquire) != 1) {
        throw std::runtime_error(
            "a second RHI Sync replayed Vulkan timestamp query callbacks"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=TimestampQueryCompletionOwnership "
        "status=Ready gpu_completion=Ready owner=Completion "
        "order=signal->completion->query->ordinary "
        "allocator_slot_reuse=verified large_query_pairs={} "
        "post_growth_submit=accepted valid_bits={} duration_ns={} "
        "reused_duration_ns={} readback=verified replay=0",
        large_query_pair_count,
        timestamp->valid_bits,
        timestamp->duration_ns,
        reused_timestamp->duration_ns
    );
}

void RunOcclusionQueryCompletionOwnership() {
    auto& device = RenderDevice::Get();
    const auto* vulkan_device =
        static_cast<const VulkanDevice*>(device.GetImpl());
    const bool precise_sample_count =
        vulkan_device != nullptr &&
        vulkan_device->GetCoreFeatures().core_1_0.occlusionQueryPrecise ==
            VK_TRUE;
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;

    BufferRef source = device.CreateBuffer<uint32>(
        "occlusion_query_source", kElementCount, usage
    );
    BufferRef destination = device.CreateBuffer<uint32>(
        "occlusion_query_destination", kElementCount, usage
    );
    std::array<uint32, kElementCount> values{};
    std::array<uint32, kElementCount> readback{};
    for (uint32 index = 0; index < values.size(); ++index) {
        values[index] = 0x200C0000u + index * 31u;
    }

    std::atomic<uint32> callback_count{0};
    std::atomic<uint32> completion_callback_count{0};
    std::atomic<uint32> ordinary_callback_count{0};
    std::atomic<uint32> callback_errors{0};
    std::atomic<uint32> callback_stage{0};
    FenceRef            query_done = device.CreateFence();

    CommandList commands(EQueueType::Graphics);
    GpuCompletionFuture gpu_completion =
        commands.TrackGpuCompletion("Phase20OcclusionCompletion");
    commands.CopyFrom(
        OwnedBytes(values),
        source->GetView(),
        "OcclusionQueryInitialize"
    );

    QueryToken query =
        commands.BeginOcclusionQuery("Phase20Occlusion");
    QueryFuture future = query.GetFuture();
    gpu_completion.Then([&](const GpuCompletionResult& _result) {
        const std::optional<QueryResult> query_result =
            future.TryGet();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != GpuCompletionStatus::Ready ||
            !query_result.has_value() ||
            query_result->status != QueryStatus::Ready ||
            query_done.Get()->GetValue() < 1 ||
            query_done.Get()->IsRejected(1) ||
            ResourceCast(query_done.Get())->IsFailed() ||
            callback_stage.load(std::memory_order_acquire) != 0) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        callback_stage.store(1, std::memory_order_release);
        completion_callback_count.fetch_add(
            1, std::memory_order_release
        );
    });
    future.Then([&](const QueryResult& _result) {
        const auto* occlusion =
            std::get_if<OcclusionQueryResult>(&_result.payload);
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            _result.kind != QueryKind::Occlusion ||
            !IsExpectedZeroOcclusionResult(
                occlusion, precise_sample_count
            ) ||
            gpu_completion.Status() !=
                GpuCompletionStatus::Ready ||
            callback_stage.load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        callback_stage.store(2, std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_release);
    });

    // A visibility query can legally span non-raster commands. This gives the
    // real Vulkan path observable work while retaining the deterministic
    // zero-sample result needed by the headless gate.
    for (uint32 copy = 0; copy < 16; ++copy) {
        if ((copy & 1u) == 0) {
            commands.CopyFrom(
                source->GetView(),
                destination->GetView(),
                "OcclusionQueryForwardCopy"
            );
        } else {
            commands.CopyFrom(
                destination->GetView(),
                source->GetView(),
                "OcclusionQueryReverseCopy"
            );
        }
    }
    commands.EndOcclusionQuery(query);
    commands.CopyFrom(
        destination->GetView(),
        WritableBytes(readback),
        "OcclusionQueryReadback"
    );
    commands.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            callback_stage.load(std::memory_order_acquire) != 2) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        callback_stage.store(3, std::memory_order_release);
        ordinary_callback_count.fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit submit = commands.Submit();
    submit.Signal(query_done.Get(), 1);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(submit),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const GpuCompletionResult completion_result =
        gpu_completion.Get();
    const QueryResult result = future.Get();
    const auto* occlusion =
        std::get_if<OcclusionQueryResult>(&result.payload);
    if (result.status != QueryStatus::Ready ||
        result.kind != QueryKind::Occlusion ||
        !IsExpectedZeroOcclusionResult(
            occlusion, precise_sample_count
        ) ||
        completion_result.status !=
            GpuCompletionStatus::Ready ||
        callback_count.load(std::memory_order_acquire) != 1 ||
        completion_callback_count.load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callback_count.load(
            std::memory_order_acquire
        ) != 1 ||
        callback_stage.load(std::memory_order_acquire) != 3 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        readback != values) {
        throw std::runtime_error(
            "Vulkan occlusion query violated result or Completion ordering"
        );
    }

    constexpr size_t bulk_pair_count = 48;
    std::array<QueryFuture, bulk_pair_count> bulk_futures{};
    std::array<std::atomic<uint32>, bulk_pair_count>
        bulk_callback_counts{};
    std::atomic<uint32> bulk_callback_errors{0};
    CommandList bulk_commands(EQueueType::Graphics);
    for (size_t index = 0; index < bulk_pair_count; ++index) {
        QueryToken bulk_query =
            bulk_commands.BeginOcclusionQuery(
                "Phase20OcclusionBulk"
            );
        bulk_futures[index] = bulk_query.GetFuture();
        bulk_futures[index].Then(
            [&, index](const QueryResult& _result) {
                const auto* bulk_occlusion =
                    std::get_if<OcclusionQueryResult>(
                        &_result.payload
                    );
                if (GetCurrentRHIThreadRole() !=
                        ERHIThreadRole::Completion ||
                    _result.status != QueryStatus::Ready ||
                    _result.kind != QueryKind::Occlusion ||
                    !IsExpectedZeroOcclusionResult(
                        bulk_occlusion, precise_sample_count
                    )) {
                    bulk_callback_errors.fetch_add(
                        1, std::memory_order_relaxed
                    );
                }
                bulk_callback_counts[index].fetch_add(
                    1, std::memory_order_release
                );
            }
        );
        bulk_commands.EndOcclusionQuery(bulk_query);
    }
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        bulk_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (size_t index = 0; index < bulk_pair_count; ++index) {
        const QueryResult bulk_result =
            bulk_futures[index].Get();
        const auto* bulk_occlusion =
            std::get_if<OcclusionQueryResult>(
                &bulk_result.payload
            );
        if (bulk_result.status != QueryStatus::Ready ||
            bulk_result.kind != QueryKind::Occlusion ||
            !IsExpectedZeroOcclusionResult(
                bulk_occlusion, precise_sample_count
            ) ||
            bulk_callback_counts[index].load(
                std::memory_order_acquire
            ) != 1) {
            throw std::runtime_error(
                "bulk Vulkan occlusion query did not resolve exactly once"
            );
        }
    }
    if (bulk_callback_errors.load(
            std::memory_order_acquire
        ) != 0) {
        throw std::runtime_error(
            "bulk Vulkan occlusion query callbacks left Completion ownership"
        );
    }

    std::atomic<uint32> reused_callback_count{0};
    CommandList reused_commands(EQueueType::Graphics);
    QueryToken reused_query =
        reused_commands.BeginOcclusionQuery(
            "Phase20OcclusionPostGrowthReuse"
        );
    QueryFuture reused_future = reused_query.GetFuture();
    reused_future.Then([&](const QueryResult& _result) {
        const auto* reused_occlusion =
            std::get_if<OcclusionQueryResult>(&_result.payload);
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            _result.kind != QueryKind::Occlusion ||
            !IsExpectedZeroOcclusionResult(
                reused_occlusion, precise_sample_count
            )) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        reused_callback_count.fetch_add(
            1, std::memory_order_release
        );
    });
    reused_commands.EndOcclusionQuery(reused_query);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        reused_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult reused_result = reused_future.Get();
    const auto* reused_occlusion =
        std::get_if<OcclusionQueryResult>(
            &reused_result.payload
        );
    if (reused_result.status != QueryStatus::Ready ||
        reused_result.kind != QueryKind::Occlusion ||
        !IsExpectedZeroOcclusionResult(
            reused_occlusion, precise_sample_count
        ) ||
        reused_callback_count.load(
            std::memory_order_acquire
        ) != 1 ||
        callback_errors.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "Vulkan occlusion query allocator reuse failed"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=OcclusionQueryCompletionOwnership "
        "queue=Graphics status=Ready samples={} visible={} "
        "count_precise={} "
        "availability=explicit pool=allocator_local bulk_pairs={} "
        "growth=verified reuse=verified "
        "recording=serial_island "
        "order=signal->completion->query->ordinary "
        "callbacks=exactly_once readback=verified replay=0",
        occlusion->sample_count.value_or(0),
        occlusion->visible,
        precise_sample_count,
        bulk_pair_count
    );
}

void RunTimestampQuerySuccessfulBatchPublication() {
    using namespace std::chrono_literals;

    auto&            device            = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x5048313751534241ull;

    std::array<FenceRef, 2> signals{
        device.CreateFence(),
        device.CreateFence(),
    };
    std::array<QueryFuture, 2> futures{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::atomic<uint32>                callback_errors{0};

    CommandList first(EQueueType::Graphics);
    first.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken first_query =
        first.BeginTimestampQuery("Phase17ASuccessBatchFirst");
    futures[0] = first_query.GetFuture();
    first.EndTimestampQuery(first_query);

    CommandList second(EQueueType::Graphics);
    second.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken second_query =
        second.BeginTimestampQuery("Phase17ASuccessBatchSecond");
    futures[1] = second_query.GetFuture();
    second.EndTimestampQuery(second_query);

    auto signal_succeeded = [&](size_t _index) {
        auto* fence = static_cast<VulkanFence*>(
            ResourceCast(signals[_index].Get())
        );
        return fence != nullptr && signals[_index]->GetValue() >= 1 &&
               !fence->IsRejected(1) && !fence->IsFailed();
    };
    auto peer_ready = [&](size_t _index) {
        if (!futures[_index].WaitFor(500ms)) {
            return false;
        }
        const std::optional<QueryResult> result =
            futures[_index].TryGet();
        return result.has_value() &&
               result->status == QueryStatus::Ready &&
               result->kind == QueryKind::Timestamp;
    };

    futures[0].Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !peer_ready(1) ||
            !signal_succeeded(0) ||
            !signal_succeeded(1)) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    futures[1].Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !peer_ready(0) ||
            !signal_succeeded(0) ||
            !signal_succeeded(1)) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[1].fetch_add(1, std::memory_order_release);
    });

    first.AddCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !peer_ready(1) ||
            query_callbacks[0].load(std::memory_order_acquire) != 1 ||
            query_callbacks[1].load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    second.AddCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !peer_ready(0) ||
            query_callbacks[0].load(std::memory_order_acquire) != 1 ||
            query_callbacks[1].load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[1].fetch_add(1, std::memory_order_release);
    });
    first.AddSuccessCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        success_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    second.AddSuccessCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[1].load(std::memory_order_acquire) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        success_callbacks[1].fetch_add(1, std::memory_order_release);
    });

    CmdSubmit first_submit = first.Submit();
    first_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    first_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    first_submit.async_queue_scope = async_queue_scope;
    first_submit.Signal(signals[0].Get(), 1);

    CmdSubmit second_submit = second.Submit();
    second_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    second_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    second_submit.async_queue_scope = async_queue_scope;
    second_submit.Signal(signals[1].Get(), 1);

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(first_submit)
    );
    batch.emplace_back(
        EQueueType::Graphics, std::move(second_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    for (size_t index = 0; index < futures.size(); ++index) {
        const QueryResult result = futures[index].Get();
        if (result.status != QueryStatus::Ready ||
            result.kind != QueryKind::Timestamp ||
            !signal_succeeded(index) ||
            query_callbacks[index].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "successful timestamp batch did not retire exactly once"
            );
        }
    }
    if (callback_errors.load(std::memory_order_acquire) != 0 ||
        source_capture.Overflowed() || source_capture.Count() != 2 ||
        native_capture.Overflowed() || native_capture.Count() != 2) {
        throw std::runtime_error(
            "successful timestamp batch exposed a partial terminal frontier"
        );
    }

    const uint64 batch_sequence =
        source_capture.Event(0).batch_sequence;
    if (batch_sequence == 0) {
        throw std::runtime_error(
            "successful timestamp batch lost its backend sequence"
        );
    }
    for (size_t index = 0; index < 2; ++index) {
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(index);
        const VulkanNativeSubmissionEvent& native_event =
            native_capture.Event(index);
        if (source_event.batch_sequence != batch_sequence ||
            source_event.source_index != index ||
            source_event.queue != EQueueType::Graphics ||
            source_event.async_queue_scope != async_queue_scope ||
            native_event.queue != EQueueType::Graphics ||
            native_event.thread_role != ERHIThreadRole::Submission ||
            !native_event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "successful timestamp batch escaped stable Submission ordering"
            );
        }
    }
    if (native_capture.Event(0).native_queue_handle !=
        native_capture.Event(1).native_queue_handle) {
        throw std::runtime_error(
            "successful timestamp batch did not exercise one native queue"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (size_t index = 0; index < futures.size(); ++index) {
        if (query_callbacks[index].load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
            success_callbacks[index].load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "a second RHI Sync replayed successful batch callbacks"
            );
        }
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=TimestampQuerySuccessfulBatchPublication "
        "sources=2 queues=Graphics,Graphics batch_sequence={} "
        "signals=terminal_before_query_callbacks "
        "queries=batch_published_before_notify "
        "cross_future_wait=ready owner=Completion "
        "query_callbacks=exactly_once "
        "ordinary_callbacks=exactly_once "
        "success_callbacks=exactly_once "
        "native_submit=2 native_owner=Submission replay=0",
        batch_sequence
    );
}

void RunTimestampQueryCrossQueueBatchPublication() {
    using namespace std::chrono_literals;

    auto& device = RenderDevice::Get();
    const RHIQueueTopology topology = device.GetQueueTopology();
    if (!topology.graphics.available || !topology.compute.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] "
            "name=TimestampQueryCrossQueueBatchPublication "
            "reason=queue_unavailable"
        );
        return;
    }

    constexpr uint64 async_queue_scope = 0x504831375143524Full;
    const bool native_alias =
        topology.graphics.native_queue_id ==
        topology.compute.native_queue_id;

    std::array<FenceRef, 2> signals{
        device.CreateFence(),
        device.CreateFence(),
    };
    std::array<QueryFuture, 2> futures{};
    std::array<GpuCompletionFuture, 2> completions{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> completion_callbacks{};
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::atomic<uint32> callback_errors{0};
    std::atomic<uint32> queue_sync_returns{0};
    std::atomic<uint32> sync_before_callback_finished{0};
    std::atomic<bool> blocking_callback_finished{false};
    std::binary_semaphore blocking_callback_entered{0};
    std::binary_semaphore release_blocking_callback{0};
    std::counting_semaphore<2> queue_sync_returned{0};
    QueueLocalSyncWaitCapture queue_sync_wait_capture{};
    OneShotSemaphoreRelease release_callback_guard(
        release_blocking_callback
    );

    auto signal_succeeded = [&](size_t _index) {
        auto* fence = static_cast<VulkanFence*>(
            ResourceCast(signals[_index].Get())
        );
        return fence != nullptr && signals[_index]->GetValue() >= 1 &&
               !fence->IsRejected(1) && !fence->IsFailed();
    };
    auto peer_ready = [&](size_t _index) {
        if (!futures[_index].WaitFor(500ms)) {
            return false;
        }
        const std::optional<QueryResult> result =
            futures[_index].TryGet();
        return result.has_value() &&
               result->status == QueryStatus::Ready &&
               result->kind == QueryKind::Timestamp;
    };

    CommandList graphics(EQueueType::Graphics);
    graphics.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    completions[0] = graphics.TrackGpuCompletion(
        "Phase19ACrossQueueGraphicsCompletion"
    );
    QueryToken graphics_query =
        graphics.BeginTimestampQuery("Phase17ACrossQueueGraphics");
    futures[0] = graphics_query.GetFuture();
    graphics.EndTimestampQuery(graphics_query);

    CommandList compute(EQueueType::Compute);
    compute.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    completions[1] = compute.TrackGpuCompletion(
        "Phase19ACrossQueueComputeCompletion"
    );
    QueryToken compute_query =
        compute.BeginTimestampQuery("Phase17ACrossQueueCompute");
    futures[1] = compute_query.GetFuture();
    compute.EndTimestampQuery(compute_query);

    for (size_t index = 0; index < completions.size(); ++index) {
        completions[index].Then(
            [&, index](const GpuCompletionResult& _result) {
                const size_t peer = 1 - index;
                const auto peer_completion =
                    completions[peer].TryGet();
                const auto own_query = futures[index].TryGet();
                const auto peer_query = futures[peer].TryGet();
                if (GetCurrentRHIThreadRole() !=
                        ERHIThreadRole::Completion ||
                    _result.status !=
                        GpuCompletionStatus::Ready ||
                    !peer_completion.has_value() ||
                    peer_completion->status !=
                        GpuCompletionStatus::Ready ||
                    !own_query.has_value() ||
                    own_query->status != QueryStatus::Ready ||
                    !peer_query.has_value() ||
                    peer_query->status != QueryStatus::Ready ||
                    !signal_succeeded(0) ||
                    !signal_succeeded(1)) {
                    callback_errors.fetch_add(
                        1, std::memory_order_relaxed
                    );
                }
                completion_callbacks[index].fetch_add(
                    1, std::memory_order_release
                );
            }
        );
    }

    futures[0].Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !peer_ready(1) ||
            completions[0].Status() !=
                GpuCompletionStatus::Ready ||
            completions[1].Status() !=
                GpuCompletionStatus::Ready ||
            completion_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            completion_callbacks[1].load(
                std::memory_order_acquire
            ) != 1 ||
            !signal_succeeded(0) ||
            !signal_succeeded(1)) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    futures[1].Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !peer_ready(0) ||
            completions[0].Status() !=
                GpuCompletionStatus::Ready ||
            completions[1].Status() !=
                GpuCompletionStatus::Ready ||
            completion_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            completion_callbacks[1].load(
                std::memory_order_acquire
            ) != 1 ||
            !signal_succeeded(0) ||
            !signal_succeeded(1)) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });

    graphics.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            query_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            query_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        blocking_callback_entered.release();
        release_blocking_callback.acquire();
        ordinary_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
        blocking_callback_finished.store(
            true, std::memory_order_release
        );
    });
    compute.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            query_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            query_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    graphics.AddSuccessCallback([&] {
        if (ordinary_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        success_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    compute.AddSuccessCallback([&] {
        if (ordinary_callbacks[0].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[1].load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        success_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    graphics_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    graphics_submit.async_queue_scope = async_queue_scope;
    graphics_submit.Signal(signals[0].Get(), 1);

    CmdSubmit compute_submit = compute.Submit();
    compute_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    compute_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    compute_submit.async_queue_scope = async_queue_scope;
    compute_submit.Signal(signals[1].Get(), 1);

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(graphics_submit)
    );
    batch.emplace_back(
        EQueueType::Compute, std::move(compute_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );

    if (!blocking_callback_entered.try_acquire_for(5s)) {
        throw std::runtime_error(
            "cross-queue timestamp batch did not enter its grouped callback"
        );
    }

    auto queue_sync = [&](EQueueType _queue) {
        device.GetCommandQueue(_queue).Sync();
        if (!blocking_callback_finished.load(
                std::memory_order_acquire
            )) {
            sync_before_callback_finished.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        queue_sync_returns.fetch_add(
            1, std::memory_order_release
        );
        queue_sync_returned.release();
    };

    bool both_sync_waits_entered = false;
    bool sync_returned_before_release = false;
    size_t observed_sync_returns = 0;
    {
        ScopedQueueLocalSyncWaitObserver queue_sync_wait_observer(
            queue_sync_wait_capture
        );
        std::jthread graphics_sync_thread(
            [&] { queue_sync(EQueueType::Graphics); }
        );
        std::jthread compute_sync_thread(
            [&] { queue_sync(EQueueType::Compute); }
        );

        // The observer fires only after CompleteAll has captured the target
        // retirement serial and the associated Completion group. This closes
        // the scheduler gap between starting the helper and actually entering
        // the wait contract.
        both_sync_waits_entered =
            queue_sync_wait_capture.WaitForTwo(5s);
        if (both_sync_waits_entered &&
            queue_sync_returned.try_acquire_for(200ms)) {
            sync_returned_before_release = true;
            observed_sync_returns = 1;
        }

        release_callback_guard.Release();
        while (observed_sync_returns < 2 &&
               queue_sync_returned.try_acquire_for(5s)) {
            ++observed_sync_returns;
        }
        graphics_sync_thread.join();
        compute_sync_thread.join();
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    for (size_t index = 0; index < futures.size(); ++index) {
        const QueryResult result = futures[index].Get();
        const GpuCompletionResult completion =
            completions[index].Get();
        if (result.status != QueryStatus::Ready ||
            completion.status != GpuCompletionStatus::Ready ||
            result.kind != QueryKind::Timestamp ||
            !signal_succeeded(index) ||
            query_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            completion_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            success_callbacks[index].load(
                std::memory_order_acquire
            ) != 1) {
            throw std::runtime_error(
                "cross-queue timestamp batch did not retire exactly once"
            );
        }
    }
    if (!both_sync_waits_entered ||
        !queue_sync_wait_capture.ValidGraphicsComputePair() ||
        sync_returned_before_release ||
        observed_sync_returns != 2 ||
        queue_sync_returns.load(std::memory_order_acquire) != 2 ||
        sync_before_callback_finished.load(
            std::memory_order_acquire
        ) != 0 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        source_capture.Overflowed() ||
        source_capture.Count() != 2 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 2) {
        throw std::runtime_error(
            "queue-local Sync escaped the cross-queue completion group"
        );
    }

    constexpr std::array expected_queues{
        EQueueType::Graphics,
        EQueueType::Compute,
    };
    const uint64 batch_sequence =
        source_capture.Event(0).batch_sequence;
    for (size_t index = 0; index < expected_queues.size(); ++index) {
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(index);
        const VulkanNativeSubmissionEvent& native_event =
            native_capture.Event(index);
        if (batch_sequence == 0 ||
            source_event.batch_sequence != batch_sequence ||
            source_event.source_index != index ||
            source_event.queue != expected_queues[index] ||
            source_event.async_queue_scope != async_queue_scope ||
            native_event.queue != expected_queues[index] ||
            native_event.thread_role != ERHIThreadRole::Submission ||
            !native_event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "cross-queue timestamp batch lost stable submission ownership"
            );
        }
    }
    if ((native_capture.Event(0).native_queue_handle ==
         native_capture.Event(1).native_queue_handle) != native_alias) {
        throw std::runtime_error(
            "cross-queue timestamp batch disagreed with queue topology"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (size_t index = 0; index < futures.size(); ++index) {
        if (query_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            completion_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            ordinary_callbacks[index].load(
                std::memory_order_acquire
            ) != 1 ||
            success_callbacks[index].load(
                std::memory_order_acquire
            ) != 1) {
            throw std::runtime_error(
                "a second Sync replayed cross-queue batch callbacks"
            );
        }
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=TimestampQueryCrossQueueBatchPublication "
        "sources=2 queues=Graphics,Compute native_alias={} "
        "queries=batch_published_before_notify "
        "completions=batch_published_before_notify "
        "cross_future_wait=ready owner=Completion "
        "queue_local_sync=blocked_until_group_settled "
        "callbacks=exactly_once replay=0",
        native_alias
    );
}

void RunTimestampQueryMixedMultiSegmentCallbackTiers() {
    auto& device = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x50483137514D4958ull;

    std::array<FenceRef, 2> signals{
        device.CreateFence(),
        device.CreateFence(),
    };
    std::binary_semaphore first_translated{0};
    std::binary_semaphore second_translated{0};
    std::atomic<uint32> phase{0};
    std::atomic<uint32> callback_errors{0};
    std::atomic<uint32> query_callbacks{0};
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};

    auto signal_succeeded = [&](size_t _index) {
        auto* fence = static_cast<VulkanFence*>(
            ResourceCast(signals[_index].Get())
        );
        return fence != nullptr && signals[_index]->GetValue() >= 1 &&
               !fence->IsRejected(1) && !fence->IsFailed();
    };
    auto advance_phase = [&](uint32 _expected) {
        if (phase.fetch_add(1, std::memory_order_acq_rel) !=
            _expected) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
    };

    CommandList multi_segment(EQueueType::Graphics);
    multi_segment.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    multi_segment.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(
            &first_translated, EQueueType::Graphics
        ),
        "Phase17AMixedTierFirstSegment"
    );
    multi_segment.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(
            &second_translated, EQueueType::Graphics
        ),
        "Phase17AMixedTierSecondSegment"
    );
    multi_segment.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
            ERHIThreadRole::Completion) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        advance_phase(1);
        ordinary_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    multi_segment.AddSuccessCallback([&] {
        advance_phase(3);
        success_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });

    CommandList query_source(EQueueType::Graphics);
    query_source.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken query =
        query_source.BeginTimestampQuery("Phase17AMixedTierQuery");
    QueryFuture future = query.GetFuture();
    query_source.EndTimestampQuery(query);
    future.Then([&](const QueryResult& _result) {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !signal_succeeded(0) ||
            !signal_succeeded(1)) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        advance_phase(0);
        query_callbacks.fetch_add(
            1, std::memory_order_release
        );
    });
    query_source.AddCallback([&] {
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            query_callbacks.load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        advance_phase(2);
        ordinary_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    query_source.AddSuccessCallback([&] {
        advance_phase(4);
        success_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit multi_submit = multi_segment.Submit();
    multi_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    multi_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    multi_submit.async_queue_scope = async_queue_scope;
    multi_submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };
    multi_submit.Signal(signals[0].Get(), 1);

    CmdSubmit query_submit = query_source.Submit();
    query_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    query_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    query_submit.async_queue_scope = async_queue_scope;
    query_submit.Signal(signals[1].Get(), 1);

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(multi_submit)
    );
    batch.emplace_back(
        EQueueType::Graphics, std::move(query_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult result = future.Get();
    if (result.status != QueryStatus::Ready ||
        result.kind != QueryKind::Timestamp ||
        phase.load(std::memory_order_acquire) != 5 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        query_callbacks.load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        !signal_succeeded(0) ||
        !signal_succeeded(1) ||
        !first_translated.try_acquire() ||
        !second_translated.try_acquire() ||
        source_capture.Overflowed() ||
        source_capture.Count() != 3 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 3) {
        throw std::runtime_error(
            "mixed multi-segment timestamp batch violated callback tiers"
        );
    }

    constexpr std::array expected_segment_indices{
        uint32{0}, uint32{1}, uint32{0}
    };
    constexpr std::array expected_segment_counts{
        uint32{2}, uint32{2}, uint32{1}
    };
    constexpr std::array expected_original_sources{
        uint32{0}, uint32{0}, uint32{1}
    };
    const uint64 batch_sequence =
        source_capture.Event(0).batch_sequence;
    for (size_t index = 0; index < 3; ++index) {
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(index);
        if (batch_sequence == 0 ||
            source_event.batch_sequence != batch_sequence ||
            source_event.source_index != index ||
            source_event.original_source_index !=
                expected_original_sources[index] ||
            source_event.source_segment_index !=
                expected_segment_indices[index] ||
            source_event.source_segment_count !=
                expected_segment_counts[index] ||
            source_event.queue != EQueueType::Graphics ||
            source_event.async_queue_scope != async_queue_scope ||
            native_capture.Event(index).queue !=
                EQueueType::Graphics ||
            native_capture.Event(index).thread_role !=
                ERHIThreadRole::Submission ||
            !native_capture.Event(index).outcome.WasSubmitted()) {
            throw std::runtime_error(
                "mixed timestamp batch lost materialized source identity"
            );
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (phase.load(std::memory_order_acquire) != 5 ||
        query_callbacks.load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 1) {
        throw std::runtime_error(
            "a second Sync replayed mixed topology callbacks"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=TimestampQueryMixedMultiSegmentCallbackTiers "
        "original_sources=2 executable_sources=3 "
        "source0_segments=2 source1_query=true "
        "order=Query,AllOrdinary,AllSuccess "
        "owner=Completion callbacks=exactly_once replay=0"
    );
}

void RunTimestampQueryCopyOnlyPayloadArrival() {
    auto& device = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x5048313751434F50ull;

    QueryFuture graphics_future{};
    QueryFuture copy_future{};
    GpuCompletionFuture copy_completion{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::atomic<uint32> completion_callbacks{0};
    std::atomic<uint32> callback_errors{0};
    FenceRef graphics_done = device.CreateFence();

    CommandList graphics(EQueueType::Graphics);
    graphics.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken graphics_query =
        graphics.BeginTimestampQuery("Phase17ACopyOnlyPeer");
    graphics_future = graphics_query.GetFuture();
    graphics.EndTimestampQuery(graphics_query);

    // Keep a backend-internal query-only payload alongside the real Copy query
    // coverage below. Stripping its native commands exercises the executable
    // source contract that batch admission and rejection paths may construct.
    CommandList copy_payload_builder(EQueueType::Graphics);
    copy_completion =
        copy_payload_builder.TrackGpuCompletion(
            "Phase19ACopyPayloadCompletion"
        );
    QueryToken copy_query =
        copy_payload_builder.BeginTimestampQuery(
            "Phase17ACopyOnlyPayload"
        );
    copy_future = copy_query.GetFuture();
    copy_payload_builder.EndTimestampQuery(copy_query);
    CmdSubmit copy_submit = copy_payload_builder.Submit();
    copy_submit.cmds.clear();
    copy_submit.callbacks.clear();
    copy_submit.success_callbacks.clear();
    copy_submit.cached_args.clear();
    copy_submit.segments.clear();
    copy_submit.wait_events.clear();
    copy_submit.signal_events.clear();
    copy_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    copy_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    copy_submit.async_queue_scope = async_queue_scope;

    auto graphics_signal_succeeded = [&] {
        auto* fence = static_cast<VulkanFence*>(
            ResourceCast(graphics_done.Get())
        );
        return fence != nullptr && graphics_done->GetValue() >= 1 &&
               !fence->IsRejected(1) && !fence->IsFailed();
    };
    graphics_future.Then([&](const QueryResult& _result) {
        const std::optional<QueryResult> copy_result =
            copy_future.TryGet();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Ready ||
            !copy_result.has_value() ||
            copy_result->status != QueryStatus::Error ||
            copy_result->error_reason.empty() ||
            !graphics_signal_succeeded()) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    copy_future.Then([&](const QueryResult& _result) {
        const std::optional<QueryResult> graphics_result =
            graphics_future.TryGet();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != QueryStatus::Error ||
            _result.error_reason.empty() ||
            !graphics_result.has_value() ||
            graphics_result->status != QueryStatus::Ready ||
            !graphics_signal_succeeded()) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    copy_completion.Then([&](const GpuCompletionResult& _result) {
        const auto graphics_result = graphics_future.TryGet();
        const auto copy_result = copy_future.TryGet();
        if (GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            _result.status != GpuCompletionStatus::Ready ||
            !graphics_result.has_value() ||
            graphics_result->status != QueryStatus::Ready ||
            !copy_result.has_value() ||
            copy_result->status != QueryStatus::Error ||
            !graphics_signal_succeeded()) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        completion_callbacks.fetch_add(
            1, std::memory_order_release
        );
    });

    CmdSubmit graphics_submit = graphics.Submit();
    graphics_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    graphics_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    graphics_submit.async_queue_scope = async_queue_scope;
    graphics_submit.Signal(graphics_done.Get(), 1);

    // A completion-only Copy submit must still cross the native queue and
    // reach Completion. This specifically guards the Copy translator's
    // has_queue_work classification.
    CommandList completion_only_copy(EQueueType::Copy);
    GpuCompletionFuture completion_only_future =
        completion_only_copy.TrackGpuCompletion(
            "Phase19ACompletionOnlyCopy"
        );
    std::atomic<uint32> completion_only_callbacks{0};
    completion_only_future.Then(
        [&](const GpuCompletionResult& _result) {
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                _result.status != GpuCompletionStatus::Ready) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            completion_only_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        completion_only_copy.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (completion_only_future.Get().status !=
            GpuCompletionStatus::Ready ||
        completion_only_callbacks.load(
            std::memory_order_acquire
        ) != 1) {
        throw std::runtime_error(
            "completion-only Copy submit did not reach Completion"
        );
    }

    SourceSubmissionCapture        source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(graphics_submit)
    );
    batch.emplace_back(EQueueType::Copy, std::move(copy_submit));
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult graphics_result = graphics_future.Get();
    const QueryResult copy_result = copy_future.Get();
    const GpuCompletionResult copy_completion_result =
        copy_completion.Get();
    if (graphics_result.status != QueryStatus::Ready ||
        copy_result.status != QueryStatus::Error ||
        copy_completion_result.status !=
            GpuCompletionStatus::Ready ||
        copy_result.error_reason.empty() ||
        query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        completion_callbacks.load(std::memory_order_acquire) != 1 ||
        completion_only_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        !graphics_signal_succeeded() ||
        source_capture.Overflowed() ||
        source_capture.Count() != 2 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 2) {
        throw std::runtime_error(
            "query-only Copy payload did not arrive at Completion group"
        );
    }

    constexpr std::array expected_queues{
        EQueueType::Graphics,
        EQueueType::Copy,
    };
    const uint64 batch_sequence =
        source_capture.Event(0).batch_sequence;
    for (size_t index = 0; index < expected_queues.size(); ++index) {
        const VulkanSourceSubmissionEvent& source_event =
            source_capture.Event(index);
        const VulkanNativeSubmissionEvent& native_event =
            native_capture.Event(index);
        if (batch_sequence == 0 ||
            source_event.batch_sequence != batch_sequence ||
            source_event.source_index != index ||
            source_event.queue != expected_queues[index] ||
            source_event.async_queue_scope != async_queue_scope ||
            native_event.queue != expected_queues[index] ||
            native_event.thread_role != ERHIThreadRole::Submission ||
            !native_event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "query-only Copy payload lost stable source ownership"
            );
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        source_capture.Count() != 2 ||
        native_capture.Count() != 2) {
        throw std::runtime_error(
            "a second Sync replayed query-only Copy retirement"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=TimestampQueryCopyOnlyPayloadArrival "
        "sources=2 queues=Graphics,Copy "
        "copy_payload=query_only copy_query=Error "
        "copy_completion=Ready completion_only_copy=Ready "
        "group_arrival=verified owner=Completion "
        "callbacks=exactly_once native_submit=2 replay=0"
    );
}

void RunCopyTimestampQueryCapabilityAndIsolation() {
    auto& device = RenderDevice::Get();
    auto* vk_device =
        static_cast<VulkanDevice*>(device.GetImpl());
    if (vk_device == nullptr) {
        throw std::runtime_error(
            "Copy timestamp test could not access the Vulkan device"
        );
    }

    const uint32 native_valid_bits =
        vk_device->GetTimestampValidBits(EQueueType::Copy);
    const EVulkanTimestampQueryResetMode native_reset_mode =
        vk_device->GetTimestampQueryResetMode(EQueueType::Copy);
    const bool native_supported =
        vk_device->SupportsTimestampQueries(EQueueType::Copy);
    const char* native_reset_mode_name =
        native_reset_mode ==
                EVulkanTimestampQueryResetMode::CommandBuffer ?
            "command_buffer" :
        native_reset_mode == EVulkanTimestampQueryResetMode::Host ?
            "host" :
            "unsupported";
    const EBufferUsageFlags usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;
    BufferRef source = device.CreateBuffer<uint32>(
        "copy_timestamp_source", kElementCount, usage
    );
    BufferRef destination = device.CreateBuffer<uint32>(
        "copy_timestamp_destination", kElementCount, usage
    );

    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    size_t                          expected_native_submits = 0;

    auto run_copy_submission =
        [&](uint32 _seed, bool _expect_ready) -> QueryResult {
        std::array<uint32, kElementCount> values{};
        std::array<uint32, kElementCount> readback{};
        for (uint32 index = 0; index < values.size(); ++index) {
            values[index] = _seed + index * 37u;
        }

        std::atomic<uint32> callback_stage{0};
        std::atomic<uint32> completion_callbacks{0};
        std::atomic<uint32> query_callbacks{0};
        std::atomic<uint32> ordinary_callbacks{0};
        std::atomic<uint32> success_callbacks{0};
        std::atomic<uint32> callback_errors{0};
        FenceRef            done = device.CreateFence();

        auto signal_succeeded = [&] {
            auto* fence = static_cast<VulkanFence*>(
                ResourceCast(done.Get())
            );
            return fence != nullptr && done->GetValue() >= 1 &&
                   !fence->IsRejected(1) && !fence->IsFailed();
        };
        auto query_matches_expectation =
            [&](const QueryResult& _result) {
            const auto* timestamp =
                std::get_if<TimestampQueryResult>(&_result.payload);
            if (_expect_ready) {
                return _result.status == QueryStatus::Ready &&
                       _result.kind == QueryKind::Timestamp &&
                       _result.error_reason.empty() &&
                       timestamp != nullptr &&
                       timestamp->valid_bits ==
                           std::min(native_valid_bits, uint32{64}) &&
                       timestamp->valid_bits != 0 &&
                       timestamp->tick_period_ns > 0.0 &&
                       timestamp->duration_ns >= 0.0;
            }
            return _result.status == QueryStatus::Error &&
                   _result.kind == QueryKind::Timestamp &&
                   !_result.error_reason.empty() &&
                   timestamp == nullptr;
        };

        CommandList commands(EQueueType::Copy);
        GpuCompletionFuture completion =
            commands.TrackGpuCompletion(
                _expect_ready ?
                    "Phase21BCopyTimestampCompletion" :
                    "Phase21BCopyTimestampUnsupportedCompletion"
            );
        commands.CopyFrom(
            OwnedBytes(values),
            source->GetView(),
            "CopyTimestampUpload"
        );
        commands.PushScope(
            _expect_ready ?
                "Phase21BCopyTimestampScope" :
                "Phase21BCopyTimestampUnsupportedScope"
        );
        QueryToken query = commands.BeginTimestampQuery(
            _expect_ready ?
                "Phase21BCopyTimestamp" :
                "Phase21BCopyTimestampUnsupported"
        );
        QueryFuture future = query.GetFuture();
        commands.CopyFrom(
            source->GetView(),
            destination->GetView(),
            "CopyTimestampMeasuredCopy"
        );
        commands.EndTimestampQuery(query);
        commands.PopScope();
        commands.CopyFrom(
            destination->GetView(),
            WritableBytes(readback),
            "CopyTimestampReadback"
        );

        completion.Then([&](const GpuCompletionResult& _result) {
            const std::optional<QueryResult> query_result =
                future.TryGet();
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                _result.status != GpuCompletionStatus::Ready ||
                !signal_succeeded() ||
                !query_result.has_value() ||
                !query_matches_expectation(*query_result) ||
                callback_stage.load(std::memory_order_acquire) != 0) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            callback_stage.store(1, std::memory_order_release);
            completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        });
        future.Then([&](const QueryResult& _result) {
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                !query_matches_expectation(_result) ||
                completion.Status() != GpuCompletionStatus::Ready ||
                !signal_succeeded() ||
                callback_stage.load(std::memory_order_acquire) != 1) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            callback_stage.store(2, std::memory_order_release);
            query_callbacks.fetch_add(1, std::memory_order_release);
        });
        commands.AddCallback([&] {
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                callback_stage.load(std::memory_order_acquire) != 2) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            callback_stage.store(3, std::memory_order_release);
            ordinary_callbacks.fetch_add(
                1, std::memory_order_release
            );
        });
        commands.AddSuccessCallback([&] {
            if (GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                callback_stage.load(std::memory_order_acquire) != 3) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            callback_stage.store(4, std::memory_order_release);
            success_callbacks.fetch_add(
                1, std::memory_order_release
            );
        });

        CmdSubmit submit = commands.Submit();
        const size_t scope_command_count = std::count_if(
            submit.cmds.begin(),
            submit.cmds.end(),
            [](const UniquePtr<Command>& _command) {
                return _command &&
                       _command->Type() == Command::EType::Scope;
            }
        );
        if (scope_command_count != 2) {
            throw std::runtime_error(
                "Copy timestamp source lost its balanced visual scope"
            );
        }
        submit.Signal(done.Get(), 1);
        RHIExecutor::Get().Submit(
            EQueueType::Copy,
            std::move(submit),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        ++expected_native_submits;

        const GpuCompletionResult completion_result =
            completion.Get();
        const QueryResult result = future.Get();
        if (!query_matches_expectation(result) ||
            completion_result.status != GpuCompletionStatus::Ready ||
            !signal_succeeded() ||
            readback != values ||
            callback_stage.load(std::memory_order_acquire) != 4 ||
            completion_callbacks.load(std::memory_order_acquire) !=
                1 ||
            query_callbacks.load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks.load(std::memory_order_acquire) !=
                1 ||
            success_callbacks.load(std::memory_order_acquire) != 1 ||
            callback_errors.load(std::memory_order_acquire) != 0) {
            throw std::runtime_error(
                _expect_ready ?
                    "supported Copy timestamp submission violated its Completion contract" :
                    "unsupported Copy timestamp profiling disrupted real Copy work"
            );
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (completion_callbacks.load(std::memory_order_acquire) !=
                1 ||
            query_callbacks.load(std::memory_order_acquire) != 1 ||
            ordinary_callbacks.load(std::memory_order_acquire) !=
                1 ||
            success_callbacks.load(std::memory_order_acquire) != 1) {
            throw std::runtime_error(
                "Copy timestamp submission replayed Completion callbacks"
            );
        }
        return result;
    };

    std::optional<QueryResult> first_supported_result{};
    if (native_supported) {
        first_supported_result.emplace(
            run_copy_submission(0x21B10000u, true)
        );
    }

    ScriptedTimestampValidBitsCapture unsupported_capture{};
    QueryResult unsupported_result{};
    {
        ScopedScriptedTimestampValidBitsOverride
            scripted_override(unsupported_capture);
        unsupported_result =
            run_copy_submission(0x21B20000u, false);
    }
    if (unsupported_capture.count.load(std::memory_order_acquire) !=
            1 ||
        unsupported_capture.queue.load(std::memory_order_acquire) !=
            static_cast<uint32>(EQueueType::Copy) ||
        unsupported_capture.native_valid_bits.load(
            std::memory_order_acquire
        ) != native_valid_bits ||
        unsupported_capture.wrong_owner.load(
            std::memory_order_acquire
        ) ||
        unsupported_result.status != QueryStatus::Error) {
        throw std::runtime_error(
            "scripted Copy timestamp capability override lost ownership or identity"
        );
    }

    const QueryResult recovery_result =
        run_copy_submission(
            0x21B30000u,
            native_supported
        );

    if (native_capture.Overflowed() ||
        native_capture.Count() != expected_native_submits) {
        throw std::runtime_error(
            "Copy timestamp test observed the wrong native submission count"
        );
    }
    for (size_t index = 0; index < native_capture.Count(); ++index) {
        const VulkanNativeSubmissionEvent& event =
            native_capture.Event(index);
        if (event.queue != EQueueType::Copy ||
            event.thread_role != ERHIThreadRole::Submission ||
            !event.outcome.WasSubmitted()) {
            throw std::runtime_error(
                "Copy timestamp work did not retain native Submission ownership"
            );
        }
    }

    if (native_supported) {
        const auto* first_timestamp =
            std::get_if<TimestampQueryResult>(
                &first_supported_result->payload
            );
        const auto* recovery_timestamp =
            std::get_if<TimestampQueryResult>(
                &recovery_result.payload
            );
        if (first_timestamp == nullptr ||
            recovery_timestamp == nullptr ||
            first_timestamp->valid_bits == 0 ||
            recovery_timestamp->valid_bits == 0) {
            throw std::runtime_error(
                "Copy timestamp allocator reuse did not restore native capability"
            );
        }
        LOG_INFO(
            "[TESTCASE][PASS] name=TimestampQueryCopySupported "
            "queue=Copy status=Ready valid_bits={} reset_mode={} "
            "copy=verified scope=balanced "
            "gpu_completion=Ready order=signal->completion->query->ordinary->success "
            "owner=Completion callbacks=exactly_once "
            "allocator_reuse=verified native_owner=Submission replay=0",
            recovery_timestamp->valid_bits,
            native_reset_mode_name
        );
    } else {
        LOG_INFO(
            "[TESTCASE][SKIP] name=TimestampQueryCopySupported "
            "reason=timestamp_unsupported valid_bits={} "
            "reset_mode=unsupported",
            native_valid_bits
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=TimestampQueryCopyUnsupportedFallback "
        "queue=Copy injected_valid_bits=0 query=Error copy=verified "
        "scope=balanced "
        "signal=success gpu_completion=Ready native_submit=accepted "
        "native_owner=Submission owner=Completion "
        "callbacks=exactly_once runtime=recovered replay=0"
    );
}

void RunTimestampQueryPreparationRejection() {
    constexpr uint64 async_queue_scope = 0x5048313751505245ull;

    auto& device = RenderDevice::Get();

    FenceRef rejected_done = device.CreateFence();
    FenceRef sibling_done  = device.CreateFence();
    FenceRef recovered_done = device.CreateFence();
    auto* rejected_fence = static_cast<VulkanFence*>(
        ResourceCast(rejected_done.Get())
    );
    auto* sibling_fence = static_cast<VulkanFence*>(
        ResourceCast(sibling_done.Get())
    );
    auto* recovered_fence = static_cast<VulkanFence*>(
        ResourceCast(recovered_done.Get())
    );
    if (rejected_fence == nullptr || sibling_fence == nullptr ||
        recovered_fence == nullptr) {
        throw std::runtime_error(
            "timestamp Query preparation rejection could not access Vulkan fences"
        );
    }

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::atomic<uint32>                callback_errors{0};
    QueryFuture                        rejected_future{};
    QueryFuture                        sibling_future{};

    CommandList rejected(EQueueType::Graphics);
    rejected.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken rejected_query =
        rejected.BeginTimestampQuery("Phase17AQueryPrepareRejected");
    rejected_future = rejected_query.GetFuture();
    rejected.EndTimestampQuery(rejected_query);
    rejected.AddCallback([&] {
        const std::optional<QueryResult> sibling_result =
            sibling_future.TryGet();
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !sibling_result.has_value() ||
            sibling_result->status != QueryStatus::Error ||
            !sibling_fence->IsRejected(1) ||
            sibling_fence->IsFailed()) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    rejected.AddSuccessCallback([&] {
        success_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit rejected_submit = rejected.Submit();
    rejected_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    rejected_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    rejected_submit.async_queue_scope = async_queue_scope;
    rejected_submit.Signal(rejected_done.Get(), 1);

    CommandList sibling(EQueueType::Graphics);
    sibling.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken sibling_query =
        sibling.BeginTimestampQuery("Phase17AQueryPrepareSibling");
    sibling_future = sibling_query.GetFuture();
    sibling.EndTimestampQuery(sibling_query);
    sibling.AddCallback([&] {
        const std::optional<QueryResult> rejected_result =
            rejected_future.TryGet();
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !rejected_result.has_value() ||
            rejected_result->status != QueryStatus::Error ||
            !rejected_fence->IsRejected(1) ||
            rejected_fence->IsFailed()) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[1].fetch_add(1, std::memory_order_release);
    });
    sibling.AddSuccessCallback([&] {
        success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });
    CmdSubmit sibling_submit = sibling.Submit();
    sibling_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    sibling_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    sibling_submit.async_queue_scope = async_queue_scope;
    sibling_submit.Signal(sibling_done.Get(), 1);

    rejected_future.Then([&](const QueryResult& _result) {
        const std::optional<QueryResult> sibling_result =
            sibling_future.TryGet();
        if (_result.status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !sibling_result.has_value() ||
            sibling_result->status != QueryStatus::Error ||
            !sibling_fence->IsRejected(1) ||
            sibling_fence->IsFailed()) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    sibling_future.Then([&](const QueryResult& _result) {
        if (_result.status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[1].fetch_add(1, std::memory_order_release);
    });

    ScriptedQueryPreparationCapture scripted_capture{};
    NativeSubmissionCapture         native_capture{};
    ScopedNativeSubmissionObserver  native_observer(native_capture);
    {
        ScopedScriptedQueryPreparationOverride scripted_override(
            scripted_capture
        );
        Array<RHIBackendSubmissionBatchEntry> rejected_batch{};
        rejected_batch.emplace_back(
            EQueueType::Graphics, std::move(rejected_submit)
        );
        rejected_batch.emplace_back(
            EQueueType::Graphics, std::move(sibling_submit)
        );
        RHIExecutor::Get().Submit(
            std::move(rejected_batch),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }

    const QueryResult rejected_result = rejected_future.Get();
    const QueryResult sibling_result  = sibling_future.Get();
    const uint32 scripted_prepare_count =
        scripted_capture.count.load(std::memory_order_acquire);
    if (scripted_prepare_count == 0 || scripted_prepare_count > 2 ||
        scripted_capture.queue.load(std::memory_order_acquire) !=
            static_cast<uint32>(EQueueType::Graphics) ||
        scripted_capture.timeline.load(std::memory_order_acquire) == 0 ||
        scripted_capture.query_count.load(std::memory_order_acquire) != 2 ||
        scripted_capture.wrong_owner.load(std::memory_order_acquire) ||
        rejected_result.status != QueryStatus::Error ||
        sibling_result.status != QueryStatus::Error ||
        !rejected_fence->IsRejected(1) ||
        rejected_fence->IsFailed() ||
        !sibling_fence->IsRejected(1) ||
        sibling_fence->IsFailed() ||
        ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
        query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        success_callbacks[0].load(std::memory_order_acquire) != 0 ||
        success_callbacks[1].load(std::memory_order_acquire) != 0 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "recoverable timestamp Query preparation rejection violated "
            "pre-Completion suffix publication"
        );
    }

    std::atomic<uint32> recovered_callbacks{0};
    std::atomic<uint32> recovered_success_callbacks{0};
    CommandList recovered(EQueueType::Graphics);
    recovered.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    recovered.AddCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        recovered_callbacks.fetch_add(1, std::memory_order_release);
    });
    recovered.AddSuccessCallback([&] {
        recovered_success_callbacks.fetch_add(
            1, std::memory_order_release
        );
    });
    CmdSubmit recovered_submit = recovered.Submit();
    recovered_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    recovered_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    recovered_submit.async_queue_scope = async_queue_scope;
    recovered_submit.Signal(recovered_done.Get(), 1);
    Array<RHIBackendSubmissionBatchEntry> recovered_batch{};
    recovered_batch.emplace_back(
        EQueueType::Graphics, std::move(recovered_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(recovered_batch), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (recovered_done->GetValue() < 1 ||
        recovered_fence->IsRejected(1) ||
        recovered_fence->IsFailed() ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1 ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 1 ||
        !native_capture.Event(0).outcome.WasSubmitted() ||
        native_capture.Event(0).thread_role !=
            ERHIThreadRole::Submission) {
        throw std::runtime_error(
            "timestamp Query preparation rejection hard-latched the runtime"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
        query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        recovered_callbacks.load(std::memory_order_acquire) != 1 ||
        recovered_success_callbacks.load(std::memory_order_acquire) != 1 ||
        native_capture.Count() != 1) {
        throw std::runtime_error(
            "timestamp Query preparation rejection replayed Completion work"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=TimestampQueryPreparationRejection "
        "async_scope={} prepare_calls={} status=Error suffix_query=Error "
        "publish_before_completion=true signal=rejected-not-failed "
        "native_rejected_batch=0 recovery_submit=accepted "
        "owner=Completion callbacks=exactly_once replay=0",
        async_queue_scope,
        scripted_prepare_count
    );
}

void RunTimestampQueryPreflightRejection() {
    using namespace std::chrono_literals;

    constexpr uint64 async_queue_scope = 0x5048313751554552ull;

    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> callback_errors{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    QueryFuture                         future{};
    QueryFuture                         sibling_future{};

    CommandList commands(EQueueType::Graphics);
    // This callback intentionally precedes the Query fallback in recording
    // order. Preflight rejection must terminalize every Future before any
    // ordinary callback, so observing the result here can never self-block.
    commands.AddCallback([&] {
        const std::optional<QueryResult> result = future.TryGet();
        if (!result.has_value() ||
            result->status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors[0].fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[0].fetch_add(1, std::memory_order_release);
    });

    QueryToken query = commands.BeginTimestampQuery(
        "Phase17ARejectedMultiSegmentTimestamp"
    );
    future = query.GetFuture();
    future.Then([&](const QueryResult& _result) {
        const bool sibling_terminal = sibling_future.WaitFor(250ms);
        const std::optional<QueryResult> sibling_result =
            sibling_future.TryGet();
        if (_result.status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            !sibling_terminal ||
            !sibling_result.has_value() ||
            sibling_result->status != QueryStatus::Error) {
            callback_errors[0].fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[0].fetch_add(1, std::memory_order_release);
    });
    commands.EndTimestampQuery(query);
    commands.AddSuccessCallback([&] {
        success_callbacks[0].fetch_add(1, std::memory_order_relaxed);
    });

    CmdSubmit submit = commands.Submit();
    submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
    submit.async_queue_scope = async_queue_scope;
    submit.segments = {
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 1, 2},
    };

    // The sibling is valid in isolation. The malformed multi-segment query in
    // source zero rejects the whole native-runtime batch, so both query states
    // must be published before either source is allowed to notify callbacks.
    CommandList sibling_commands(EQueueType::Graphics);
    QueryToken sibling_query = sibling_commands.BeginTimestampQuery(
        "Phase17ARejectedBatchSiblingTimestamp"
    );
    sibling_future = sibling_query.GetFuture();
    sibling_future.Then([&](const QueryResult& _result) {
        if (_result.status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors[1].fetch_add(1, std::memory_order_relaxed);
        }
        query_callbacks[1].fetch_add(1, std::memory_order_release);
    });
    sibling_commands.EndTimestampQuery(sibling_query);
    sibling_commands.AddCallback([&] {
        const std::optional<QueryResult> first_result = future.TryGet();
        const std::optional<QueryResult> second_result =
            sibling_future.TryGet();
        if (!first_result.has_value() ||
            first_result->status != QueryStatus::Error ||
            !second_result.has_value() ||
            second_result->status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors[1].fetch_add(1, std::memory_order_relaxed);
        }
        ordinary_callbacks[1].fetch_add(1, std::memory_order_release);
    });
    sibling_commands.AddSuccessCallback([&] {
        success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
    });

    Array<RHIBackendSubmissionBatchEntry> rejected_batch{};
    rejected_batch.emplace_back(EQueueType::Graphics, std::move(submit));
    rejected_batch.emplace_back(
        EQueueType::Graphics,
        sibling_commands.Submit()
    );
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    RHIExecutor::Get().Submit(
        std::move(rejected_batch),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult result = future.Get();
    const QueryResult sibling_result = sibling_future.Get();
    if (result.status != QueryStatus::Error ||
        sibling_result.status != QueryStatus::Error ||
        result.error_reason.empty() ||
        sibling_result.error_reason.empty() ||
        ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
        query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        callback_errors[0].load(std::memory_order_acquire) != 0 ||
        callback_errors[1].load(std::memory_order_acquire) != 0 ||
        success_callbacks[0].load(std::memory_order_acquire) != 0 ||
        success_callbacks[1].load(std::memory_order_acquire) != 0 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "Vulkan query preflight rejection violated terminal ordering"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (ordinary_callbacks[0].load(std::memory_order_acquire) != 1 ||
        ordinary_callbacks[1].load(std::memory_order_acquire) != 1 ||
        query_callbacks[0].load(std::memory_order_acquire) != 1 ||
        query_callbacks[1].load(std::memory_order_acquire) != 1 ||
        success_callbacks[0].load(std::memory_order_acquire) != 0 ||
        success_callbacks[1].load(std::memory_order_acquire) != 0 ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "a second RHI Sync replayed rejected timestamp query callbacks"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=TimestampQueryPreflightRejection "
        "reason=multi-segment-query sources=2 status=Error owner=Completion "
        "batch_terminal_before_notify=true bounded_cross_future_wait=true "
        "ordinary_callback=exactly_once query_callback=exactly_once "
        "success_callback=0 native_submit=0 replay=0"
    );
}

void RunTimestampQueryMidBatchTranslateFailure() {
    using namespace std::chrono_literals;

    auto&            device            = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x50483137514D4944ull;

    FenceRef prefix_done = device.CreateFence();
    FenceRef suffix_done = device.CreateFence();
    std::binary_semaphore suffix_translate_entered{0};
    std::binary_semaphore release_suffix_translate{0};
    std::binary_semaphore prefix_query_callback_entered{0};
    OneShotSemaphoreRelease release_suffix_guard(
        release_suffix_translate
    );
    std::atomic<uint32>   prefix_query_callbacks{0};
    std::atomic<uint32>   suffix_query_callbacks{0};
    std::atomic<uint32>   prefix_completion_callbacks{0};
    std::atomic<uint32>   suffix_completion_callbacks{0};
    std::atomic<uint32>   prefix_ordinary_callbacks{0};
    std::atomic<uint32>   suffix_ordinary_callbacks{0};
    std::atomic<uint32>   prefix_success_callbacks{0};
    std::atomic<uint32>   suffix_success_callbacks{0};
    std::atomic<uint32>   callback_errors{0};

    CommandList prefix(EQueueType::Graphics);
    prefix.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    GpuCompletionFuture prefix_completion =
        prefix.TrackGpuCompletion(
            "Phase19AMidBatchPrefixCompletion"
        );
    QueryToken prefix_query =
        prefix.BeginTimestampQuery("Phase17AMidBatchPrefixTimestamp");
    QueryFuture prefix_future = prefix_query.GetFuture();
    prefix.EndTimestampQuery(prefix_query);
    prefix.AddCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        prefix_ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    prefix.AddSuccessCallback([&] {
        prefix_success_callbacks.fetch_add(1, std::memory_order_release);
    });
    CmdSubmit prefix_submit = prefix.Submit();
    prefix_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    prefix_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    prefix_submit.async_queue_scope = async_queue_scope;
    prefix_submit.Signal(prefix_done.Get(), 1);

    CommandList suffix(EQueueType::Graphics);
    suffix.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    GpuCompletionFuture suffix_completion =
        suffix.TrackGpuCompletion(
            "Phase19AMidBatchSuffixCompletion"
        );
    QueryToken suffix_query =
        suffix.BeginTimestampQuery("Phase17AMidBatchFailingTimestamp");
    QueryFuture suffix_future = suffix_query.GetFuture();
    suffix.AddCustomCommand(
        MakeUnique<BlockingTranslateProbeCommand>(
            EQueueType::Graphics,
            &suffix_translate_entered,
            &release_suffix_translate
        ),
        "Phase17AMidBatchBlockedSuffixTranslate"
    );
    suffix.AddCustomCommand(
        MakeUnique<ThrowingTranslateProbeCommand>(true),
        "Phase17AMidBatchThrowingSuffixTranslate"
    );
    suffix.EndTimestampQuery(suffix_query);
    suffix.AddCallback([&] {
        if (GetCurrentRHIThreadRole() != ERHIThreadRole::Completion) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        suffix_ordinary_callbacks.fetch_add(1, std::memory_order_release);
    });
    suffix.AddSuccessCallback([&] {
        suffix_success_callbacks.fetch_add(1, std::memory_order_release);
    });
    CmdSubmit suffix_submit = suffix.Submit();
    suffix_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    suffix_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    suffix_submit.async_queue_scope = async_queue_scope;
    suffix_submit.Signal(suffix_done.Get(), 1);

    // A successful-batch notification barrier deliberately prevents an
    // earlier callback from driving a later source's Translate. The test
    // thread releases the blocked suffix only after proving that no callback
    // escaped before every executable source reached a terminal packet.
    prefix_completion.Then(
        [&](const GpuCompletionResult& _result) {
            const auto suffix_result =
                suffix_completion.TryGet();
            if (_result.status !=
                    GpuCompletionStatus::Ready ||
                GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion ||
                !suffix_result.has_value() ||
                suffix_result->status !=
                    GpuCompletionStatus::Error) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            prefix_completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );
    suffix_completion.Then(
        [&](const GpuCompletionResult& _result) {
            if (_result.status !=
                    GpuCompletionStatus::Error ||
                GetCurrentRHIThreadRole() !=
                    ERHIThreadRole::Completion) {
                callback_errors.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            suffix_completion_callbacks.fetch_add(
                1, std::memory_order_release
            );
        }
    );
    prefix_future.Then([&](const QueryResult& _result) {
        prefix_query_callback_entered.release();
        const bool suffix_terminal = suffix_future.WaitFor(500ms);
        const std::optional<QueryResult> suffix_result =
            suffix_future.TryGet();
        auto* prefix_fence =
            static_cast<VulkanFence*>(ResourceCast(prefix_done.Get()));
        if (_result.status != QueryStatus::Ready ||
            GetCurrentRHIThreadRole() != ERHIThreadRole::Completion ||
            prefix_fence == nullptr ||
            prefix_done->GetValue() < 1 ||
            prefix_fence->IsRejected(1) ||
            prefix_fence->IsFailed() ||
            !suffix_terminal ||
            !suffix_result.has_value() ||
            suffix_result->status != QueryStatus::Error ||
            prefix_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1 ||
            suffix_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        prefix_query_callbacks.fetch_add(1, std::memory_order_release);
    });
    suffix_future.Then([&](const QueryResult& _result) {
        if (_result.status != QueryStatus::Error ||
            GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion ||
            prefix_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1 ||
            suffix_completion_callbacks.load(
                std::memory_order_acquire
            ) != 1) {
            callback_errors.fetch_add(1, std::memory_order_relaxed);
        }
        suffix_query_callbacks.fetch_add(1, std::memory_order_release);
    });

    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);
    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(EQueueType::Graphics, std::move(prefix_submit));
    batch.emplace_back(EQueueType::Graphics, std::move(suffix_submit));
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );
    if (!suffix_translate_entered.try_acquire_for(5s)) {
        throw std::runtime_error(
            "mid-batch timestamp suffix Translate did not enter"
        );
    }
    if (!prefix_future.WaitFor(5s)) {
        throw std::runtime_error(
            "mid-batch timestamp prefix did not publish before suffix release"
        );
    }
    const std::optional<QueryResult> published_prefix =
        prefix_future.TryGet();
    const bool prefix_completion_published =
        prefix_completion.WaitFor(5s);
    const auto published_prefix_completion =
        prefix_completion.TryGet();
    if (!published_prefix.has_value() ||
        published_prefix->status != QueryStatus::Ready ||
        !prefix_completion_published ||
        !published_prefix_completion.has_value() ||
        published_prefix_completion->status !=
            GpuCompletionStatus::Ready ||
        suffix_completion.TryGet().has_value()) {
        throw std::runtime_error(
            "mid-batch prefix completion did not preserve the terminal frontier"
        );
    }
    const bool prefix_callback_entered_before_terminal =
        prefix_query_callback_entered.try_acquire_for(100ms);
    if (prefix_callback_entered_before_terminal) {
        callback_errors.fetch_add(1, std::memory_order_relaxed);
    }
    if (prefix_query_callbacks.load(std::memory_order_acquire) != 0 ||
        suffix_query_callbacks.load(std::memory_order_acquire) != 0 ||
        prefix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 0 ||
        suffix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 0 ||
        prefix_ordinary_callbacks.load(std::memory_order_acquire) != 0 ||
        suffix_ordinary_callbacks.load(std::memory_order_acquire) != 0) {
        throw std::runtime_error(
            "mid-batch timestamp callback escaped before suffix terminalization"
        );
    }
    release_suffix_guard.Release();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult prefix_result = prefix_future.Get();
    const QueryResult suffix_result = suffix_future.Get();
    const GpuCompletionResult prefix_completion_result =
        prefix_completion.Get();
    const GpuCompletionResult suffix_completion_result =
        suffix_completion.Get();
    auto* suffix_fence =
        static_cast<VulkanFence*>(ResourceCast(suffix_done.Get()));
    if (prefix_result.status != QueryStatus::Ready ||
        suffix_result.status != QueryStatus::Error ||
        prefix_completion_result.status !=
            GpuCompletionStatus::Ready ||
        suffix_completion_result.status !=
            GpuCompletionStatus::Error ||
        suffix_completion_result.error_reason.empty() ||
        suffix_result.error_reason.empty() ||
        prefix_query_callbacks.load(std::memory_order_acquire) != 1 ||
        suffix_query_callbacks.load(std::memory_order_acquire) != 1 ||
        prefix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        suffix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        prefix_ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        suffix_ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        prefix_success_callbacks.load(std::memory_order_acquire) != 1 ||
        suffix_success_callbacks.load(std::memory_order_acquire) != 0 ||
        (!prefix_callback_entered_before_terminal &&
         !prefix_query_callback_entered.try_acquire()) ||
        callback_errors.load(std::memory_order_acquire) != 0 ||
        suffix_fence == nullptr ||
        (!suffix_fence->IsRejected(1) && !suffix_fence->IsFailed()) ||
        native_capture.Overflowed() ||
        native_capture.Count() != 1) {
        throw std::runtime_error(
            "mid-batch timestamp Query rejection was not published before same-queue Completion notification"
        );
    }

    const VulkanNativeSubmissionEvent& native_event =
        native_capture.Event(0);
    if (native_event.queue != EQueueType::Graphics ||
        native_event.thread_role != ERHIThreadRole::Submission ||
        !native_event.outcome.WasSubmitted()) {
        throw std::runtime_error(
            "mid-batch timestamp Query test did not submit exactly its prefix source"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (prefix_query_callbacks.load(std::memory_order_acquire) != 1 ||
        suffix_query_callbacks.load(std::memory_order_acquire) != 1 ||
        prefix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        suffix_completion_callbacks.load(
            std::memory_order_acquire
        ) != 1 ||
        prefix_ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        suffix_ordinary_callbacks.load(std::memory_order_acquire) != 1 ||
        native_capture.Count() != 1) {
        throw std::runtime_error(
            "mid-batch timestamp Query failure replayed Completion callbacks"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=TimestampQueryMidBatchTranslateFailure "
        "queues=Graphics,Graphics native_prefix_submit=1 "
        "suffix_translate=main-thread-released suffix_status=Error "
        "prefix_completion=Ready suffix_completion=Error "
        "pre_terminal_callback_entry=0 "
        "batch_terminal_before_notify=true "
        "bounded_cross_future_wait=true "
        "owner=Completion callbacks=exactly_once replay=0"
    );
}

void RunTimestampQuerySerialRecordFailure() {
    auto&            device            = RenderDevice::Get();
    constexpr uint64 async_queue_scope = 0x5048313751535246ull;

    FenceRef failing_done = device.CreateFence();
    FenceRef sibling_done = device.CreateFence();
    QueryFuture failing_future{};
    QueryFuture sibling_future{};
    std::array<std::atomic<uint32>, 2> query_callbacks{};
    std::array<std::atomic<uint32>, 2> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 2> success_callbacks{};
    std::atomic<uint32>                callback_errors{0};

    SerialQueryRecordFailureTranslationGate translation_gate(
        async_queue_scope
    );
    SourceSubmissionCapture source_capture(async_queue_scope);
    ScopedSourceSubmissionObserver source_observer(source_capture);
    NativeSubmissionCapture        native_capture{};
    ScopedNativeSubmissionObserver native_observer(native_capture);

    auto fence_failed =
        [](const FenceRef& _fence) {
            auto* fence = static_cast<VulkanFence*>(
                ResourceCast(_fence.Get())
            );
            return fence != nullptr && fence->IsFailed() &&
                   !fence->IsRejected(1);
        };
    auto query_failed =
        [](const QueryFuture& _future) {
            const std::optional<QueryResult> result =
                _future.TryGet();
            return result.has_value() &&
                   result->status == QueryStatus::Error &&
                   !result->error_reason.empty();
        };
    auto sibling_terminal =
        [&] {
            return query_failed(sibling_future) &&
                   fence_failed(sibling_done);
        };

    CommandList failing(EQueueType::Graphics);
    failing.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken failing_query =
        failing.BeginTimestampQuery(
            "Phase17ASerialRecordFailingTimestamp"
        );
    failing_future = failing_query.GetFuture();
    failing_future.Then([&](const QueryResult& _result) {
        // Take the sibling snapshot before doing any other callback work.
        // TryGet and the fence predicates are non-blocking: this proves the
        // entire suffix was already terminal when notification began.
        const bool sibling_was_terminal = sibling_terminal();
        const bool completion_owned =
            GetCurrentRHIThreadRole() ==
            ERHIThreadRole::Completion;
        if (!sibling_was_terminal || !completion_owned ||
            _result.status != QueryStatus::Error ||
            _result.error_reason.empty()) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
        translation_gate.NotifyFailureCallbackEntryChecked();
    });
    failing.AddCustomCommand(
        MakeUnique<ThrowingTranslateProbeCommand>(true),
        "Phase17ASerialQueryNativeRecordFailure"
    );
    failing.EndTimestampQuery(failing_query);
    failing.AddCallback([&] {
        const bool sibling_was_terminal = sibling_terminal();
        const bool failing_was_terminal =
            query_failed(failing_future);
        const bool completion_owned =
            GetCurrentRHIThreadRole() ==
            ERHIThreadRole::Completion;
        if (!sibling_was_terminal || !failing_was_terminal ||
            !completion_owned) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
        translation_gate.NotifyFailureCallbackEntryChecked();
    });
    failing.AddSuccessCallback([&] {
        success_callbacks[0].fetch_add(
            1, std::memory_order_release
        );
    });
    CmdSubmit failing_submit = failing.Submit();
    failing_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    failing_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    failing_submit.async_queue_scope = async_queue_scope;
    failing_submit.Signal(failing_done.Get(), 1);

    CommandList sibling(EQueueType::Graphics);
    sibling.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    QueryToken sibling_query =
        sibling.BeginTimestampQuery(
            "Phase17ASerialRecordFailureSiblingTimestamp"
        );
    sibling_future = sibling_query.GetFuture();
    sibling_future.Then([&](const QueryResult& _result) {
        const bool both_fences_terminal =
            fence_failed(failing_done) &&
            fence_failed(sibling_done);
        if (_result.status != QueryStatus::Error ||
            _result.error_reason.empty() ||
            !both_fences_terminal ||
            GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        query_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    sibling.EndTimestampQuery(sibling_query);
    sibling.AddCallback([&] {
        const bool both_queries_terminal =
            query_failed(failing_future) &&
            query_failed(sibling_future);
        const bool both_fences_terminal =
            fence_failed(failing_done) &&
            fence_failed(sibling_done);
        if (!both_queries_terminal || !both_fences_terminal ||
            GetCurrentRHIThreadRole() !=
                ERHIThreadRole::Completion) {
            callback_errors.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        ordinary_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    sibling.AddSuccessCallback([&] {
        success_callbacks[1].fetch_add(
            1, std::memory_order_release
        );
    });
    CmdSubmit sibling_submit = sibling.Submit();
    sibling_submit.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::Parallel
    );
    sibling_submit.SetResourceStateOwnership(
        ERHIResourceStateOwnership::Explicit
    );
    sibling_submit.async_queue_scope = async_queue_scope;
    sibling_submit.Signal(sibling_done.Get(), 1);

    Array<RHIBackendSubmissionBatchEntry> batch{};
    batch.emplace_back(
        EQueueType::Graphics, std::move(failing_submit)
    );
    batch.emplace_back(
        EQueueType::Graphics, std::move(sibling_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(batch), ERHIExecSubmitFlags::FlushGPU
    );

    const bool terminal_phase_observed =
        translation_gate.WaitForTerminalPhase(
            std::chrono::milliseconds(2000)
        );
    const bool callback_entered_before_release =
        terminal_phase_observed &&
        translation_gate.FailureCallbackEnteredBeforeRelease(
            std::chrono::milliseconds(500)
        );
    const bool callbacks_zero_before_release =
        query_callbacks[0].load(std::memory_order_acquire) ==
            0 &&
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) == 0;
    // Always release the observer, including the timeout path, so a late
    // Translate notification cannot strand shutdown.
    translation_gate.ReleaseTerminalPhase();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const QueryResult failing_result = failing_future.Get();
    const QueryResult sibling_result = sibling_future.Get();
    if (failing_result.status != QueryStatus::Error ||
        sibling_result.status != QueryStatus::Error ||
        failing_result.error_reason.empty() ||
        sibling_result.error_reason.empty() ||
        !fence_failed(failing_done) ||
        !fence_failed(sibling_done) ||
        query_callbacks[0].load(std::memory_order_acquire) !=
            1 ||
        query_callbacks[1].load(std::memory_order_acquire) !=
            1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 0 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 0 ||
        callback_errors.load(std::memory_order_acquire) !=
            0 ||
        !terminal_phase_observed ||
        callback_entered_before_release ||
        !callbacks_zero_before_release ||
        translation_gate.TimedOut() ||
        translation_gate.CheckedCallbackEntryCount() != 2 ||
        translation_gate.RecordedPhaseCount() +
                translation_gate.FailurePhaseCount() !=
            1 ||
        source_capture.Overflowed() ||
        source_capture.Count() != 0 ||
        native_capture.Overflowed() ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "serial timestamp Query native-record failure notified "
            "Completion before publishing the whole batch suffix"
        );
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (query_callbacks[0].load(std::memory_order_acquire) !=
            1 ||
        query_callbacks[1].load(std::memory_order_acquire) !=
            1 ||
        ordinary_callbacks[0].load(
            std::memory_order_acquire
        ) != 1 ||
        ordinary_callbacks[1].load(
            std::memory_order_acquire
        ) != 1 ||
        success_callbacks[0].load(
            std::memory_order_acquire
        ) != 0 ||
        success_callbacks[1].load(
            std::memory_order_acquire
        ) != 0 ||
        source_capture.Count() != 0 ||
        native_capture.Count() != 0) {
        throw std::runtime_error(
            "serial timestamp Query native-record failure replayed "
            "Completion or native submission"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] "
        "name=TimestampQuerySerialRecordFailure "
        "sources=2 failing_source=0 serial_query_island=true "
        "sibling_query=Error sibling_signal=failed "
        "batch_terminal_before_notify=true "
        "recorded_phase_gate_count={} failed_phase_gate_count={} "
        "pre_release_callback=0 owner=Completion "
        "callbacks=exactly_once native_submit=0 replay=0",
        translation_gate.RecordedPhaseCount(),
        translation_gate.FailurePhaseCount()
    );
}

void RunOwningReadbackFuture() {
    auto& device = RenderDevice::Get();
    constexpr size_t buffer_element_count = 16;
    constexpr size_t readback_first_element = 4;
    constexpr size_t readback_element_count = 8;
    constexpr uint32 texture_width = 8;
    constexpr uint32 texture_height = 8;
    constexpr uint32 texture_mip = 1;
    constexpr uint32 texture_layer = 1;

    const EBufferUsageFlags buffer_usage =
        EBufferUsageFlags::TRANSFER_SRC |
        EBufferUsageFlags::TRANSFER_DST;
    BufferRef buffer = device.CreateBuffer<uint32>(
        "owning_readback_buffer",
        buffer_element_count,
        buffer_usage
    );
    BufferRef texture_copy_buffer = device.CreateBuffer<uint32>(
        "owning_readback_texture_copy_buffer",
        (texture_width >> texture_mip) *
            (texture_height >> texture_mip),
        buffer_usage
    );
    BufferRef sibling_copy_buffer = device.CreateBuffer<uint32>(
        "owning_readback_sibling_copy_buffer",
        texture_width * texture_height,
        buffer_usage
    );
    TextureRef texture = device.CreateTexture(
        "owning_readback_texture",
        Extent3D(texture_width, texture_height, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST,
        2,
        2
    );
    TextureRef layer_copy_texture = device.CreateTexture(
        "owning_readback_layer_copy_texture",
        Extent3D(texture_width, texture_height, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST,
        2,
        2
    );
    TextureRef compressed_texture = device.CreateTexture(
        "owning_readback_unsupported_bc1",
        Extent3D(texture_width, texture_height, 1),
        PF_BC1_RGBA_UNORM_BLOCK,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
    const TextureInfo msaa_info(
        ETextureDimension::TEX_2D,
        ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST,
        PF_R8G8B8A8_UNORM,
        EClearAttachment::COLOR,
        Extent3D(texture_width, texture_height, 1),
        1,
        1,
        4
    );
    TextureRef msaa_texture = device.CreateTexture(
        "owning_readback_unsupported_msaa", msaa_info
    );

    TextureView offset_view = texture->GetView(0, 1).Slice(0, 1);
    offset_view.offset.x = 1;
    TextureView partial_view = texture->GetView(0, 1).Slice(0, 1);
    partial_view.extent.x = texture_width / 2;
    TextureView invalid_mip_view =
        texture->GetView(0, 1).Slice(0, 1);
    invalid_mip_view.mip_level = 255;
    CommandList unsupported_commands(EQueueType::Copy);
    const ReadbackFuture compressed_readback =
        unsupported_commands.Readback(
            compressed_texture->GetView(0, 1).Slice(0, 1),
            "OwningReadbackRejectCompressed"
        );
    const ReadbackFuture msaa_readback =
        unsupported_commands.Readback(
            msaa_texture->GetView(0, 1).Slice(0, 1),
            "OwningReadbackRejectMsaa"
        );
    const ReadbackFuture offset_readback =
        unsupported_commands.Readback(
            offset_view, "OwningReadbackRejectOffset"
        );
    const ReadbackFuture partial_readback =
        unsupported_commands.Readback(
            partial_view, "OwningReadbackRejectPartial"
        );
    const ReadbackFuture invalid_mip_readback =
        unsupported_commands.Readback(
            invalid_mip_view, "OwningReadbackRejectInvalidMip"
        );
    CmdSubmit unsupported_submit = unsupported_commands.Submit();
    if (compressed_readback.Valid() || msaa_readback.Valid() ||
        offset_readback.Valid() || partial_readback.Valid() ||
        invalid_mip_readback.Valid() ||
        !unsupported_submit.cmds.empty() ||
        !unsupported_submit.gpu_completion_tokens.empty()) {
        throw std::runtime_error(
            "owning texture readback admitted an unsupported compressed, "
            "multisampled, or partial subresource"
        );
    }

    std::array<uint32, buffer_element_count> buffer_values{};
    for (uint32 index = 0; index < buffer_values.size(); ++index) {
        buffer_values[index] =
            0x19B00000u + index * 37u + 5u;
    }
    constexpr size_t texture_mip_width =
        texture_width >> texture_mip;
    constexpr size_t texture_mip_height =
        texture_height >> texture_mip;
    std::array<uint32, texture_mip_width * texture_mip_height>
        texture_values{};
    for (uint32 index = 0; index < texture_values.size(); ++index) {
        texture_values[index] =
            0xFF000000u | (index * 0x00070311u);
    }
    std::array<uint32, texture_width * texture_height>
        sibling_values{};
    for (uint32 index = 0; index < sibling_values.size(); ++index) {
        sibling_values[index] =
            0x19B10000u + index * 53u + 9u;
    }

    TextureView texture_view =
        texture->GetView(texture_mip, 1).Slice(texture_layer, 1);
    TextureView texture_upload_view = texture_view;
    texture_upload_view.extent = uint3(
        texture_mip_width, texture_mip_height, 1
    );
    TextureView layer_copy_view =
        layer_copy_texture->GetView(texture_mip, 1).Slice(
            texture_layer, 1
        );
    layer_copy_view.extent = uint3(
        texture_mip_width, texture_mip_height, 1
    );
    TextureView sibling_view =
        layer_copy_texture->GetView(0, 1).Slice(0, 1);
    sibling_view.extent = uint3(
        texture_width, texture_height, 1
    );
    BufferView buffer_view = buffer->GetView(
        readback_first_element * sizeof(uint32),
        readback_element_count * sizeof(uint32)
    );

    // Keep the nonzero-layer texture-to-buffer copy in its own submission.
    // A later texture readback must not be able to repair a missing transition
    // for the copied layer and accidentally make this regression pass.
    CommandList layer_copy_commands(EQueueType::Copy);
    layer_copy_commands.CopyFrom(
        OwnedBytes(texture_values),
        layer_copy_view,
        "OwningReadbackTextureLayerUpload"
    );
    layer_copy_commands.CopyFrom(
        layer_copy_view,
        texture_copy_buffer->GetView(),
        "OwningReadbackTextureLayerCopy"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        layer_copy_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );

    CommandList layer_copy_readback_commands(EQueueType::Copy);
    ReadbackFuture texture_copy_future =
        layer_copy_readback_commands.Readback(
            texture_copy_buffer->GetView(),
            "OwningReadbackTextureLayerCopyBuffer"
        );
    if (!texture_copy_future.Valid() ||
        texture_copy_future.ExpectedByteSize() !=
            texture_values.size() * sizeof(uint32)) {
        throw std::runtime_error(
            "nonzero-layer texture copy did not create a valid readback"
        );
    }
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        layer_copy_readback_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    const ReadbackResult texture_copy_result =
        texture_copy_future.Get();
    const auto readback_texture_copy_values =
        texture_copy_result.CopyAs<uint32>();
    const Array<uint32> expected_texture_values(
        texture_values.begin(), texture_values.end()
    );
    if (texture_copy_result.status != ReadbackStatus::Ready ||
        !readback_texture_copy_values.has_value() ||
        *readback_texture_copy_values != expected_texture_values) {
        throw std::runtime_error(
            "isolated nonzero-layer texture-to-buffer readback mismatch"
        );
    }

    // The first partial restore publishes a whole-image preferred-state
    // promise. Exercise an untouched sibling in the next submission so that
    // the promise is valid only if the first restore initialized its
    // complementary subresources as well.
    CommandList sibling_commands(EQueueType::Copy);
    sibling_commands.CopyFrom(
        OwnedBytes(sibling_values),
        sibling_view,
        "OwningReadbackSiblingUpload"
    );
    sibling_commands.CopyFrom(
        sibling_view,
        sibling_copy_buffer->GetView(),
        "OwningReadbackSiblingCopy"
    );
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        sibling_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );

    CommandList sibling_readback_commands(EQueueType::Copy);
    ReadbackFuture sibling_future =
        sibling_readback_commands.Readback(
            sibling_copy_buffer->GetView(),
            "OwningReadbackSiblingCopyBuffer"
        );
    if (!sibling_future.Valid() ||
        sibling_future.ExpectedByteSize() !=
            sibling_values.size() * sizeof(uint32)) {
        throw std::runtime_error(
            "sibling subresource copy did not create a valid readback"
        );
    }
    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        sibling_readback_commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );
    const ReadbackResult sibling_result = sibling_future.Get();
    const auto readback_sibling_values =
        sibling_result.CopyAs<uint32>();
    const Array<uint32> expected_sibling_values(
        sibling_values.begin(), sibling_values.end()
    );
    if (sibling_result.status != ReadbackStatus::Ready ||
        !readback_sibling_values.has_value() ||
        *readback_sibling_values != expected_sibling_values) {
        throw std::runtime_error(
            "next-submit sibling subresource readback mismatch"
        );
    }

    CommandList commands(EQueueType::Copy);
    commands.CopyFrom(
        OwnedBytes(buffer_values),
        buffer->GetView(),
        "OwningReadbackBufferUpload"
    );
    commands.CopyFrom(
        OwnedBytes(texture_values),
        texture_upload_view,
        "OwningReadbackTextureUpload"
    );
    ReadbackFuture buffer_future = commands.Readback(
        buffer_view, "OwningReadbackBuffer"
    );
    ReadbackFuture texture_future = commands.Readback(
        texture_view, "OwningReadbackTexture"
    );
    if (!buffer_future.Valid() || !texture_future.Valid() ||
        buffer_future.ExpectedByteSize() !=
            readback_element_count * sizeof(uint32) ||
        texture_future.ExpectedByteSize() !=
            texture_values.size() * sizeof(uint32)) {
        throw std::runtime_error(
            "owning readback did not preserve logical byte sizes"
        );
    }

    std::atomic<uint32> callback_count{0};
    std::atomic_bool callbacks_observed_siblings_terminal{true};
    buffer_future.Then([&](const ReadbackResult& _result) {
        if (_result.status != ReadbackStatus::Ready ||
            texture_future.Status() == ReadbackStatus::Pending) {
            callbacks_observed_siblings_terminal.store(
                false, std::memory_order_release
            );
        }
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    texture_future.Then([&](const ReadbackResult& _result) {
        if (_result.status != ReadbackStatus::Ready ||
            buffer_future.Status() == ReadbackStatus::Pending) {
            callbacks_observed_siblings_terminal.store(
                false, std::memory_order_release
            );
        }
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    RHIExecutor::Get().Submit(
        EQueueType::Copy,
        commands.Submit(),
        ERHIExecSubmitFlags::FlushGPU
    );

    const ReadbackResult buffer_result = buffer_future.Get();
    const ReadbackResult texture_result = texture_future.Get();
    const auto readback_buffer_values =
        buffer_result.CopyAs<uint32>();
    const auto readback_texture_values =
        texture_result.CopyAs<uint32>();
    if (buffer_result.status != ReadbackStatus::Ready ||
        texture_result.status != ReadbackStatus::Ready ||
        !readback_buffer_values.has_value() ||
        !readback_texture_values.has_value()) {
        throw std::runtime_error(
            "owning readback Future did not resolve Ready with typed data"
        );
    }
    if (!std::equal(
            readback_buffer_values->begin(),
            readback_buffer_values->end(),
            buffer_values.begin() + readback_first_element
        )) {
        LOG_ERROR(
            "owning buffer subrange mismatch actual_first={} "
            "expected_first={} actual_last={} expected_last={}",
            readback_buffer_values->front(),
            buffer_values[readback_first_element],
            readback_buffer_values->back(),
            buffer_values[
                readback_first_element +
                readback_element_count - 1
            ]
        );
        throw std::runtime_error(
            "owning buffer subrange readback mismatch"
        );
    }
    if (*readback_texture_values != expected_texture_values) {
        throw std::runtime_error(
            "owning mip/layer texture readback mismatch"
        );
    }
    // Get is released by terminal publication; callback notification is a
    // deliberately later phase. Data acquisition above therefore does not
    // require a global Sync, while this Sync makes callback accounting
    // deterministic for the test.
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (callback_count.load(std::memory_order_acquire) != 2 ||
        !callbacks_observed_siblings_terminal.load(
            std::memory_order_acquire
        )) {
        throw std::runtime_error(
            "owning readback callbacks did not observe terminal siblings"
        );
    }

    LOG_INFO(
        "[TESTCASE][PASS] name=OwningReadbackFuture "
        "queue=Copy buffer=subrange texture=mip1,layer1 "
        "layer_copy=isolated_nonzero_layer "
        "sibling=next_submit_mip0_layer0 "
        "state_restore=whole_image bytes={},{},{},{} "
        "callbacks=exactly_once siblings=terminal "
        "native_staging=completion-owned host_snapshot=future-owned "
        "unsupported=compressed,msaa,partial",
        buffer_result.ByteSize(),
        texture_result.ByteSize(),
        texture_copy_result.ByteSize(),
        sibling_result.ByteSize()
    );
}

} // namespace

int main(int argc, const char** argv) {
    bool task_system_initialized = false;
    bool render_device_initialized = false;
    try {
        ValidateArguments(argc, argv);
        const bool parallel = HasArgument(argc, argv, "--parallel");
        const bool inject_worker_failure =
            HasArgument(argc, argv, "--inject-worker-failure");
        const bool production_heavy = HasArgument(argc, argv, "--production-heavy");
        const bool production_gate =
            HasArgument(argc, argv, "--production-gate") || production_heavy;
        const bool inject_translate_failure =
            HasArgument(argc, argv, "--inject-translate-failure");
        const bool inject_multi_segment_translate_failure =
            HasArgument(argc, argv, "--inject-multi-segment-translate-failure");
        const bool pipeline_window1 =
            HasArgument(argc, argv, "--pipeline-window1");
        const bool pipeline_window2 =
            HasArgument(argc, argv, "--pipeline-window2");
        const bool pipeline_window_mode =
            pipeline_window1 || pipeline_window2;
        const bool present_boundary =
            HasArgument(argc, argv, "--present-boundary");
        const bool present_hard =
            HasArgument(argc, argv, "--present-hard");
        const bool present_mode =
            present_boundary || present_hard;
        const bool rt_export_rejection =
            HasArgument(argc, argv, "--rt-export-rejection");
        const bool readback_future_mode =
            HasArgument(argc, argv, "--readback-future");
        const bool occlusion_query_mode =
            HasArgument(argc, argv, "--occlusion-query");
        const bool timestamp_query_mode =
            HasArgument(argc, argv, "--timestamp-query");
        const bool timestamp_query_success_batch_mode =
            HasArgument(
                argc, argv, "--timestamp-query-success-batch"
            );
        const bool timestamp_query_mid_failure_mode =
            HasArgument(argc, argv, "--timestamp-query-mid-failure");
        const bool timestamp_query_record_failure_mode =
            HasArgument(
                argc, argv, "--timestamp-query-record-failure"
            );
        const bool gpu_scope_stream_mode =
            HasArgument(argc, argv, "--gpu-scope-stream");
        const uint32 submission_batch_window =
            pipeline_window1 ? 1u : 2u;

        LogSystem::Init();
        ConfigManager::GetInstance().Init(std::filesystem::current_path());
        TaskSystem::Init();
        task_system_initialized = true;

        RenderDevice::Init(DeviceInitInfo{
            .rhi_type         = ERHIType::Vulkan,
            .name             = "RHIParallelRecordVulkanTest",
            .rhi_api_version  = "1.3",
            .rhi_thread       = true,
            .rhi_bypass       = false,
            .parallel_recording =
                parallel || pipeline_window_mode || present_mode ||
                readback_future_mode ||
                occlusion_query_mode ||
                timestamp_query_mode ||
                timestamp_query_success_batch_mode ||
                timestamp_query_mid_failure_mode ||
                timestamp_query_record_failure_mode ||
                gpu_scope_stream_mode,
            .parallel_record_workers = 4,
            .parallel_record_verify =
                parallel || pipeline_window_mode || present_mode ||
                readback_future_mode ||
                occlusion_query_mode ||
                timestamp_query_mode ||
                timestamp_query_success_batch_mode ||
                timestamp_query_mid_failure_mode ||
                timestamp_query_record_failure_mode ||
                gpu_scope_stream_mode,
            .parallel_record_min_work_units_per_job = production_gate ? 64u : 1u,
            .submission_batch_window = submission_batch_window,
            .parallel_record_worker_throw_trigger = inject_worker_failure ? 1u : 0u,
        });
        render_device_initialized = true;

        if (readback_future_mode) {
            RunOwningReadbackFuture();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (occlusion_query_mode) {
            RunOcclusionQueryCompletionOwnership();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (gpu_scope_stream_mode) {
            RunGpuScopeStreamCompletionAndParallelIsolation();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (timestamp_query_success_batch_mode) {
            RunTimestampQuerySuccessfulBatchPublication();
            RunTimestampQueryCrossQueueBatchPublication();
            RunTimestampQueryMixedMultiSegmentCallbackTiers();
            RunTimestampQueryCopyOnlyPayloadArrival();
            RunCopyTimestampQueryCapabilityAndIsolation();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }
        if (timestamp_query_record_failure_mode) {
            RunTimestampQuerySerialRecordFailure();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }
        if (timestamp_query_mid_failure_mode) {
            RunTimestampQueryMidBatchTranslateFailure();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }
        if (timestamp_query_mode) {
            RunTimestampQueryCompletionOwnership();
            RunTimestampQuerySuccessfulBatchPublication();
            RunTimestampQueryCrossQueueBatchPublication();
            RunTimestampQueryMixedMultiSegmentCallbackTiers();
            RunTimestampQueryCopyOnlyPayloadArrival();
            RunCopyTimestampQueryCapabilityAndIsolation();
            RunTimestampQueryPreparationRejection();
            // This malformed-batch rejection hard-latches the runtime, so the
            // mid-batch Translate failure has its own focused invocation.
            RunTimestampQueryPreflightRejection();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (present_mode) {
            if (present_boundary) {
                RunPresentSourceContractRejection();
                RunSerialControlPipelineBoundary();
                RunPresentPipelineBoundary();
                // This is the terminal focused lifecycle test: it stops and
                // joins the RHI runtime before returning.
                RunQueuedPresentShutdownBoundary();
            } else {
                RunPresentHardFailureBoundary();
            }
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (rt_export_rejection) {
            RunRaytracingExportAcceptanceRejection();
            RunRaytracingAcceptedReadbackMaterializationFailure();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        if (pipeline_window_mode) {
            RunBoundedCrossBatchSubmissionPipeline(
                submission_batch_window
            );
            if (pipeline_window2) {
                RunBoundedCrossBatchMalformedPreflightOrdering();
                RunBoundedCrossBatchRecoverableRejection();
                // This stops the RHI runtime and must remain the final focused
                // lifecycle test before RenderDevice::Dispose().
                RunBoundedPipelineShutdownCancellation();
            }
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        RunMultiSegmentCompletionAggregateCpuProbe();

        if (inject_translate_failure) {
            RunParallelTranslateFailureRetirement();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }
        if (inject_multi_segment_translate_failure) {
            RunMultiSegmentPrefixSubmitSuffixTranslateFailure();
            RenderDevice::Dispose();
            render_device_initialized = false;
            TaskSystem::ShutDown();
            task_system_initialized = false;
            return 0;
        }

        RunOrderedReadback(
            parallel, inject_worker_failure, production_gate, production_heavy
        );
        RunActiveRdgExplicitBarrierReadback(parallel);
        RunActiveRdgAsyncQueueDag(parallel);
        RunActiveRdgGraphicsCopyRoundTrip(parallel);
        RunActiveRdgTransientAliasReadback(parallel);
        RunActiveRdgTransientTextureAliasReadback(parallel);
        RunTransientDepthStencilAspectAllocation();
        RunActiveRdgTextureArraySubrange(parallel);
        RunExplicitTextureArrayRangeShapeChange(parallel);
        RunUpperTopologyBatch();
        RunPendingSourceTopologyBatch();
        RunContinuousFrameInFlightRetirement();
        RunRecoverableCopyDependencyRejection();
        RunCrossQueueTopologyBatch();
        RunDirectCopyExecuteRejected();
        RunVulkanStorageUnifiedCopySubmission();
        RunAsyncQueueParallelTranslateSmoke();
        RunMultiSegmentSourceExecution();
        RunMultiSegmentCopyRoundTrip();
        RunMultiSegmentRecoverableRejection();
        RunParallelTranslateRecoverableRejection();
        RunRuntimeRejectCompletionOwnership();
        PrimeGlobalTransientPoolForDispose();
        // This explicitly stops the runtime and must remain the final test
        // before device disposal.
        RunShutdownDependencyCancellation();

        RenderDevice::Dispose();
        if (RenderGraphResourcePool::Global().BufferCount() != 0 ||
            RenderGraphResourcePool::Global().TextureCount() != 0) {
            throw std::runtime_error(
                "RenderDevice::Dispose did not reset the global transient pool"
            );
        }
        LOG_INFO(
            "[TESTCASE][PASS] name=TransientPoolDeviceDispose "
            "shutdown_order=executor,pool,backend cached_resources=0"
        );
        render_device_initialized = false;
        TaskSystem::ShutDown();
        task_system_initialized = false;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TESTCASE][EXCEPTION] " << error.what() << std::endl;
        LOG_ERROR("[TESTCASE][FAIL] name=ParallelRecordOrderedReadback error={}", error.what());
        if (render_device_initialized) {
            RenderDevice::Dispose();
        }
        if (task_system_initialized) {
            TaskSystem::ShutDown();
        }
        return 1;
    } catch (...) {
        std::cerr << "[TESTCASE][EXCEPTION] unknown" << std::endl;
        LOG_ERROR("[TESTCASE][FAIL] name=ParallelRecordOrderedReadback error=unknown");
        if (render_device_initialized) {
            RenderDevice::Dispose();
        }
        if (task_system_initialized) {
            TaskSystem::ShutDown();
        }
        return 1;
    }
}
