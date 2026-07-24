#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIThreadOwnership.h"
#include "rhi/vulkan/VulkanCustomCommand.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanIOService.h"
#include "rhi/vulkan/VulkanQueue.h"
#include "rhi/vulkan/VulkanRHIResource.h"
#include "rhi/vulkan/VulkanSubmissionDiagnostics.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Moer;
using namespace Moer::Render;

namespace {

constexpr size_t kElementCount = 64;
constexpr uint32 kIterations   = 24;
constexpr uint32 kHeavyCopiesPerWave = 48;
constexpr uint32 kHeavyCopyCount = kHeavyCopiesPerWave * 2;

class TranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    explicit TranslateProbeCommand(std::binary_semaphore* _translated) :
        translated(_translated) {}

    void Execute(const VkDispatchContext&) const override {
        translated->release();
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }

    std::binary_semaphore* translated;
};

class BlockingTranslateProbeCommand final : public VkCustomDispatchCmd {
public:
    BlockingTranslateProbeCommand(
        EQueueType             _queue,
        std::binary_semaphore* _entered,
        std::binary_semaphore* _release
    ) :
        queue(_queue),
        entered(_entered),
        release(_release) {}

    void Execute(const VkDispatchContext&) const override {
        entered->release();
        release->acquire();
    }

    EQueueType GetQueueType() const override {
        return queue;
    }

private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return {};
    }

    EQueueType             queue;
    std::binary_semaphore* entered;
    std::binary_semaphore* release;
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
    std::array<VulkanSourceSubmissionEvent, 3> events{};
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

class CopyDependencyWaitCapture final {
public:
    static void Observe(
        void*                                        _context,
        const VulkanSubmissionDependencyWaitEvent& _event
    ) noexcept {
        auto& capture =
            *static_cast<CopyDependencyWaitCapture*>(_context);
        if (_event.queue != EQueueType::Copy) {
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
};

class ScopedDependencyWaitObserver final {
public:
    explicit ScopedDependencyWaitObserver(
        CopyDependencyWaitCapture& _capture
    ) :
        observer{
            .context  = &_capture,
            .callback = &CopyDependencyWaitCapture::Observe,
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
            argument != "--inject-translate-failure") {
            throw std::invalid_argument("unsupported argument: " + std::string(argument));
        }
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

    const RHIQueueTopology topology = RenderDevice::Get().GetQueueTopology();
    if (!topology.graphics.available || !topology.compute.available) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "graphics_native={} compute_native={} reason=queue_unavailable "
            "cpu_seam=VulkanTranslateWaveScheduler",
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id
        );
        return;
    }
    if (topology.graphics.native_queue_id == topology.compute.native_queue_id) {
        LOG_INFO(
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "graphics_native={} compute_native={} reason=native_queue_alias "
            "cpu_seam=VulkanTranslateWaveScheduler",
            topology.graphics.native_queue_id,
            topology.compute.native_queue_id
        );
        return;
    }

    constexpr uint64                   async_queue_scope = 0x5048415345313541ull;
    SourceSubmissionCapture            submission_capture(async_queue_scope);
    ScopedSourceSubmissionObserver     submission_observer(submission_capture);
    std::array<std::atomic<uint32>, 3> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 3> success_callbacks{};
    std::binary_semaphore              graphics_front_entered{0};
    std::binary_semaphore              graphics_tail_entered{0};
    std::binary_semaphore              compute_later_entered{0};
    std::binary_semaphore              release_graphics_front{0};
    std::binary_semaphore              release_compute_later{0};
    OneShotSemaphoreRelease            release_graphics_guard(release_graphics_front);
    OneShotSemaphoreRelease            release_compute_guard(release_compute_later);

