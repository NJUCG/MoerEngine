#include "rhi/metal/MetalQueue.h"
#include "rhi/metal/MetalBindless.h"
#include "rhi/metal/MetalDraw.h"
#include "rhi/metal/MetalPipeline.h"
#include "rhi/metal/MetalResource.h"
#include "rhi/metal/MetalSwapchain.h"
#include "rhi/RHIThreadOwnership.h"
#include "log/LogSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace Moer::Render {
namespace {
void ValidateQueueTransfer(const QueueTransferCmd& transfer, EQueueType current_queue) {
    const EQueueType other_queue = transfer.IsImport() ? transfer.src_queue : transfer.dst_queue;
    if ((current_queue != EQueueType::Graphics && current_queue != EQueueType::Copy) ||
        (other_queue != EQueueType::Graphics && other_queue != EQueueType::Copy) ||
        current_queue == other_queue) {
        Unsupported("this queue transfer");
    }
    const auto validate_texture = [](TextureView view) {
        if (dynamic_cast<MetalTexture*>(view.GetTexture()) == nullptr) {
            Unsupported("a foreign texture in queue transfer");
        }
    };
    const auto validate_buffer = [](BufferView view) {
        if (dynamic_cast<MetalBuffer*>(view.GetBuffer()) == nullptr) {
            Unsupported("a foreign buffer in queue transfer");
        }
    };
    for (const auto& item : transfer.ImportTextures()) validate_texture(item.texture);
    for (const auto& item : transfer.ExportTextures()) validate_texture(item.texture);
    for (const auto& item : transfer.ImportBuffers()) validate_buffer(item.buffer);
    for (const auto& item : transfer.ExportBuffers()) validate_buffer(item.buffer);
}

void ValidateBarrier(const BarrierCmd& barrier) {
    // Metal resources use the default tracked hazard mode. Each graphics,
    // compute, and blit operation has its own encoder on the same native queue,
    // so same-queue state transitions need no explicit encoding here.
    if (barrier.IsQueueTransition() ||
        (barrier.GetSrcQueue() != EQueueType::Ignore &&
         barrier.GetSrcQueue() != EQueueType::Graphics) ||
        (barrier.GetDstQueue() != EQueueType::Ignore &&
         barrier.GetDstQueue() != EQueueType::Graphics)) {
        Unsupported("a cross-queue graphics barrier");
    }
    const auto validate_texture = [](uint64 handle, uint mip, uint mip_count,
                                     uint layer, uint layer_count) {
        auto* texture = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(handle));
        if (texture == nullptr || mip_count == 0 || layer_count == 0 ||
            mip + mip_count > texture->GetNumMips() ||
            layer + layer_count > texture->GetNumArray()) {
            Unsupported("this graphics texture barrier range");
        }
    };
    const auto validate_buffer = [](uint64 handle, uint64 offset, uint64 size) {
        auto* buffer = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(handle));
        if (buffer == nullptr || size == 0 || offset > buffer->GetByteSize() ||
            size > buffer->GetByteSize() - offset) {
            Unsupported("this graphics buffer barrier range");
        }
    };
    for (const auto& item : barrier.ReadTextures()) {
        validate_texture(item.handle, item.mip_level, item.mip_cnt,
                         item.array_layer, item.array_cnt);
    }
    for (const auto& item : barrier.WriteTextures()) {
        validate_texture(item.handle, item.mip_level, item.mip_cnt,
                         item.array_layer, item.array_cnt);
    }
    for (const auto& item : barrier.ReadBuffers()) {
        validate_buffer(item.handle, item.offset, item.byte_size);
    }
    for (const auto& item : barrier.WriteBuffers()) {
        validate_buffer(item.handle, item.offset, item.byte_size);
    }
    for (const auto& item : barrier.ExplicitTextures()) {
        if (item.queue_transfer.phase != EBarrierQueueTransferPhase::None) {
            Unsupported("an explicit graphics queue ownership transfer");
        }
        validate_texture(item.handle, item.mip_level, item.mip_count,
                         item.array_layer, item.array_count);
    }
    for (const auto& item : barrier.ExplicitBuffers()) {
        if (item.queue_transfer.phase != EBarrierQueueTransferPhase::None) {
            Unsupported("an explicit graphics queue ownership transfer");
        }
        validate_buffer(item.handle, item.offset, item.byte_size);
    }
}

