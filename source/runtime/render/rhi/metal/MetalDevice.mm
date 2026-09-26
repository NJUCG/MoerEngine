#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/metal/MetalDevice.h"
#include "log/LogSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Moer::Render {
namespace {

[[noreturn]] void Unsupported(const char* operation) {
    throw std::runtime_error(std::string("Metal RHI has not implemented ") + operation);
}

MTLPixelFormat ToMetalFormat(EPixelFormat format) {
    switch (format) {
        case PF_R8G8B8A8_UNORM:
            return MTLPixelFormatRGBA8Unorm;
        case PF_R8G8B8A8_SRGB:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case PF_R32G32B32A32_SFLOAT:
            return MTLPixelFormatRGBA32Float;
        default:
            Unsupported("this color format");
    }
}

NSUInteger PixelStride(EPixelFormat format) {
    return format == PF_R32G32B32A32_SFLOAT ? 16 : 4;
}

// The bootstrap queues wait for each command buffer to finish. This host
// timeline still distinguishes submitted, completed, and rejected values so
// callers cannot mistake a rejected dependency for completed GPU work.
class MetalFence final : public Fence {
public:
    uint64 GetValue() const override {
        std::lock_guard lock(mutex_);
        return completed_;
    }

    void Wait(uint64 value) override {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return completed_ >= value || IsRejectedLocked(value); });
    }

    void MarkSubmitted(uint64 value) override {
        {
            std::lock_guard lock(mutex_);
            submitted_ = std::max(submitted_, value);
        }
        cv_.notify_all();
    }

    bool WaitSubmitted(
        uint64 value, const std::atomic_bool* continue_waiting, EQueueType, uint32
    ) override {
        std::unique_lock lock(mutex_);
        while (submitted_ < value && !IsRejectedLocked(value) &&
               (continue_waiting == nullptr || continue_waiting->load(std::memory_order_acquire))) {
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        }
        return submitted_ >= value && !IsRejectedLocked(value);
    }

    void Reject(uint64 value) noexcept override {
        {
            std::lock_guard lock(mutex_);
            try {
                rejected_.insert(value);
            } catch (...) {
                reject_all_ = true;
            }
        }
        cv_.notify_all();
    }

    bool IsRejected(uint64 value) const override {
        std::lock_guard lock(mutex_);
        return IsRejectedLocked(value);
    }

    void Complete(uint64 value) {
        {
            std::lock_guard lock(mutex_);
            completed_ = std::max(completed_, value);
        }
        cv_.notify_all();
    }

private:
    bool IsRejectedLocked(uint64 value) const {
        return reject_all_ || rejected_.contains(value);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64 submitted_{0};
    uint64 completed_{0};
    std::unordered_set<uint64> rejected_;
    bool reject_all_{false};
};

class MetalBuffer final : public Buffer {
public:
    MetalBuffer(const BufferInfo& info, id<MTLBuffer> buffer) : Buffer(info), buffer_(buffer) {}

    id<MTLBuffer> Native() const noexcept { return buffer_; }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        buffer_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLBuffer> buffer_;
};

class MetalTexture final : public Texture {
public:
    MetalTexture(const TextureInfo& info, id<MTLTexture> texture) : Texture(info), texture_(texture) {}

    id<MTLTexture> Native() const noexcept { return texture_; }

    uint GetMipByteSize(uint mip) const override {
        if (mip != 0) Unsupported("texture mip sizes");
        return GetWidth() * GetHeight() * PixelStride(GetFormat());
    }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        texture_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLTexture> texture_;
};

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

class MetalSwapchain final : public Swapchain {
public:
    MetalSwapchain(id<MTLDevice> device, const SwapchainCreateInfo& info) : device_(device) {
        Recreate(info);
    }

    ~MetalSwapchain() override { Detach(); }

