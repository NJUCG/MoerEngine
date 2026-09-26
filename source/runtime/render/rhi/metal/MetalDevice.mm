#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/metal/MetalDevice.h"
#include "log/LogSystem.h"

#include <stdexcept>
#include <string>

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
        default:
            Unsupported("this color format");
    }
}

class MetalTexture final : public Texture {
public:
    MetalTexture(const TextureInfo& info, id<MTLTexture> texture) : Texture(info), texture_(texture) {}

    id<MTLTexture> Native() const noexcept { return texture_; }

    uint GetMipByteSize(uint mip) const override {
        if (mip != 0) Unsupported("texture mip sizes");
        return GetWidth() * GetHeight() * 4;
    }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        texture_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLTexture> texture_;
};

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

    void Wait(WaitEvent) override { Unsupported("queue wait events"); }
    WaitEvent Execute(CmdSubmit&& submit) override {
        if (!submit.wait_events.empty() || !submit.signal_events.empty() ||
            !submit.callbacks.empty() || !submit.success_callbacks.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("submission synchronization, callbacks, or profiling");
        }
        // Validate the complete submit before encoding any GPU work. A later
        // unsupported command must not leave a partially executed submit.
        for (const auto& command : submit.cmds) {
            if (command->Type() != Command::EType::ClearResource) {
                Unsupported("this graphics command");
            }
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
        if (submit.cmds.empty()) return {0, 0};

        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            if (command_buffer == nil) throw std::runtime_error("Cannot create Metal command buffer");
            for (const auto& command : submit.cmds) {
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
    IOWaitEvt Execute(IOQueueSubmission&&) override { Unsupported("IO queue submission"); }
    IOWaitEvt Execute(CmdSubmit&&) override { Unsupported("copy queue submission"); }
    void CopyFrom(BufferView, BufferView) override { Unsupported("buffer copy"); }
    void CopyFrom(TextureView, TextureView) override { Unsupported("texture copy"); }
    void CopyFrom(TextureView, BufferView) override { Unsupported("texture readback"); }
    void CopyFrom(BufferView, TextureView) override { Unsupported("texture upload"); }
    void CopyFrom(std::span<byte>, BufferView) override { Unsupported("buffer upload"); }
    void CopyFrom(std::span<byte>, TextureView) override { Unsupported("texture upload"); }
    FenceRef GetFenceHandle() override { Unsupported("copy fence"); }
    void Sync(uint64) override { Unsupported("copy queue synchronization"); }
};

} // namespace

struct MetalDevice::Native {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    MetalCommandQueue graphics;
    MetalCopyQueue copy;

    Native(id<MTLDevice> metal_device, id<MTLCommandQueue> metal_queue) :
        device(metal_device), queue(metal_queue), graphics(metal_queue) {}
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

FenceRef MetalDevice::CreateFence() { Unsupported("fences"); }
BufferRef MetalDevice::CreateBuffer(std::string_view, uint, uint, EBufferUsageFlags, EPixelFormat) {
    Unsupported("buffers");
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
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
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
    topology.copy.available = false;
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

} // namespace Moer::Render