// Completion callbacks may submit more RHI work. Run them after Execute has
// returned from the executor publication gate, never on that gate's owner.
class MetalCompletionDispatcher final {
public:
    struct Packet {
        uint64 timeline{0};
        Array<std::function<void()>> callbacks;
        Array<std::function<void()>> success_callbacks;
    };
    static_assert(std::is_nothrow_move_assignable_v<Array<std::function<void()>>>);

    MetalCompletionDispatcher() : worker_([this] { Run(); }) {}

    ~MetalCompletionDispatcher() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    void Enqueue(std::list<Packet>& prepared) noexcept {
        {
            std::lock_guard lock(mutex_);
            pending_.splice(pending_.end(), prepared);
        }
        cv_.notify_all();
    }

    void WaitThrough(uint64 timeline) {
        if (std::this_thread::get_id() == worker_.get_id()) {
            throw std::runtime_error("Metal completion callback cannot wait for its own queue");
        }
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return finished_timeline_ >= timeline; });
    }

private:
    void Run() {
        RHIThreadRoleScope role(ERHIThreadRole::Completion);
        for (;;) {
            std::list<Packet> ready;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (pending_.empty() && stopping_) return;
                ready.splice(ready.end(), pending_, pending_.begin());
            }
            Packet& packet = ready.front();
            auto invoke = [](Array<std::function<void()>>& callbacks) {
                for (auto& callback : callbacks) {
                    try {
                        if (callback) callback();
                    } catch (const std::exception& error) {
                        try { LOG_ERROR("[Metal] completion callback failed: {}", error.what()); } catch (...) {}
                    } catch (...) {
                        try { LOG_ERROR("[Metal] completion callback failed"); } catch (...) {}
                    }
                }
            };
            invoke(packet.callbacks);
            invoke(packet.success_callbacks);
            const uint64 timeline = packet.timeline;
            ready.clear();
            {
                std::lock_guard lock(mutex_);
                finished_timeline_ = timeline;
            }
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::list<Packet> pending_;
    uint64 finished_timeline_{0};
    bool stopping_{false};
    std::thread worker_;
};

class MetalCommandQueue final : public CommandQueue {
public:
    explicit MetalCommandQueue(id<MTLCommandQueue> queue) : queue_(queue) {}