    bool Recreate(const SwapchainCreateInfo& info) override {
        if (!info.surface.IsValid() || info.size.x == 0 || info.size.y == 0 ||
            ![NSThread isMainThread]) {
            return false;
        }
        const WindowNativeHandle handle = info.surface.source->GetNativeWindow();
        if (handle.window_system != EWindowSystemType::GLFW) return false;
        auto* glfw_window = reinterpret_cast<GLFWwindow*>(handle.window_system_handle);
        NSWindow* window = glfwGetCocoaWindow(glfw_window);
        NSView* view = window.contentView;
        if (view == nil) return false;

        const MTLPixelFormat pixel_format = ToMetalFormat(info.preferred_format);
        const WindowSurfaceIdentity identity = info.surface.GetIdentity();
        if (view_ != view || identity_ != identity || info.force_surface_recreate) {
            Detach();
            view_ = view;
            previous_layer_ = view.layer;
            previous_wants_layer_ = view.wantsLayer;
            layer_ = [CAMetalLayer layer];
            layer_.device = device_;
            layer_.framebufferOnly = NO; // Present copies the RHI framebuffer to the drawable.
            view.wantsLayer = YES;
            view.layer = layer_;
        }
        layer_.pixelFormat = pixel_format;
        layer_.frame = view.bounds;
        layer_.contentsScale = window.backingScaleFactor;
        layer_.drawableSize = CGSizeMake(info.size.x, info.size.y);
        source_ = info.surface.source;
        identity_ = identity;
        format = info.preferred_format;
        size = info.size;
        return true;
    }

    bool IsPresentationReady() const noexcept override { return layer_ != nil && source_ != nullptr; }
    WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override { return identity_; }

    id<CAMetalDrawable> NextDrawable() const { return [layer_ nextDrawable]; }

private:
    void Detach() {
        if (view_ != nil && view_.layer == layer_) {
            view_.layer = previous_layer_;
            view_.wantsLayer = previous_wants_layer_;
        }
        layer_ = nil;
        view_ = nil;
        previous_layer_ = nil;
        source_.reset();
        identity_ = {};
    }

    id<MTLDevice> device_;
    CAMetalLayer* layer_{nil};
    NSView* view_{nil};
    CALayer* previous_layer_{nil};
    bool previous_wants_layer_{false};
    WindowSurfaceSourceRef source_{};
    WindowSurfaceIdentity identity_{};
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
        if (!submit.signal_events.empty() || !submit.callbacks.empty() ||
            !submit.success_callbacks.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("submission synchronization, callbacks, or profiling");
        }
        // Validate the complete submit before encoding any GPU work. A later
        // unsupported command must not leave a partially executed submit.
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            if (command->Type() == Command::EType::QueueTransfer) {
                ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Graphics);
                continue;
            }
            if (command->Type() != Command::EType::ClearResource) {
                Unsupported("this graphics command");
            }
            has_gpu_work = true;
            const auto& clear = static_cast<const ClearResourceCmd&>(*command);
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
        for (const WaitEvent& event : submit.wait_events) Wait(event);
        if (!has_gpu_work) return {0, 0};

        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            if (command_buffer == nil) throw std::runtime_error("Cannot create Metal command buffer");
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::QueueTransfer) continue;
                const auto& clear = static_cast<const ClearResourceCmd&>(*command);
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
                    std::string([command_buffer.error.localizedDescription UTF8String] ?: "unknown GPU error"));
            }
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
    }

    ProfileData GetProfilerEntry() override { return {}; }

private:
    id<MTLCommandQueue> queue_;
};

class MetalCopyQueue final : public CopyQueue {
public:
    explicit MetalCopyQueue(id<MTLCommandQueue> queue) :
        queue_(queue), timeline_fence_(MoerNew(MetalFence)()) {}

