#include "rhi/metal/MetalDevice.h"
#include "rhi/metal/MetalBindless.h"
#include "rhi/metal/MetalPipeline.h"
#include "rhi/metal/MetalQueue.h"
#include "rhi/metal/MetalResource.h"
#include "rhi/metal/MetalSwapchain.h"
#include "log/LogSystem.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace Moer::Render {
namespace {
void ValidateMetalBufferInfo(const BufferInfo& info) {
    if (info.size == 0 || info.stride == 0) Unsupported("zero-sized buffers");
    const bool texel_buffer =
        (info.usage & EBufferUsageFlags::TEXTURE_BUFFER) != EBufferUsageFlags::NONE;
    if (texel_buffer && (info.stride != sizeof(uint32_t) ||
                         (info.format != PF_UNDEFINED && info.format != PF_R32_UINT))) {
        Unsupported("this Metal texel buffer format");
    }
}

MTLResourceOptions ToMetalBufferResourceOptions(EBufferUsageFlags usage) {
    return (usage & EBufferUsageFlags::CPU_VISIBLE) != EBufferUsageFlags::NONE ?
        MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
}

struct MetalBufferLayout {
    NSUInteger byte_size;
    NSUInteger texel_width = 0;
    NSUInteger texel_height = 0;
};

MetalBufferLayout CalculateMetalBufferLayout(id<MTLDevice> device, const BufferInfo& info) {
    if ((info.usage & EBufferUsageFlags::TEXTURE_BUFFER) == EBufferUsageFlags::NONE) {
        return {info.size * info.stride};
    }
    const uint count = static_cast<uint>(info.size);
    const NSUInteger linear_alignment =
        [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Uint];
    const NSUInteger texel_width =
        count > 4096 ? 4096 : std::max<NSUInteger>(count, linear_alignment / sizeof(uint32_t));
    const NSUInteger texel_height = (count + 4095) / 4096;
    return {texel_width * texel_height * sizeof(uint32_t), texel_width, texel_height};
}

id<MTLTexture> CreateMetalTexelBufferTexture(id<MTLBuffer> buffer, const MetalBufferLayout& layout) {
    if (layout.texel_width == 0) return nil;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                      width:layout.texel_width height:layout.texel_height mipmapped:NO];
    descriptor.storageMode = buffer.storageMode;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:0
                                                bytesPerRow:layout.texel_width * sizeof(uint32_t)];
    if (texture == nil) Unsupported("this Metal texel buffer allocation");
    return texture;
}

void ValidateMetalTextureLayout(const TextureInfo& info) {
    const bool is_2d = info.dimension == ETextureDimension::TEX_2D && info.array_size == 1;
    const bool is_2d_array = info.dimension == ETextureDimension::TEX_2D_ARRAY && info.array_size > 0;
    const bool is_cube = info.dimension == ETextureDimension::TEX_CUBE &&
                         info.array_size == 6 && info.extent.x == info.extent.y;
    if ((!is_2d && !is_2d_array && !is_cube) || info.depth != 1 ||
        info.num_mips < 1 || info.num_samples != 1 || info.extent.x <= 0 || info.extent.y <= 0) {
        throw std::runtime_error(
            "Metal RHI has not implemented texture layout: dimension=" +
            std::to_string(static_cast<uint>(info.dimension)) +
            " extent=" + std::to_string(info.extent.x) + "x" + std::to_string(info.extent.y) +
            " array=" + std::to_string(info.array_size) +
            " mips=" + std::to_string(info.num_mips) +
            " samples=" + std::to_string(info.num_samples)
        );
    }
}