    void Wait(WaitEvent event) override {
        auto* fence = dynamic_cast<MetalFence*>(reinterpret_cast<Fence*>(event.timeline_handle));
        const std::atomic_bool do_not_wait{false};
        if (fence == nullptr ||
            !fence->WaitSubmitted(event.value, &do_not_wait, EQueueType::Graphics, 1)) {
            throw std::runtime_error("Metal graphics dependency was not submitted or was rejected");
        }
        fence->Wait(event.value);
        if (fence->IsRejected(event.value)) {
            throw std::runtime_error("Metal graphics dependency was rejected");
        }
    }
    WaitEvent Execute(CmdSubmit&& submit) override {
        if (!submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty()) {
            Unsupported("GPU completion or query tokens");
        }
        for (const SignalEvent& signal : submit.signal_events) {
            auto* fence = dynamic_cast<MetalFence*>(
                reinterpret_cast<Fence*>(signal.timeline_handle));
            if (fence == nullptr || signal.value == 0) {
                Unsupported("this graphics signal fence");
            }
        }
        // Frame profiling markers currently produce no Metal timestamp queries.
        // A submit without query tokens may still carry the frame boundary and
        // deferred-delete bit; Objective-C resource owners retire with CmdSubmit.
        // Validate the complete submit before encoding any GPU work. A later
        // unsupported command must not leave a partially executed submit.
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            if (command->Type() == Command::EType::Scope) {
                // Scope commands only mark GPU debug/profiling regions. Metal
                // timestamp collection is not enabled by this queue yet.
                continue;
            }
            if (command->Type() == Command::EType::QueueTransfer) {
                ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Graphics);
                continue;
            }
            if (command->Type() == Command::EType::Barrier) {
                ValidateBarrier(static_cast<const BarrierCmd&>(*command));
                continue;
            }
            if (command->Type() == Command::EType::UpdateBindlessArray) {
                const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
                if (dynamic_cast<MetalBindlessArray*>(update.Handle()) == nullptr) {
                    Unsupported("a foreign bindless array");
                }
                continue;
            }
            if (command->Type() == Command::EType::UploadTexture) {
                ValidateTextureUpload(static_cast<const UploadTextureCmd&>(*command));
                has_gpu_work = true;
                continue;
            }
            if (command->Type() == Command::EType::UploadBuffer) {
                ValidateBufferUpload(static_cast<const UploadBufferCmd&>(*command));
                has_gpu_work = true;
                continue;
            }
            if (command->Type() == Command::EType::CopyBackBuffer) {
                const auto& copy = static_cast<const CopyBackBufferCmd&>(*command);
                auto* buffer = dynamic_cast<MetalBuffer*>(
                    reinterpret_cast<Buffer*>(copy.Handle()));
                if (buffer == nullptr || copy.Data() == nullptr ||
                    copy.HasOwningReadback() || copy.ByteSize() == 0 ||
                    copy.Offset() > buffer->GetByteSize() ||
                    copy.ByteSize() > buffer->GetByteSize() - copy.Offset()) {
                    Unsupported("this graphics buffer readback layout");
                }
                has_gpu_work = true;
                continue;
            }
            if (command->Type() == Command::EType::SetDrawState) {
                ValidateSimpleDraw(static_cast<const SetDrawStateCmd&>(*command));
                has_gpu_work = true;
                continue;
            }
            if (command->Type() == Command::EType::ShaderDispatch) {
                ValidateComputeDispatch(
                    static_cast<const DispatchCmd&>(*command), submit.cached_args);
                has_gpu_work = true;
                continue;
            }
            if (command->Type() != Command::EType::ClearResource) {
                throw std::runtime_error("Metal RHI has not implemented graphics command " +
                    command->name + " (type=" +
                    std::to_string(static_cast<uint>(command->Type())) + ")");
            }
            has_gpu_work = true;
            const auto& clear = static_cast<const ClearResourceCmd&>(*command);
            if (clear.IsBuffer() && clear.IsUInt()) {
                const BufferView& view = clear.Buffer();
                auto* buffer = dynamic_cast<MetalBuffer*>(view.GetBuffer());
                if (buffer == nullptr || view.GetByteSize() == 0 ||
                    view.GetByteOffset() % sizeof(uint32_t) != 0 ||
                    view.GetByteSize() % sizeof(uint32_t) != 0 ||
                    view.GetByteOffset() > buffer->GetByteSize() ||
                    view.GetByteSize() > buffer->GetByteSize() - view.GetByteOffset()) {
                    Unsupported("this buffer clear view");
                }
                continue;
            }
            if (!clear.IsTexture() || !clear.IsFloat4()) {
                Unsupported("this clear value or resource");
            }
            const TextureView& view = clear.Texture();
            auto* texture = dynamic_cast<MetalTexture*>(view.GetTexture());
            if (texture == nullptr || view.format != texture->GetFormat() ||
                view.mip_level != 0 || view.num_mips != 1 ||
                view.array_layer != 0 || view.num_array != 1 ||
                view.offset != uint3{0, 0, 0} || view.extent != texture->GetExtent()) {
                Unsupported("this texture clear view");
            }
        }
        std::list<MetalCompletionDispatcher::Packet> prepared;
        if (!submit.callbacks.empty() || !submit.success_callbacks.empty()) {
            prepared.emplace_back(); // Reserve before accepting GPU work.
        }
        for (const WaitEvent& event : submit.wait_events) Wait(event);
        if (!has_gpu_work) {
            for (const auto& command : submit.cmds) {
                if (command->Type() != Command::EType::UpdateBindlessArray) continue;
                const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
                if (update.HandOffUpdates()) {
                    static_cast<MetalBindlessArray*>(update.Handle())->Apply(update.UpdateCommands());
                }
            }
        }
        if (has_gpu_work) @autoreleasepool {
            MTLCommandBufferDescriptor* buffer_descriptor = [MTLCommandBufferDescriptor new];
            buffer_descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
            const auto make_command_buffer = [&]() {
                return [queue_ commandBufferWithDescriptor:buffer_descriptor];
            };
            const auto gpu_error = [](id<MTLCommandBuffer> buffer) {
                std::string message(
                    [buffer.error.localizedDescription UTF8String] ?: "unknown GPU error");
                NSArray<id<MTLCommandBufferEncoderInfo>>* encoders =
                    buffer.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
                for (id<MTLCommandBufferEncoderInfo> encoder in encoders) {
                    if (encoder.errorState == MTLCommandEncoderErrorStateCompleted) continue;
                    message += " [";
                    message += encoder.label.UTF8String ?: "unnamed encoder";
                    message += ":" + std::to_string(encoder.errorState) + "]";
                }
                return message;
            };
            id<MTLCommandBuffer> command_buffer = make_command_buffer();
            if (command_buffer == nil) throw std::runtime_error("Cannot create Metal command buffer");
            NSMutableArray<id<MTLBuffer>>* staging_buffers = [NSMutableArray array];
            NSMutableArray<id<MTLTexture>>* texture_views = [NSMutableArray array];
            NSMutableArray<id<MTLSamplerState>>* sampler_states = [NSMutableArray array];
            struct PendingReadback {
                NSUInteger staging_index;
                void* destination;
                uint64 byte_size;
            };
            std::vector<PendingReadback> readbacks;
            bool encoded_work = false;
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::Scope ||
                    command->Type() == Command::EType::QueueTransfer ||
                    command->Type() == Command::EType::Barrier) continue;
                if (command->Type() == Command::EType::UpdateBindlessArray) {
                    if (encoded_work) {
                        [command_buffer commit];
                        [command_buffer waitUntilCompleted];
                        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                            throw std::runtime_error("Metal work before bindless update failed: " +
                                gpu_error(command_buffer));
                        }
                        command_buffer = make_command_buffer();
                        if (command_buffer == nil) {
                            throw std::runtime_error("Cannot create Metal command buffer");
                        }
                        encoded_work = false;
                    }
                    const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
                    if (update.HandOffUpdates()) {
                        static_cast<MetalBindlessArray*>(update.Handle())->Apply(update.UpdateCommands());
                    }
                    continue;
                }
                encoded_work = true;
                if (command->Type() == Command::EType::UploadTexture ||
                    command->Type() == Command::EType::UploadBuffer) {
                    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                    if (blit == nil) throw std::runtime_error("Cannot encode Metal graphics upload");
                    if (command->Type() == Command::EType::UploadTexture) {
                        EncodeTextureUpload(queue_.device, blit, staging_buffers,
                            static_cast<const UploadTextureCmd&>(*command));
                    } else {
                        EncodeBufferUpload(queue_.device, blit, staging_buffers,
                            static_cast<const UploadBufferCmd&>(*command));
                    }
                    [blit endEncoding];
                    continue;
                }
                if (command->Type() == Command::EType::SetDrawState) {
                    const auto& draw = static_cast<const SetDrawStateCmd&>(*command);
                    std::vector<uint> indirect_counts(draw.DrawData().size(), 0);
                    std::vector<std::pair<uint, id<MTLBuffer>>> pending_counts;
                    for (uint mesh_index = 0; mesh_index < draw.DrawData().size(); ++mesh_index) {
                        const auto& indirect = draw.DrawData()[mesh_index].indirect_draw_param;
                        if (!indirect) continue;
                        indirect_counts[mesh_index] = indirect->count;
                        if (!indirect->count_buffer) continue;
                        const BufferView& view = *indirect->count_buffer;
                        auto* source = static_cast<MetalBuffer*>(view.GetBuffer());
                        id<MTLBuffer> staging = [queue_.device
                            newBufferWithLength:sizeof(uint32_t)
                            options:MTLResourceStorageModeShared];
                        if (staging == nil) {
                            throw std::runtime_error("Cannot allocate Metal indirect count staging");
                        }
                        [staging_buffers addObject:staging];
                        pending_counts.emplace_back(mesh_index, staging);
                        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                        if (blit == nil) {
                            throw std::runtime_error("Cannot encode Metal indirect count readback");
                        }
                        [blit copyFromBuffer:source->Native() sourceOffset:view.GetByteOffset()
                                    toBuffer:staging destinationOffset:0 size:sizeof(uint32_t)];
                        [blit endEncoding];
                    }
                    if (!pending_counts.empty()) {
                        // The count may be produced by an earlier dispatch in this submit.
                        // Finish that work before deciding how many Metal draws to encode.
                        [command_buffer commit];
                        [command_buffer waitUntilCompleted];
                        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                            throw std::runtime_error("Metal indirect count readback failed: " +
                                gpu_error(command_buffer));
                        }
                        for (const auto& [mesh_index, staging] : pending_counts) {
                            uint32_t count = 0;
                            std::memcpy(&count, staging.contents, sizeof(count));
                            indirect_counts[mesh_index] = std::min(count, indirect_counts[mesh_index]);
                        }
                        command_buffer = make_command_buffer();
                        if (command_buffer == nil) {
                            throw std::runtime_error("Cannot create Metal command buffer");
                        }
                        encoded_work = false;
                    }
                    EncodeSimpleDraw(
                        command_buffer, draw, indirect_counts, staging_buffers,
                        texture_views, sampler_states);
                    encoded_work = true;
                    continue;
                }
                if (command->Type() == Command::EType::ShaderDispatch) {
                    EncodeComputeDispatch(
                        command_buffer, staging_buffers, texture_views,
                        static_cast<const DispatchCmd&>(*command), submit.cached_args);
                    continue;
                }
                if (command->Type() == Command::EType::CopyBackBuffer) {
                    const auto& copy = static_cast<const CopyBackBufferCmd&>(*command);
                    auto* buffer = static_cast<MetalBuffer*>(
                        reinterpret_cast<Buffer*>(copy.Handle()));
                    id<MTLBuffer> staging = [queue_.device
                        newBufferWithLength:copy.ByteSize()
                        options:MTLResourceStorageModeShared];
                    if (staging == nil) throw std::runtime_error("Cannot allocate Metal readback staging");
                    [staging_buffers addObject:staging];
                    readbacks.push_back({staging_buffers.count - 1, copy.Data(), copy.ByteSize()});
                    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                    if (blit == nil) throw std::runtime_error("Cannot encode Metal buffer readback");
                    [blit copyFromBuffer:buffer->Native() sourceOffset:copy.Offset()
                                toBuffer:staging destinationOffset:0 size:copy.ByteSize()];
                    [blit endEncoding];
                    continue;
                }
                const auto& clear = static_cast<const ClearResourceCmd&>(*command);
                if (clear.IsBuffer()) {
                    const BufferView& view = clear.Buffer();
                    auto* buffer = static_cast<MetalBuffer*>(view.GetBuffer());
                    const uint32_t value = clear.UIntValue();
                    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                    if (blit == nil) throw std::runtime_error("Cannot encode Metal buffer clear");
                    if ((value & 0xffu) == ((value >> 8) & 0xffu) &&
                        (value & 0xffu) == ((value >> 16) & 0xffu) &&
                        (value & 0xffu) == ((value >> 24) & 0xffu)) {
                        [blit fillBuffer:buffer->Native()
                                    range:NSMakeRange(view.GetByteOffset(), view.GetByteSize())
                                    value:static_cast<uint8_t>(value)];
                    } else {
                        id<MTLBuffer> staging = [queue_.device
                            newBufferWithLength:view.GetByteSize()
                            options:MTLResourceStorageModeShared];
                        if (staging == nil) throw std::runtime_error("Cannot allocate Metal buffer clear staging");
                        auto* words = static_cast<uint32_t*>(staging.contents);
                        for (uint64 i = 0; i < view.GetByteSize() / sizeof(uint32_t); ++i) {
                            words[i] = value;
                        }
                        [staging_buffers addObject:staging];
                        [blit copyFromBuffer:staging sourceOffset:0
                                    toBuffer:buffer->Native()
                          destinationOffset:view.GetByteOffset() size:view.GetByteSize()];
                    }
                    [blit endEncoding];
                    continue;
                }
                auto* texture = static_cast<MetalTexture*>(clear.Texture().GetTexture());
                const float4 color = clear.Float4Value();
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = texture->Native();
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(
                    color.x, color.y, color.z, color.w
                );
                id<MTLRenderCommandEncoder> encoder =
                    [command_buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) throw std::runtime_error("Cannot encode Metal texture clear");
                [encoder endEncoding];
            }
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal texture clear failed: " +
                    gpu_error(command_buffer));
            }
            for (const PendingReadback& readback : readbacks) {
                std::memcpy(readback.destination,
                            [staging_buffers[readback.staging_index] contents],
                            readback.byte_size);
            }
        }
        for (const SignalEvent& signal : submit.signal_events) {
            auto* fence = static_cast<MetalFence*>(
                reinterpret_cast<Fence*>(signal.timeline_handle));
            fence->MarkSubmitted(signal.value);
            fence->Complete(signal.value);
        }
        if (!prepared.empty()) {
            std::lock_guard lock(completion_enqueue_mutex_);
            prepared.front().timeline = ++next_callback_timeline_;
            prepared.front().callbacks = std::move(submit.callbacks);
            prepared.front().success_callbacks = std::move(submit.success_callbacks);
            completions_.Enqueue(prepared);
        }
        return {0, 0};
    }

    void Present(SwapchainRef swapchain, TextureView source, PresentReceiptRef receipt) override {
        auto* target = dynamic_cast<MetalSwapchain*>(swapchain.Get());
        auto* texture = dynamic_cast<MetalTexture*>(source.texture);
        if (target == nullptr || texture == nullptr || !target->IsPresentationReady()) {
            if (receipt) receipt->Resolve(false);
            return;
        }
        @autoreleasepool {
            id<CAMetalDrawable> drawable = target->NextDrawable();
            id<MTLTexture> source_texture = texture->Native();
            if (drawable == nil || source_texture.pixelFormat != drawable.texture.pixelFormat ||
                source_texture.width != drawable.texture.width ||
                source_texture.height != drawable.texture.height) {
                if (receipt) receipt->Resolve(false, true);
                return;
            }
            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) {
                if (receipt) receipt->Resolve(false);
                return;
            }
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (blit == nil) {
                if (receipt) receipt->Resolve(false);
                return;
            }
            [blit copyFromTexture:source_texture
                     sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(source_texture.width, source_texture.height, 1)
                       toTexture:drawable.texture
                destinationSlice:0 destinationLevel:0
               destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [command presentDrawable:drawable];
            [command commit];
            if (receipt) receipt->Resolve(true);
        }
    }

    void Sync() override {
        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        [command commit];
        [command waitUntilCompleted];
        completions_.WaitThrough(next_callback_timeline_.load());
    }

    ProfileData GetProfilerEntry() override { return {}; }