    IOWaitEvt Execute(IOQueueSubmission&&) override { Unsupported("IO queue submission"); }
    IOWaitEvt Execute(CmdSubmit&& submit) override {
        std::lock_guard lock(mutex_);
        if (!submit.wait_events.empty() || !submit.signal_events.empty() ||
            !submit.callbacks.empty() || !submit.success_callbacks.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("copy submission synchronization, callbacks, or profiling");
        }
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            switch (command->Type()) {
                case Command::EType::QueueTransfer:
                    ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Copy);
                    break;
                case Command::EType::UploadTexture: {
                    has_gpu_work = true;
                    const auto& upload = static_cast<const UploadTextureCmd&>(*command);
                    auto* texture = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
                    const uint3 size = upload.Size();
                    if (texture == nullptr || upload.Format() != texture->GetFormat() ||
                        upload.MipLevel() != 0 || upload.ArrayLayer() != 0 ||
                        upload.Offset() != uint3{0, 0, 0} || size != texture->GetExtent() ||
                        upload.Data().size_bytes() != size_t(size.x) * size.y * PixelStride(upload.Format())) {
                        Unsupported("this texture upload layout");
                    }
                    break;
                }
                case Command::EType::UploadBuffer: {
                    has_gpu_work = true;
                    const auto& upload = static_cast<const UploadBufferCmd&>(*command);
                    auto* buffer = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
                    if (buffer == nullptr || upload.ByteSize() == 0 ||
                        upload.Offset() > buffer->GetByteSize() ||
                        upload.ByteSize() > buffer->GetByteSize() - upload.Offset() ||
                        upload.Data().size_bytes() != upload.ByteSize()) {
                        Unsupported("this buffer upload layout");
                    }
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
        if (!has_gpu_work) return {uint64(timeline_fence_.Get()), timeline_fence_->GetValue()};

        @autoreleasepool {
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
                    const auto& upload = static_cast<const UploadBufferCmd&>(*command);
                    auto* buffer = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
                    id<MTLBuffer> staging = [queue_.device
                        newBufferWithBytes:upload.Data().data()
                                   length:upload.ByteSize()
                                  options:MTLResourceStorageModeShared];
                    if (staging == nil) throw std::runtime_error("Cannot allocate Metal buffer staging");
                    [staging_buffers addObject:staging];
                    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer->Native()
                      destinationOffset:upload.Offset() size:upload.ByteSize()];
                    continue;
                }
                const auto& upload = static_cast<const UploadTextureCmd&>(*command);
                auto* texture = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
                const NSUInteger width = upload.Size().x;
                const NSUInteger height = upload.Size().y;
                const NSUInteger source_row_bytes = width * PixelStride(upload.Format());
                const NSUInteger staging_row_bytes = (source_row_bytes + 255) & ~NSUInteger(255);
                id<MTLBuffer> staging = [queue_.device
                    newBufferWithLength:staging_row_bytes * height
                                options:MTLResourceStorageModeShared];
                if (staging == nil) throw std::runtime_error("Cannot allocate Metal upload staging buffer");
                auto* destination = static_cast<uint8_t*>(staging.contents);
                const auto* source = reinterpret_cast<const uint8_t*>(upload.Data().data());
                for (NSUInteger row = 0; row < height; ++row) {
                    std::memcpy(destination + row * staging_row_bytes,
                                source + row * source_row_bytes, source_row_bytes);
                }
                [staging_buffers addObject:staging];
                [blit copyFromBuffer:staging sourceOffset:0
                  sourceBytesPerRow:staging_row_bytes
                sourceBytesPerImage:staging_row_bytes * height
                         sourceSize:MTLSizeMake(width, height, 1)
                          toTexture:texture->Native() destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
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
    }

private:
    id<MTLCommandQueue> queue_;
    std::mutex mutex_;
    FenceRef timeline_fence_;
    uint64 next_timeline_{0};
};

} // namespace

struct MetalDevice::Native {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    MetalCommandQueue graphics;
    MetalCopyQueue copy;

    Native(id<MTLDevice> metal_device, id<MTLCommandQueue> metal_queue) :
        device(metal_device), queue(metal_queue), graphics(metal_queue), copy(metal_queue) {}
};