void ValidateDepthTextureUsage(EPixelFormat format, ETextureUsageFlags usage) {
    const bool is_depth = format == PF_D16_UNORM || format == PF_D32_SFLOAT ||
                          format == PF_D32_SFLOAT_S8_UINT;
    if ((is_depth && (usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (!is_depth && (usage & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (is_depth && (usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED)) {
        Unsupported("this texture format and usage combination");
    }
}

MTLTextureUsage ToMetalTextureUsage(ETextureUsageFlags usage) {
    MTLTextureUsage native_usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    const auto attachment_usage =
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT;
    if ((usage & attachment_usage) != ETextureUsageFlags::UNDEFINED) {
        native_usage |= MTLTextureUsageRenderTarget;
    }
    if ((usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED) {
        native_usage |= MTLTextureUsageShaderWrite;
    }
    return native_usage;
}

MTLTextureDescriptor* CreateMetalTextureDescriptor(const TextureInfo& info, MTLPixelFormat native_format) {
    MTLTextureDescriptor* desc = info.dimension == ETextureDimension::TEX_CUBE ?
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:native_format
                                                              size:info.extent.x
                                                         mipmapped:info.num_mips > 1] :
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:native_format
                                                            width:info.extent.x
                                                           height:info.extent.y
                                                        mipmapped:info.num_mips > 1];
    if (info.dimension == ETextureDimension::TEX_2D_ARRAY) {
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = info.array_size;
    }
    desc.mipmapLevelCount = info.num_mips;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = ToMetalTextureUsage(info.usage);
    return desc;
}
} // namespace

struct MetalDevice::Native {
    id<MTLDevice> device;
    MetalGraphicsQueuePtr graphics;
    MetalCopyQueuePtr copy;

    Native(id<MTLDevice> metal_device, id<MTLCommandQueue> metal_queue) :
        device(metal_device),
        graphics(CreateMetalGraphicsQueue(metal_queue)),
        copy(CreateMetalCopyQueue(metal_queue)) {}
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
    const BufferInfo info(count, stride, usage, format);
    ValidateMetalBufferInfo(info);
    const MTLResourceOptions options = ToMetalBufferResourceOptions(usage);
    const MetalBufferLayout layout = CalculateMetalBufferLayout(native_->device, info);

    id<MTLBuffer> buffer = [native_->device newBufferWithLength:layout.byte_size
                                                       options:options];
    if (buffer == nil) throw std::runtime_error("Cannot allocate Metal buffer");
    id<MTLTexture> texel_texture = CreateMetalTexelBufferTexture(buffer, layout);
    auto result = BufferRef(MoerNew(MetalBuffer)(info, buffer, texel_texture));
    result->SetName(name);
    return result;
}
TextureRef MetalDevice::CreateTexture(std::string_view name, const TextureInfo& info) {
    ValidateMetalTextureLayout(info);
    const MTLPixelFormat native_format = ToMetalFormat(info.format);
    ValidateDepthTextureUsage(info.format, info.usage);

    MTLTextureDescriptor* desc = CreateMetalTextureDescriptor(info, native_format);
    id<MTLTexture> texture = [native_->device newTextureWithDescriptor:desc];
    if (texture == nil) throw std::runtime_error("Cannot allocate Metal texture");
    auto result = TextureRef(MoerNew(MetalTexture)(info, texture));
    result->SetName(name);
    return result;
}
BindlessArrayRef MetalDevice::CreateBindlessArray(uint capacity) {
    return BindlessArrayRef(MoerNew(MetalBindlessArray)(native_->device, capacity));
}
RaytracingGeometryRef MetalDevice::CreateRaytracingGeometry(const RaytracingGeometryInfo&) {
    Unsupported("raytracing geometry");
}
RaytracingSceneRef MetalDevice::CreateRaytracingScene() { Unsupported("raytracing scene"); }
CommandQueue& MetalDevice::GetCommandQueue(EQueueType type) {
    if (type != EQueueType::Graphics) Unsupported("this command queue");
    return *native_->graphics;
}
CopyQueue& MetalDevice::GetCopyQueue() { return *native_->copy; }
RHIQueueTopology MetalDevice::GetQueueTopology() const {
    RHIQueueTopology topology{};
    topology.compute.available = false;
    return topology;
}
SwapchainRef MetalDevice::CreateSwapchain(const SwapchainCreateInfo& info) {
    auto result = SwapchainRef(MoerNew(MetalSwapchain)(native_->device, info));
    return result->IsPresentationReady() ? result : SwapchainRef{};
}
PipelineHandle MetalDevice::CreatePipeline(GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info) {
    return CreateMetalGraphicsPipeline(native_->device, std::move(create_info), std::move(shader_info));
}
PipelineHandle MetalDevice::CreatePipeline(PipelineShaderInfo&& shader_info) {
    return CreateMetalComputePipeline(native_->device, std::move(shader_info));
}
void MetalDevice::WaitIdle() { native_->graphics->Sync(); }
bool MetalDevice::SupportsTessellation() const {
    return [native_->device supportsFamily:MTLGPUFamilyApple3] ||
           [native_->device supportsFamily:MTLGPUFamilyMac1];
}
uint32_t MetalDevice::GetMaxTessellationFactor() const {
    return SupportsTessellation() ? 64 : 0;
}

} // namespace Moer::Render
