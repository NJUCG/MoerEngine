#pragma once

#include "rhi/RHICommand.h"
#include "shaderheaders/shared/raster/culling/ShaderParameters.h"

#include <atomic>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Moer::Render::Raster::Detail {

enum class LegacyCounterReadbackStatus : uint8 {
    Pending,
    Ready,
    Error,
};

struct LegacyCounterReadbackState {
    [[nodiscard]] LegacyCounterReadbackStatus GetStatus() const noexcept {
        return status.load(std::memory_order_acquire);
    }

    void Publish(LegacyCounterReadbackStatus _status) noexcept {
        LegacyCounterReadbackStatus expected =
            LegacyCounterReadbackStatus::Pending;
        (void)status.compare_exchange_strong(
            expected,
            _status,
            std::memory_order_release,
            std::memory_order_relaxed
        );
    }

    GpuCullingCounterData counters{};

private:
    std::atomic<LegacyCounterReadbackStatus> status{
        LegacyCounterReadbackStatus::Pending
    };
};

// The success callback is retained by an accepted submission until GPU
// retirement. Keeping the destination state in the same lease makes the raw
// CopyBackBufferCmd pointer safe even if the CullingPass is destroyed first.
// Destruction without successful retirement converts Pending to Error.
class LegacyCounterReadbackCompletion final {
public:
    explicit LegacyCounterReadbackCompletion(
        std::shared_ptr<LegacyCounterReadbackState> _state
    ) noexcept :
        state_(std::move(_state)) {}

    ~LegacyCounterReadbackCompletion() {
        state_->Publish(LegacyCounterReadbackStatus::Error);
    }

    void PublishReady() noexcept {
        state_->Publish(LegacyCounterReadbackStatus::Ready);
    }

private:
    std::shared_ptr<LegacyCounterReadbackState> state_;
};

[[nodiscard]] inline std::shared_ptr<LegacyCounterReadbackState>
QueueLegacyCounterReadback(
    CommandList&     _command_list,
    BufferView       _counter_buffer,
    std::string_view _name
) {
    if (_counter_buffer.GetBuffer() == nullptr ||
        _counter_buffer.GetByteSize() !=
            sizeof(GpuCullingCounterData)) {
        throw std::invalid_argument(
            "legacy culling counter readback requires one complete "
            "GpuCullingCounterData buffer view"
        );
    }

    auto state = std::make_shared<LegacyCounterReadbackState>();
    auto completion =
        std::make_shared<LegacyCounterReadbackCompletion>(state);

    // Register the only destination owner before recording the raw pointer.
    // Rejection discards success callbacks, whose final lease publishes Error.
    _command_list.AddSuccessCallback(
        [completion] { completion->PublishReady(); }
    );
    try {
        _command_list.CopyFrom(
            _counter_buffer,
            std::span<byte>(
                reinterpret_cast<byte*>(&state->counters),
                sizeof(state->counters)
            ),
            _name
        );
    } catch (...) {
        state->Publish(LegacyCounterReadbackStatus::Error);
        throw;
    }
    return state;
}

} // namespace Moer::Render::Raster::Detail