    try {
        CommandList graphics_front(EQueueType::Graphics);
        graphics_front.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        graphics_front.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Graphics, &graphics_front_entered, &release_graphics_front
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
        graphics_front_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
        graphics_front_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        graphics_front_submit.async_queue_scope = async_queue_scope;

        CommandList graphics_tail(EQueueType::Graphics);
        graphics_tail.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        graphics_tail.AddCustomCommand(
            MakeUnique<TranslateProbeCommand>(&graphics_tail_entered), "AsyncQueueGraphicsTailTranslateProbe"
        );
        graphics_tail.AddCallback([&] {
            ordinary_callbacks[1].fetch_add(1, std::memory_order_relaxed);
        });
        graphics_tail.AddSuccessCallback([&] {
            success_callbacks[1].fetch_add(1, std::memory_order_relaxed);
        });
        CmdSubmit graphics_tail_submit = graphics_tail.Submit();
        graphics_tail_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
        graphics_tail_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        graphics_tail_submit.async_queue_scope = async_queue_scope;

        CommandList compute_later(EQueueType::Compute);
        compute_later.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        compute_later.AddCustomCommand(
            MakeUnique<BlockingTranslateProbeCommand>(
                EQueueType::Compute, &compute_later_entered, &release_compute_later
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
        compute_later_submit.SetTranslateExecutionClass(ERHITranslateExecutionClass::Parallel);
        compute_later_submit.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
        compute_later_submit.async_queue_scope = async_queue_scope;

        Array<RHIBackendSubmissionBatchEntry> submits{};
        submits.emplace_back(EQueueType::Graphics, std::move(graphics_front_submit));
        submits.emplace_back(EQueueType::Graphics, std::move(graphics_tail_submit));
        submits.emplace_back(EQueueType::Compute, std::move(compute_later_submit));
        RHIExecutor::Get().Submit(std::move(submits), ERHIExecSubmitFlags::FlushGPU);

        const auto deadline                             = std::chrono::steady_clock::now() + 5s;
        const bool graphics_front_translate_entered     = graphics_front_entered.try_acquire_until(deadline);
        const bool compute_later_translate_entered      = compute_later_entered.try_acquire_until(deadline);
        const bool graphics_tail_entered_before_release = graphics_tail_entered.try_acquire();
        release_graphics_guard.Release();
        release_compute_guard.Release();

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        const bool graphics_tail_translate_entered =
            graphics_tail_entered_before_release || graphics_tail_entered.try_acquire();

        if (!graphics_front_translate_entered || !compute_later_translate_entered ||
            graphics_tail_entered_before_release || !graphics_tail_translate_entered) {
            throw std::runtime_error("ready native lane did not bypass a blocked same-native "
                                     "successor while preserving its release gate");
        }
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
                success_callbacks[index].load(std::memory_order_acquire) != 1) {
                throw std::runtime_error("parallel Translate callbacks did not retire exactly once");
            }
        }

        if (submission_capture.Overflowed() || submission_capture.Count() != 3) {
            throw std::runtime_error("parallel Translate did not produce exactly three observed "
                                     "source submissions");
        }
        constexpr std::array expected_queues{
            EQueueType::Graphics,
            EQueueType::Graphics,
            EQueueType::Compute,
        };
        const uint64 observed_batch_sequence = submission_capture.Event(0).batch_sequence;
        if (observed_batch_sequence == 0) {
            throw std::runtime_error("Submission observer reported an invalid batch sequence");
        }
        for (size_t index = 0; index < expected_queues.size(); ++index) {
            const VulkanSourceSubmissionEvent& event = submission_capture.Event(index);
            if (event.source_index != index || event.queue != expected_queues[index] ||
                event.batch_sequence != observed_batch_sequence) {
                throw std::runtime_error("Submission owner did not preserve stable G,G,C source "
                                         "order in one batch");
            }
        }

        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        for (size_t index = 0; index < ordinary_callbacks.size(); ++index) {
            if (ordinary_callbacks[index].load(std::memory_order_acquire) != 1 ||
                success_callbacks[index].load(std::memory_order_acquire) != 1) {
                throw std::runtime_error("a second Sync replayed parallel Translate callbacks");
            }
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
        "graphics_native={} compute_native={} sources=3 batch=1 "
        "source_order=G,G,C first_ready_lanes=G,C "
        "observed_submit_order=G,G,C async_scope={} explicit_state=true "
        "callbacks=exactly_once",
        topology.graphics.native_queue_id,
        topology.compute.native_queue_id,
        async_queue_scope
    );
}

void RunParallelTranslateFailureRetirement() {
    auto&                  device            = RenderDevice::Get();
    const RHIQueueTopology topology          = device.GetQueueTopology();
    constexpr uint64       async_queue_scope = 0x5048313541464149ull;

    FenceRef                           graphics_done = device.CreateFence();
    FenceRef                           compute_done  = device.CreateFence();
    FenceRef                           later_done    = device.CreateFence();
    std::array<std::atomic<uint32>, 3> ordinary_callbacks{};
    std::array<std::atomic<uint32>, 3> success_callbacks{};
    std::binary_semaphore              compute_translated{0};

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

    CommandList compute(EQueueType::Compute);
    compute.SetResourceStateOwnership(ERHIResourceStateOwnership::Explicit);
    compute.AddCustomCommand(
        MakeUnique<TranslateProbeCommand>(&compute_translated), "ParallelTranslateRecordedSuffixProbe"
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

    Array<RHIBackendSubmissionBatchEntry> failing_batch{};
    failing_batch.emplace_back(EQueueType::Graphics, std::move(graphics_submit));
    failing_batch.emplace_back(EQueueType::Compute, std::move(compute_submit));
    RHIExecutor::Get().Submit(std::move(failing_batch), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const bool distinct_native       = topology.graphics.native_queue_id != topology.compute.native_queue_id;
    const bool suffix_was_translated = compute_translated.try_acquire();
    if (suffix_was_translated != distinct_native) {
        throw std::runtime_error("parallel Translate failure did not preserve native-lane wave "
                                 "dispatch semantics");
    }
    if (!ResourceCast(graphics_done.Get())->IsFailed() || !ResourceCast(compute_done.Get())->IsFailed() ||
        ResourceCast(graphics_done.Get())->IsRejected(1) || ResourceCast(compute_done.Get())->IsRejected(1)) {
        throw std::runtime_error("hard parallel Translate failure did not fail every wave signal");
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

    LOG_INFO(
        "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
        "distinct_native={} suffix_recorded={} signals=failed "
        "callbacks=exactly_once hard_latch=later_batch_failed",
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
    CopyDependencyWaitCapture      copy_wait_capture{};
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
            .parallel_recording = parallel,
            .parallel_record_workers = 4,
            .parallel_record_verify = parallel,
            .parallel_record_min_work_units_per_job = production_gate ? 64u : 1u,
            .parallel_record_worker_throw_trigger = inject_worker_failure ? 1u : 0u,
        });
        render_device_initialized = true;

        if (inject_translate_failure) {
            RunParallelTranslateFailureRetirement();
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