private:
    id<MTLCommandQueue> queue_;
    MetalCompletionDispatcher completions_;
    std::mutex completion_enqueue_mutex_;
    std::atomic<uint64> next_callback_timeline_{0};
};

class MetalCopyQueue final : public CopyQueue {
public:
    explicit MetalCopyQueue(id<MTLCommandQueue> queue) :
        queue_(queue), timeline_fence_(MoerNew(MetalFence)()) {}

    IOWaitEvt Execute(IOQueueSubmission&&) override { Unsupported("IO queue submission"); }
    IOWaitEvt Execute(CmdSubmit&& submit) override {
        std::lock_guard lock(mutex_);
        if (!submit.wait_events.empty() || !submit.signal_events.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("copy submission synchronization or profiling");
        }
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            switch (command->Type()) {
                case Command::EType::QueueTransfer:
                    ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Copy);
                    break;
                case Command::EType::UploadTexture: {
                    has_gpu_work = true;
                    ValidateTextureUpload(static_cast<const UploadTextureCmd&>(*command));
                    break;
                }
                case Command::EType::UploadBuffer: {
                    has_gpu_work = true;
                    ValidateBufferUpload(static_cast<const UploadBufferCmd&>(*command));
                    break;
                }
                case Command::EType::BufferToBuffer: {
                    has_gpu_work = true;
                    const auto& copy = static_cast<const CopyBufferCmd&>(*command);
                    auto* source = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.SrcHandle()));
                    auto* destination = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.DstHandle()));
                    if (source == nullptr || destination == nullptr || copy.ByteSize() == 0 ||
                        copy.SrcOffset() > source->GetByteSize() ||
                        copy.ByteSize() > source->GetByteSize() - copy.SrcOffset() ||
                        copy.DstOffset() > destination->GetByteSize() ||
                        copy.ByteSize() > destination->GetByteSize() - copy.DstOffset()) {
                        Unsupported("this buffer copy layout");
                    }
                    break;
                }
                case Command::EType::TextureToTexture: {
                    has_gpu_work = true;
                    const auto& copy = static_cast<const CopyTextureCmd&>(*command);
                    auto* source = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.SrcHandle()));
                    auto* destination = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.DstHandle()));
                    if (source == nullptr || destination == nullptr ||
                        copy.Format() != source->GetFormat() || copy.Format() != destination->GetFormat() ||
                        copy.SrcMipLevel() != 0 || copy.DstMipLevel() != 0 ||
                        copy.SrcOffset() != uint3{0, 0, 0} || copy.DstOffset() != uint3{0, 0, 0} ||
                        copy.Size() != source->GetExtent() || copy.Size() != destination->GetExtent()) {
                        Unsupported("this texture copy layout");
                    }
                    break;
                }
                default:
                    Unsupported("this copy command");
            }
        }
        std::list<MetalCompletionDispatcher::Packet> prepared;
        prepared.emplace_back(); // Allocate the completion node before GPU work is accepted.

        if (has_gpu_work) @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
            if (blit == nil) throw std::runtime_error("Cannot encode Metal copy commands");
            NSMutableArray<id<MTLBuffer>>* staging_buffers = [NSMutableArray array];
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::QueueTransfer) continue;
                if (command->Type() == Command::EType::BufferToBuffer) {
                    const auto& copy = static_cast<const CopyBufferCmd&>(*command);
                    auto* source = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.SrcHandle()));
                    auto* destination = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.DstHandle()));
                    [blit copyFromBuffer:source->Native() sourceOffset:copy.SrcOffset()
                               toBuffer:destination->Native() destinationOffset:copy.DstOffset()
                                   size:copy.ByteSize()];
                    continue;
                }
                if (command->Type() == Command::EType::TextureToTexture) {
                    const auto& copy = static_cast<const CopyTextureCmd&>(*command);
                    auto* source = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.SrcHandle()));
                    auto* destination = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.DstHandle()));
                    [blit copyFromTexture:source->Native() sourceSlice:0 sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(copy.Size().x, copy.Size().y, 1)
                               toTexture:destination->Native()
                        destinationSlice:0 destinationLevel:0
                       destinationOrigin:MTLOriginMake(0, 0, 0)];
                    continue;
                }
                if (command->Type() == Command::EType::UploadBuffer) {
                    EncodeBufferUpload(queue_.device, blit, staging_buffers,
                        static_cast<const UploadBufferCmd&>(*command));
                    continue;
                }
                EncodeTextureUpload(queue_.device, blit, staging_buffers,
                    static_cast<const UploadTextureCmd&>(*command));
            }
            [blit endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal copy submission failed: " +
                    std::string([command_buffer.error.localizedDescription UTF8String] ?: "unknown GPU error"));
            }
        }
        const uint64 timeline = ++next_timeline_;
        timeline_fence_->MarkSubmitted(timeline);
        static_cast<MetalFence*>(timeline_fence_.Get())->Complete(timeline);
        prepared.front().timeline = timeline;
        prepared.front().callbacks = std::move(submit.callbacks);
        prepared.front().success_callbacks = std::move(submit.success_callbacks);
        completions_.Enqueue(prepared);
        return {uint64(timeline_fence_.Get()), timeline};
    }
    void CopyFrom(BufferView, BufferView) override { Unsupported("buffer copy"); }
    void CopyFrom(TextureView, TextureView) override { Unsupported("texture copy"); }
    void CopyFrom(TextureView, BufferView) override { Unsupported("texture readback"); }
    void CopyFrom(BufferView, TextureView) override { Unsupported("texture upload"); }
    void CopyFrom(std::span<byte>, BufferView) override { Unsupported("buffer upload"); }
    void CopyFrom(std::span<byte>, TextureView) override { Unsupported("texture upload"); }
    FenceRef GetFenceHandle() override { return timeline_fence_; }
    void Sync(uint64 timeline) override {
        timeline_fence_->Wait(timeline);
        if (timeline_fence_->IsRejected(timeline)) {
            throw std::runtime_error("Metal copy timeline was rejected");
        }
        completions_.WaitThrough(timeline);
    }

private:
    id<MTLCommandQueue> queue_;
    std::mutex mutex_;
    MetalCompletionDispatcher completions_;
    FenceRef timeline_fence_;
    uint64 next_timeline_{0};
};

} // namespace

MetalGraphicsQueuePtr CreateMetalGraphicsQueue(id<MTLCommandQueue> queue) {
    return MetalGraphicsQueuePtr(new MetalCommandQueue(queue), [](CommandQueue* value) {
        delete static_cast<MetalCommandQueue*>(value);
    });
}

MetalCopyQueuePtr CreateMetalCopyQueue(id<MTLCommandQueue> queue) {
    return MetalCopyQueuePtr(new MetalCopyQueue(queue), [](CopyQueue* value) {
        delete static_cast<MetalCopyQueue*>(value);
    });
}

} // namespace Moer::Render
