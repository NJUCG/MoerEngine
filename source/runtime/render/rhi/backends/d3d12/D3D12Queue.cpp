#include "D3D12Backend.h"

namespace Moer::Render::D3D12 {
namespace {
void RunCallbacks(Array<std::function<void()>>& callbacks) {
    auto owned = std::move(callbacks);
    for (auto& callback : owned)
        try {
            callback();
        } catch (const std::exception& error) {
            LOG_ERROR("D3D12 completion callback: {}", error.what());
        } catch (...) {
            LOG_ERROR("D3D12 completion callback threw");
        }
}
} // namespace
Queue::Queue(Context& context) :
    context(context),
    timeline(MoerNew(Timeline)(context.device.Get())) {
    const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    DX_CHECK_HRESULT(context.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    queue->SetName(L"Moer Graphics/Compute/Copy");
    worker = std::jthread([this](std::stop_token stop) {
        Complete(stop);
    });
}
Queue::~Queue() {
    try {
        Sync();
    } catch (const std::exception& error) {
        LOG_ERROR("D3D12 queue drain: {}", error.what());
    }
    worker.request_stop();
    completion_cv.notify_all();
    worker.join();
}
void Queue::Wait(WaitEvent event) {
    std::lock_guard lock(submission_mutex);
    auto*           fence = reinterpret_cast<Timeline*>(event.timeline_handle);
    if (!fence || !fence->IsSubmitted(event.value))
        throw std::runtime_error("D3D12 queue wait producer was not accepted");
    DX_CHECK_HRESULT(queue->Wait(fence->fence.Get(), event.value));
}
WaitEvent Queue::SubmitPacket(std::unique_ptr<Packet> owning_packet) {
    // Allocate Completion's owner before native execution. An allocation
    // failure must not free resources already visible to the GPU.
    owning_packet->error.reserve(256);
    std::unique_lock completion_lock(completion_mutex);
    pending.push_back(std::move(owning_packet));
    Packet* packet = pending.back().get();
    auto&   submit = packet->submit;
    if (packet->error.empty()) {
        for (const auto wait : submit.wait_events) {
            auto* fence = reinterpret_cast<Timeline*>(wait.timeline_handle);
            if (!fence || !fence->IsSubmitted(wait.value)) {
                packet->error = "D3D12 dependency producer was not accepted";
                break;
            }
            const HRESULT result = queue->Wait(fence->fence.Get(), wait.value);
            if (FAILED(result)) {
                packet->error = "D3D12 native queue wait failed";
                break;
            }
        }
    }
    if (packet->error.empty()) {
        ID3D12CommandList* lists[] = {packet->list.Get()};
        queue->ExecuteCommandLists(1, lists);
        packet->accepted = true;
        // Once ExecuteCommandLists crosses the native boundary, resources and
        // allocator storage belong to Completion, including on device removal.
        packet->rollback.clear();
        for (const auto signal : submit.signal_events) {
            auto*         fence  = reinterpret_cast<Timeline*>(signal.timeline_handle);
            const HRESULT result = queue->Signal(fence->fence.Get(), signal.value);
            if (SUCCEEDED(result))
                fence->MarkSubmitted(signal.value);
            else
                packet->error = "D3D12 native fence signal failed";
        }
    } else {
        for (auto it = packet->rollback.rbegin(); it != packet->rollback.rend(); ++it)
            (*it)();
        packet->rollback.clear();
        packet->accepted = true;
    }
    if (packet->presentation) {
        const HRESULT result = packet->error.empty() ? packet->presentation->Present(0, 0) : E_FAIL;
        if (packet->present_receipt)
            packet->present_receipt->Resolve(SUCCEEDED(result));
        if (FAILED(result))
            packet->error = "D3D12 swapchain Present failed";
    }
    // Present may queue native work after the copy command list. The retirement
    // signal must cover that work before backbuffers can be resized or freed.
    packet->timeline     = ++next_timeline;
    const uint64  value  = packet->timeline;
    const HRESULT signal = queue->Signal(static_cast<Timeline*>(timeline.Get())->fence.Get(), value);
    if (FAILED(signal))
        packet->error = "D3D12 queue completion signal failed";
    timeline->MarkSubmitted(value);
    completion_lock.unlock();
    completion_cv.notify_all();
    return {uint64(timeline.Get()), value};
}
WaitEvent Queue::Execute(CmdSubmit&& submit) {
    const bool sync = submit.b_sync || context.force_sync;
    WaitEvent  event{};
    {
        std::lock_guard lock(submission_mutex);
        auto            packet = std::make_unique<Packet>(std::move(submit));
        try {
            Recorder(context, *packet).Record();
        } catch (const std::exception& error) {
            packet->error = error.what();
            LOG_ERROR("D3D12 recording rejected: {}", error.what());
        }
        event = SubmitPacket(std::move(packet));
    }
    if (sync)
        Sync(event.value);
    return event;
}
void Queue::Sync() {
    uint64 value;
    {
        std::lock_guard lock(submission_mutex);
        value = next_timeline;
    }
    Sync(value);
}
void Queue::Sync(uint64 value) {
    if (std::this_thread::get_id() == worker.get_id())
        throw std::logic_error("D3D12 completion callback cannot wait for its own CPU retirement");
    std::unique_lock lock(completion_mutex);
    completion_cv.wait(lock, [&] {
        return retired >= value;
    });
}
std::unique_lock<std::mutex> Queue::LockIdle() {
    for (;;) {
        Sync();
        std::unique_lock lock(submission_mutex);
        std::lock_guard  completion_lock(completion_mutex);
        if (retired >= next_timeline)
            return lock;
        // A later Present raced the drain. Do not wait with submission_mutex:
        // Completion callbacks are allowed to submit further work.
    }
}
void Queue::Complete(std::stop_token stop) {
    RHIThreadRoleScope role(ERHIThreadRole::Completion);
    while (true) {
        std::unique_ptr<Packet> packet;
        {
            std::unique_lock lock(completion_mutex);
            completion_cv.wait(lock, stop, [&] {
                return !pending.empty();
            });
            if (pending.empty()) {
                if (stop.stop_requested())
                    break;
                else
                    continue;
            }
            packet = std::move(pending.front());
            pending.pop_front();
        }
        auto& submit  = packet->submit;
        bool  success = timeline->Wait(packet->timeline) && packet->error.empty();
        if (!success && packet->error.empty())
            packet->error = "D3D12 device removed before completion";
        const auto completion_batch = submit.gpu_completion_publish_batch.Valid() ? submit.gpu_completion_publish_batch : GpuCompletionBackendAccess::BeginPublishBatch();
        const auto query_batch      = submit.query_publish_batch.Valid() ? submit.query_publish_batch : QueryBackendAccess::BeginPublishBatch();
        if (success) {
            try {
                for (auto& readback : packet->readbacks)
                    readback();
                if (packet->query_readback) {
                    auto&             buffer = *static_cast<GpuBuffer*>(packet->query_readback.Get());
                    void*             mapped = nullptr;
                    const D3D12_RANGE range{0, SIZE_T(buffer.GetByteSize())};
                    DX_CHECK_HRESULT(buffer.resource->Map(0, &range, &mapped));
                    uint64 frequency = 0;
                    DX_CHECK_HRESULT(queue->GetTimestampFrequency(&frequency));
                    for (const auto& query : packet->queries) {
                        const auto* values = static_cast<const uint64*>(mapped) + query.index;
                        if (query.token.Kind() == QueryKind::Timestamp) {
                            const double period = 1e9 / double(frequency);
                            QueryBackendAccess::PublishTimestamp(query.token, {values[0], values[1], 64, period, double(values[1] - values[0]) * period}, query_batch);
                        } else
                            QueryBackendAccess::PublishOcclusion(query.token, {values[0], values[0] != 0}, query_batch);
                    }
                    const D3D12_RANGE empty{0, 0};
                    buffer.resource->Unmap(0, &empty);
                }
            } catch (const std::exception& error) {
                success       = false;
                packet->error = error.what();
            }
        }
        submit.PublishPendingGpuCompletionOutcome(success, packet->error, completion_batch);
        if (!success) {
            submit.PublishPendingQueryErrors(packet->error, query_batch);
            submit.RejectPendingSignals();
            LOG_ERROR("D3D12 submit {} failed: {}", packet->timeline, packet->error);
        } else {
            submit.signal_events.clear();
            submit.signal_rejection_keepalives.clear();
        }
        // Publish every sibling before releasing any user callback.
        submit.NotifyPendingGpuCompletions(completion_batch);
        submit.NotifyPendingQueries(query_batch);
        if (success)
            RunCallbacks(submit.success_callbacks);
        else
            submit.success_callbacks.clear();
        RunCallbacks(submit.callbacks);
        const uint64 value = packet->timeline;
        packet.reset();
        {
            std::lock_guard lock(completion_mutex);
            retired = value;
        }
        completion_cv.notify_all();
    }
}
} // namespace Moer::Render::D3D12