MetalDevice::MetalDevice() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) throw std::runtime_error("No Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) throw std::runtime_error("Cannot create Metal command queue");
    native_ = std::make_unique<Native>(device, queue);
    LOG_INFO("[Metal] Device initialized: {}", [device.name UTF8String]);
}

MetalDevice::~MetalDevice() = default;

FenceRef MetalDevice::CreateFence() { return FenceRef(MoerNew(MetalFence)()); }
BufferRef MetalDevice::CreateBuffer(
    std::string_view name, uint count, uint stride, EBufferUsageFlags usage, EPixelFormat format
) {
    if (count == 0 || stride == 0) Unsupported("zero-sized buffers");
    const BufferInfo info(count, stride, usage, format);
    const MTLResourceOptions options =
        (usage & EBufferUsageFlags::CPU_VISIBLE) != EBufferUsageFlags::NONE ?
            MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
    id<MTLBuffer> buffer = [native_->device newBufferWithLength:info.size * info.stride
                                                       options:options];
    if (buffer == nil) throw std::runtime_error("Cannot allocate Metal buffer");
    auto result = BufferRef(MoerNew(MetalBuffer)(info, buffer));
    result->SetName(name);
    return result;
}
TextureRef MetalDevice::CreateTexture(std::string_view name, const TextureInfo& info) {
    if (info.dimension != ETextureDimension::TEX_2D || info.array_size != 1 ||
        info.num_mips != 1 || info.num_samples != 1 || info.extent.x <= 0 || info.extent.y <= 0) {
        Unsupported("this texture layout");
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:ToMetalFormat(info.format)
                                 width:info.extent.x height:info.extent.y mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead;
    if ((info.usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageRenderTarget;
    }
    if ((info.usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageShaderWrite;
    }
    id<MTLTexture> texture = [native_->device newTextureWithDescriptor:desc];
    if (texture == nil) throw std::runtime_error("Cannot allocate Metal texture");
    auto result = TextureRef(MoerNew(MetalTexture)(info, texture));
    result->SetName(name);
    return result;
}
BindlessArrayRef MetalDevice::CreateBindlessArray(uint) { Unsupported("bindless arrays"); }
RaytracingGeometryRef MetalDevice::CreateRaytracingGeometry(const RaytracingGeometryInfo&) {
    Unsupported("raytracing geometry");
}
RaytracingSceneRef MetalDevice::CreateRaytracingScene() { Unsupported("raytracing scene"); }
CommandQueue& MetalDevice::GetCommandQueue(EQueueType type) {
    if (type != EQueueType::Graphics) Unsupported("this command queue");
    return native_->graphics;
}
CopyQueue& MetalDevice::GetCopyQueue() { return native_->copy; }
RHIQueueTopology MetalDevice::GetQueueTopology() const {
    RHIQueueTopology topology{};
    topology.compute.available = false;
    return topology;
}
SwapchainRef MetalDevice::CreateSwapchain(const SwapchainCreateInfo& info) {
    auto result = SwapchainRef(MoerNew(MetalSwapchain)(native_->device, info));
    return result->IsPresentationReady() ? result : SwapchainRef{};
}
PipelineHandle MetalDevice::CreatePipeline(GfxPsoCreateInfo&&, PipelineShaderInfo&&) {
    Unsupported("graphics pipelines");
}
PipelineHandle MetalDevice::CreatePipeline(PipelineShaderInfo&&) { Unsupported("compute pipelines"); }
void MetalDevice::WaitIdle() { native_->graphics.Sync(); }

void* GetMetalNativeTexture(Texture* texture) noexcept {
    auto* metal_texture = dynamic_cast<MetalTexture*>(texture);
    return metal_texture == nullptr ? nullptr : (__bridge void*)metal_texture->Native();
}

void* GetMetalNativeBuffer(Buffer* buffer) noexcept {
    auto* metal_buffer = dynamic_cast<MetalBuffer*>(buffer);
    return metal_buffer == nullptr ? nullptr : (__bridge void*)metal_buffer->Native();
}

} // namespace Moer::Render
