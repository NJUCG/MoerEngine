#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/RHI.h"
#include "rhi/RHIExecutor.h"
#include "rhi/metal/MetalDevice.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class SmokeWindowSource final : public Moer::Render::WindowSurfaceSource {
public:
    explicit SmokeWindowSource(GLFWwindow* window) : window_(window) {}

    Moer::Render::WindowSurfaceIdentity GetIdentity() const noexcept override {
        return {Moer::Render::EWindowSystemType::GLFW,
                reinterpret_cast<uintptr_t>(window_), 0, 1};
    }

    Moer::Render::WindowNativeHandle GetNativeWindow() const noexcept override {
        return {Moer::Render::EWindowSystemType::GLFW,
                reinterpret_cast<uintptr_t>(window_), 0};
    }

private:
    GLFWwindow* window_;
};

void CheckRasterTextureFormats() {
    using namespace Moer::Render;
    struct FormatCase {
        EPixelFormat format;
        MTLPixelFormat native_format;
        ETextureUsageFlags usage;
        uint32_t pixel_stride;
    };
    const FormatCase formats[] = {
        {PF_R8_UNORM, MTLPixelFormatR8Unorm, ETextureUsageFlags::COLOR_ATTACHMENT, 1},
        {PF_A2R10G10B10_UNORM_PACK32, MTLPixelFormatBGR10A2Unorm,
         ETextureUsageFlags::COLOR_ATTACHMENT, 4},
        {PF_R16G16B16A16_SFLOAT, MTLPixelFormatRGBA16Float,
         ETextureUsageFlags::COLOR_ATTACHMENT, 8},
        {PF_D16_UNORM, MTLPixelFormatDepth16Unorm,
         ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT, 2},
        {PF_D32_SFLOAT, MTLPixelFormatDepth32Float,
         ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT, 4},
        {PF_D32_SFLOAT_S8_UINT, MTLPixelFormatDepth32Float_Stencil8,
         ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT, 8},
    };
    for (const FormatCase& item : formats) {
        TextureRef texture = RenderDevice::Get().CreateTexture(
            Extent2D(4, 4), item.format, item.usage | ETextureUsageFlags::SAMPLED
        );
        id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get());
        if (native == nil || native.pixelFormat != item.native_format ||
            (native.usage & MTLTextureUsageRenderTarget) == 0 ||
            texture->GetMipByteSize(0) != 16 * item.pixel_stride) {
            throw std::runtime_error("Metal raster texture format allocation failed");
        }
    }
    bool rejected_invalid_usage = false;
    try {
        RenderDevice::Get().CreateTexture(
            Extent2D(4, 4), PF_D32_SFLOAT, ETextureUsageFlags::COLOR_ATTACHMENT
        );
    } catch (const std::runtime_error&) {
        rejected_invalid_usage = true;
    }
    if (!rejected_invalid_usage) {
        throw std::runtime_error("Metal accepted a depth texture as a color attachment");
    }
    TextureRef packed = RenderDevice::Get().CreateTexture(
        Extent2D(4, 4), PF_A2R10G10B10_UNORM_PACK32,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC
    );
    CommandList clear(EQueueType::Graphics);
    clear.ClearResource(packed->GetView(), Moer::float4{1.0f, 0.0f, 0.0f, 1.0f});
    RHIExecutor::Get().Submit(EQueueType::Graphics, clear.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(packed.Get());
    id<MTLCommandQueue> queue = [native.device newCommandQueue];
    id<MTLBuffer> readback = [native.device newBufferWithLength:256 * 4
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:256
       destinationBytesPerImage:256 * 4];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    uint32_t first_pixel = 0;
    std::memcpy(&first_pixel, readback.contents, sizeof(first_pixel));
    if (command.status != MTLCommandBufferStatusCompleted || first_pixel != 0xfff00000u) {
        throw std::runtime_error("Metal A2R10G10B10 channel packing is incorrect");
    }
    std::cout << "raster color/depth texture format allocation: success" << std::endl;
}

void CheckGraphicsPipeline() {
    using namespace Moer::Render;
    constexpr std::string_view source = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VertexInput {
            float2 position [[attribute(0)]];
            float4 color [[attribute(1)]];
        };
        struct VertexOutput {
            float4 position [[position]];
            float4 color;
        };
        vertex VertexOutput vertex_main(VertexInput input [[stage_in]]) {
            return {float4(input.position, 0.0, 1.0), input.color};
        }
        fragment float4 fragment_main(VertexOutput input [[stage_in]]) {
            return input.color;
        }
    )";
    std::vector<Moer::uint8> code(source.begin(), source.end());
    SingleShaderInfo vertex{.entry_point = "vertex_main", .shader_data = code,
                            .shader_type = EShaderType::ST_VERTEX};
    SingleShaderInfo fragment{.entry_point = "fragment_main", .shader_data = code,
                              .shader_type = EShaderType::ST_FRAGMENT};
    PipelineShaderInfo shaders{.shader_group = ShaderVsPs{vertex, fragment}};
    VertexStream stream;
    stream.EmplacePerVertex({Moer::Render::VertexElement(PF_R32G32_SFLOAT),
                             Moer::Render::VertexElement(PF_R32G32B32A32_SFLOAT)});
    GfxPsoCreateInfo info(
        RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), std::move(stream),
        {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(PF_B8G8R8A8_UNORM)}
    );
    PipelineHandle pipeline = RenderDevice::Get().CreatePipeline(std::move(info), std::move(shaders));
    if (!pipeline.IsValid()) throw std::runtime_error("Metal graphics pipeline was not created");
    MoerDelete(reinterpret_cast<PipelineState*>(pipeline.handle));
    std::cout << "native Metal vertex/fragment pipeline with vertex input and blend: success"
              << std::endl;
}

void CheckGraphicsUploads() {
    using namespace Moer::Render;
    constexpr NSUInteger width = 7;
    constexpr NSUInteger height = 5;
    std::vector<uint8_t> pixels(width * height * 4);
    std::vector<uint8_t> values(20);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 13);
    for (size_t i = 0; i < values.size(); ++i) values[i] = uint8_t(i * 9);
    TextureRef texture = RenderDevice::Get().CreateTexture(
        Extent2D(width, height), PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
    );
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal graphics upload buffer", BufferInfo{64, 1, EBufferUsageFlags::TRANSFER_DST}
    );
    CommandList upload(EQueueType::Graphics);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(pixels.data()), pixels.size()),
        texture->GetView()
    );
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(values.data()), values.size()),
        buffer->GetView(8, values.size())
    );
    RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    id<MTLTexture> native_texture = (__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get());
    id<MTLBuffer> native_buffer = (__bridge id<MTLBuffer>)GetMetalNativeBuffer(buffer.Get());
    constexpr NSUInteger row_bytes = 256;
    id<MTLBuffer> texture_readback = [native_texture.device
        newBufferWithLength:row_bytes * height options:MTLResourceStorageModeShared];
    id<MTLBuffer> buffer_readback = [native_texture.device
        newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [native_texture.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native_texture sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:texture_readback destinationOffset:0 destinationBytesPerRow:row_bytes
       destinationBytesPerImage:row_bytes * height];
    [blit copyFromBuffer:native_buffer sourceOffset:0
               toBuffer:buffer_readback destinationOffset:0 size:64];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        throw std::runtime_error("Metal graphics upload readback failed");
    }
    const auto* texture_bytes = static_cast<const uint8_t*>(texture_readback.contents);
    for (NSUInteger row = 0; row < height; ++row) {
        if (std::memcmp(texture_bytes + row * row_bytes, pixels.data() + row * width * 4,
                        width * 4) != 0) {
            throw std::runtime_error("Metal graphics texture upload differs from input");
        }
    }
    if (std::memcmp(static_cast<const uint8_t*>(buffer_readback.contents) + 8,
                    values.data(), values.size()) != 0) {
        throw std::runtime_error("Metal graphics buffer upload differs from input");
    }
    std::cout << "graphics queue texture and buffer upload/readback: success" << std::endl;
}

Moer::Render::TextureRef ClearAndCheckTexture(int width, int height) {
    using namespace Moer::Render;
    TextureRef texture = RenderDevice::Get().CreateTexture(
        Extent2D(width, height), PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC
    );
    id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get());
    if (native == nil) throw std::runtime_error("Metal texture bridge failed");
    id<MTLCommandQueue> queue = [native.device newCommandQueue];

    CommandList clear(EQueueType::Graphics);
    clear.ClearResource(texture->GetView(), Moer::float4{0.08f, 0.4f, 0.75f, 1.0f});
    RHIExecutor::Get().Submit(EQueueType::Graphics, clear.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const NSUInteger row_bytes = ((static_cast<NSUInteger>(width) * 4 + 255) / 256) * 256;
    id<MTLBuffer> readback = [native.device newBufferWithLength:row_bytes * height
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:row_bytes
       destinationBytesPerImage:row_bytes * height];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    const auto* pixel = static_cast<const uint8_t*>(readback.contents);
    if (command.status != MTLCommandBufferStatusCompleted ||
        pixel[0] < 15 || pixel[0] > 30 ||
        pixel[1] < 95 || pixel[1] > 110 ||
        pixel[2] < 185 || pixel[2] > 200 || pixel[3] != 255) {
        throw std::runtime_error("Metal clear readback has an unexpected color");
    }
    std::cout << "clear/readback " << width << "x" << height << ": "
              << int(pixel[0]) << "," << int(pixel[1]) << ","
              << int(pixel[2]) << "," << int(pixel[3]) << std::endl;
    return texture;
}

void UploadAndCheckTexture(
    EPixelFormat format,
    ETextureUsageFlags usage,
    std::span<const uint8_t> input,
    size_t pixel_stride
) {
    using namespace Moer::Render;
    constexpr int width = 7;
    constexpr int height = 5;
    if (input.size_bytes() != width * height * pixel_stride) {
        throw std::runtime_error("Metal upload test input has the wrong size");
    }
    TextureRef texture = RenderDevice::Get().CreateTexture(Extent2D(width, height), format, usage);
    FenceRef copy_fence = RenderDevice::Get().GetCopyQueue().GetFenceHandle();
    const uint64_t before_upload = copy_fence->GetValue();
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        texture->GetView()
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint64_t after_upload = copy_fence->GetValue();
    if (after_upload <= before_upload ||
        !copy_fence->WaitSubmitted(after_upload) || copy_fence->IsRejected(after_upload)) {
        throw std::runtime_error("Metal copy fence did not complete the upload");
    }

    TextureRef copied = RenderDevice::Get().CreateTexture(Extent2D(width, height), format, usage);
    CommandList copy(EQueueType::Copy);
    copy.CopyFrom(texture->GetView(), copied->GetView());
    RHIExecutor::Get().Submit(EQueueType::Copy, copy.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint64_t after_copy = copy_fence->GetValue();
    if (after_copy <= after_upload ||
        !copy_fence->WaitSubmitted(after_copy) || copy_fence->IsRejected(after_copy)) {
        throw std::runtime_error("Metal copy fence did not complete the texture copy");
    }

    Moer::Array<ExportTexture> exports;
    exports.emplace_back(copied->GetView(), ETextureState::SAMPLE);
    CommandList export_command(EQueueType::Copy);
    export_command.ExportResourcesToQueue(EQueueType::Graphics, std::move(exports), {});
    RHIExecutor::Get().Submit(EQueueType::Copy, export_command.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    Moer::Array<ImportTexture> imports;
    imports.emplace_back(copied->GetView(), ETextureState::SAMPLE);
    CommandList import_command(EQueueType::Graphics);
    import_command.ImportResourcesFromQueue(EQueueType::Copy, std::move(imports), {});
    RHIExecutor::Get().Submit(
        EQueueType::Graphics, import_command.Submit().Wait(copy_fence, after_copy)
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(copied.Get());
    if (native == nil) throw std::runtime_error("Metal upload texture bridge failed");
    const NSUInteger row_bytes = (width * pixel_stride + 255) & ~NSUInteger(255);
    id<MTLBuffer> readback = [native.device newBufferWithLength:row_bytes * height
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [native.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:row_bytes
       destinationBytesPerImage:row_bytes * height];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        throw std::runtime_error("Metal upload readback failed");
    }
    const auto* output = static_cast<const uint8_t*>(readback.contents);
    for (int y = 0; y < height; ++y) {
        for (size_t x = 0; x < width * pixel_stride; ++x) {
            if (output[y * row_bytes + x] != input[y * width * pixel_stride + x]) {
                throw std::runtime_error("Metal texture upload readback differs from input");
            }
        }
    }
    std::cout << "texture upload/copy/transfer/readback " << width << "x" << height
              << " stride " << pixel_stride << ": success" << std::endl;
}

void CheckBufferUpload() {
    using namespace Moer::Render;
    constexpr size_t offset = 16;
    std::vector<uint8_t> input(20);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i * 11);
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal smoke buffer", BufferInfo{64, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        buffer->GetView(offset, input.size())
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    BufferRef copied = RenderDevice::Get().CreateBuffer(
        "Metal smoke copied buffer", BufferInfo{64, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    constexpr size_t destination_offset = 8;
    CommandList copy(EQueueType::Copy);
    copy.CopyFrom(buffer->GetView(offset, input.size()),
                  copied->GetView(destination_offset, input.size()));
    RHIExecutor::Get().Submit(EQueueType::Copy, copy.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    id<MTLBuffer> native = (__bridge id<MTLBuffer>)GetMetalNativeBuffer(copied.Get());
    if (native == nil) throw std::runtime_error("Metal buffer bridge failed");
    id<MTLBuffer> readback = [native.device newBufferWithLength:input.size()
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [native.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:native sourceOffset:destination_offset toBuffer:readback
      destinationOffset:0 size:input.size()];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted ||
        !std::equal(input.begin(), input.end(), static_cast<const uint8_t*>(readback.contents))) {
        throw std::runtime_error("Metal buffer upload readback differs from input");
    }
    std::cout << "buffer upload/copy/readback at offsets " << offset << "/"
              << destination_offset << ": success" << std::endl;
}

void CheckCopyCompletionCallbacks() {
    using namespace Moer::Render;
    auto callback_state = std::make_shared<std::atomic<int>>(0);
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal callback smoke buffer", BufferInfo{32, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    std::vector<uint8_t> input(32, 0x5a);
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        buffer->GetView()
    );
    upload.AddCallback([callback_state] {
        callback_state->fetch_add(1);
        CommandList nested(EQueueType::Copy);
        nested.AddSuccessCallback([callback_state] { callback_state->fetch_add(10); });
        RHIExecutor::Get().Submit(EQueueType::Copy, nested.Submit());
    });
    upload.AddSuccessCallback([callback_state] {
        if (callback_state->load() == 1) callback_state->fetch_add(100);
    });
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (callback_state->load() < 101) {
        throw std::runtime_error("Metal copy Sync returned before the outer callbacks finished");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callback_state->load() != 111 && std::chrono::steady_clock::now() < deadline) {
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (callback_state->load() != 111) {
        throw std::runtime_error("Metal copy completion callbacks did not run in order");
    }
    std::cout << "copy completion callbacks and reentrant submit: success" << std::endl;
}

void CheckBindlessTables() {
    using namespace Moer::Render;
    BindlessArrayRef array = RenderDevice::Get().CreateBindlessArray(8);
    id<MTLBuffer> indices = (__bridge id<MTLBuffer>)GetMetalNativeBindlessIndexBuffer(array.Get());
    id<MTLBuffer> buffers = (__bridge id<MTLBuffer>)GetMetalNativeBindlessArgumentBuffer(array.Get(), 1);
    id<MTLBuffer> textures = (__bridge id<MTLBuffer>)GetMetalNativeBindlessArgumentBuffer(array.Get(), 2);
    id<MTLBuffer> samplers = (__bridge id<MTLBuffer>)GetMetalNativeBindlessArgumentBuffer(array.Get(), 3);
    if (indices == nil || buffers == nil || textures == nil || samplers == nil ||
        GetMetalNativeBindlessArgumentBuffer(array.Get(), 4) != nullptr ||
        *static_cast<const uint64_t*>(buffers.contents) != indices.gpuAddress) {
        throw std::runtime_error("Metal bindless argument tables are incomplete");
    }
    TextureRef texture = RenderDevice::Get().CreateTexture(
        Extent2D(2, 2), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED
    );
    std::vector<uint8_t> pixels(2 * 2 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 64; pixels[i + 1] = 128; pixels[i + 2] = 192; pixels[i + 3] = 255;
    }
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(pixels.data()), pixels.size()),
        texture->GetView()
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const uint handle = array->AllocateTexture(texture->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
    if (handle != 1 || static_cast<const uint32_t*>(indices.contents)[handle] != 0 ||
        static_cast<const uint64_t*>(textures.contents)[handle] != 0) {
        throw std::runtime_error("Metal bindless allocation became visible before its update command");
    }
    CommandList update(EQueueType::Graphics);
    update.UpdateBindlessArray(array);
    RHIExecutor::Get().Submit(EQueueType::Graphics, update.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint sampler_index = uint(SF_Num) * uint(SAM_CLAMP_TO_EDGE) + uint(SF_LINEAR);
    if (static_cast<const uint32_t*>(indices.contents)[handle] != ((handle << 8) | sampler_index) ||
        static_cast<const uint64_t*>(textures.contents)[handle] !=
            ((__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get())).gpuResourceID._impl ||
        static_cast<const uint64_t*>(samplers.contents)[sampler_index] == 0) {
        throw std::runtime_error("Metal bindless texture update did not publish the GPU descriptors");
    }

    // Match the SPIRV-Cross runtime-array argument-buffer ABI and prove that
    // the GPU can follow the indirect texture/sampler handle, not just that
    // the CPU wrote plausible descriptor bytes.
    static constexpr const char* shader_source = R"MSL(
#include <metal_stdlib>
using namespace metal;
template<typename T> struct spvDescriptor { T value; };
template<typename T> struct spvDescriptorArray {
    spvDescriptorArray(const device spvDescriptor<T>* p) : ptr(&p->value) {}
    const device T& operator[](size_t i) const { return ptr[i]; }
    const device T* ptr;
};
struct Set1 { const device uint* handles [[id(0)]]; };
struct Set2 { spvDescriptor<texture2d<float>> textures [[id(0)]][1]; };
struct Set3 { spvDescriptor<sampler> samplers [[id(0)]][1]; };
kernel void sample_bindless(
    device float4* output [[buffer(0)]],
    const device Set1& set1 [[buffer(1)]],
    const device Set2& set2 [[buffer(2)]],
    const device Set3& set3 [[buffer(3)]]) {
    uint packed = set1.handles[1];
    spvDescriptorArray<texture2d<float>> texture_table{set2.textures};
    spvDescriptorArray<sampler> sampler_table{set3.samplers};
    output[0] = texture_table[packed >> 8].sample(
        sampler_table[packed & 255], float2(0.5, 0.5));
}
)MSL";
    id<MTLDevice> device = indices.device;
    NSError* shader_error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library = [device newLibraryWithSource:
        [NSString stringWithUTF8String:shader_source] options:options error:&shader_error];
    if (library == nil) {
        throw std::runtime_error("Cannot compile Metal bindless shader: " +
            std::string(shader_error.localizedDescription.UTF8String ?: "unknown error"));
    }
    id<MTLFunction> function = [library newFunctionWithName:@"sample_bindless"];
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&shader_error];
    if (pipeline == nil) {
        throw std::runtime_error("Cannot create Metal bindless pipeline: " +
            std::string(shader_error.localizedDescription.UTF8String ?: "unknown error"));
    }
    id<MTLBuffer> sampled = [device newBufferWithLength:sizeof(float) * 4
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> native_queue = [device newCommandQueue];
    id<MTLCommandBuffer> native_command = [native_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [native_command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:sampled offset:0 atIndex:0];
    [encoder setBuffer:buffers offset:0 atIndex:1];
    [encoder setBuffer:textures offset:0 atIndex:2];
    [encoder setBuffer:samplers offset:0 atIndex:3];
    [encoder useResource:(__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get())
                usage:MTLResourceUsageRead];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [native_command commit];
    [native_command waitUntilCompleted];
    const float* color = static_cast<const float*>(sampled.contents);
    if (native_command.status != MTLCommandBufferStatusCompleted ||
        std::abs(color[0] - 64.0f / 255.0f) > 0.01f ||
        std::abs(color[1] - 128.0f / 255.0f) > 0.01f ||
        std::abs(color[2] - 192.0f / 255.0f) > 0.01f ||
        std::abs(color[3] - 1.0f) > 0.01f) {
        throw std::runtime_error("Metal bindless GPU texture sampling did not match the upload");
    }

    array->UnbindTexture(handle);
    CommandList release(EQueueType::Graphics);
    release.UpdateBindlessArray(array);
    RHIExecutor::Get().Submit(EQueueType::Graphics, release.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (static_cast<const uint32_t*>(indices.contents)[handle] != 0 ||
        static_cast<const uint64_t*>(textures.contents)[handle] != 0) {
        throw std::runtime_error("Metal bindless texture unbind left a GPU descriptor active");
    }
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal bindless buffer smoke", BufferInfo{32, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    const uint32_t input_value = 0x12345678u;
    CommandList fill_buffer(EQueueType::Copy);
    fill_buffer.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(&input_value), sizeof(input_value)),
        BufferView(buffer.Get(), 8, sizeof(input_value), 1)
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, fill_buffer.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint buffer_handle = array->AllocateBuffer(BufferView(buffer.Get(), 8, 8, 1));
    if (buffer_handle != handle) throw std::runtime_error("Metal bindless slot was not recycled");
    {
        CommandList dropped(EQueueType::Graphics);
        dropped.UpdateBindlessArray(array);
    }
    CommandList retry(EQueueType::Graphics);
    retry.UpdateBindlessArray(array);
    RHIExecutor::Get().Submit(EQueueType::Graphics, retry.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (static_cast<const uint32_t*>(indices.contents)[buffer_handle] != buffer_handle ||
        static_cast<const uint64_t*>(buffers.contents)[buffer_handle + 1] !=
            ((__bridge id<MTLBuffer>)GetMetalNativeBuffer(buffer.Get())).gpuAddress + 8) {
        throw std::runtime_error("Metal bindless buffer update was not recovered after a dropped command");
    }
    static constexpr const char* buffer_shader_source = R"MSL(
#include <metal_stdlib>
using namespace metal;
template<typename T> struct spvDescriptor { T value; };
template<typename T> struct spvDescriptorArray {
    spvDescriptorArray(const device spvDescriptor<T>* p) : ptr(&p->value) {}
    const device T& operator[](size_t i) const { return ptr[i]; }
    const device T* ptr;
};
struct Set1 {
    const device uint* handles [[id(0)]];
    spvDescriptor<const device uint*> buffers [[id(1)]][1];
};
kernel void read_bindless_buffer(device uint* output [[buffer(0)]],
                                 const device Set1& set1 [[buffer(1)]]) {
    spvDescriptorArray<const device uint*> table{set1.buffers};
    output[0] = table[set1.handles[1]][0];
}
)MSL";
    id<MTLLibrary> buffer_library = [device newLibraryWithSource:
        [NSString stringWithUTF8String:buffer_shader_source] options:options error:&shader_error];
    if (buffer_library == nil) {
        throw std::runtime_error("Cannot compile Metal bindless buffer shader: " +
            std::string(shader_error.localizedDescription.UTF8String ?: "unknown error"));
    }
    id<MTLFunction> buffer_function = [buffer_library newFunctionWithName:@"read_bindless_buffer"];
    id<MTLComputePipelineState> buffer_pipeline =
        [device newComputePipelineStateWithFunction:buffer_function error:&shader_error];
    if (buffer_pipeline == nil) {
        throw std::runtime_error("Cannot create Metal bindless buffer pipeline: " +
            std::string(shader_error.localizedDescription.UTF8String ?: "unknown error"));
    }
    id<MTLBuffer> buffer_readback = [device newBufferWithLength:sizeof(uint32_t)
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> buffer_command = [native_queue commandBuffer];
    id<MTLComputeCommandEncoder> buffer_encoder = [buffer_command computeCommandEncoder];
    [buffer_encoder setComputePipelineState:buffer_pipeline];
    [buffer_encoder setBuffer:buffer_readback offset:0 atIndex:0];
    [buffer_encoder setBuffer:buffers offset:0 atIndex:1];
    [buffer_encoder useResource:(__bridge id<MTLBuffer>)GetMetalNativeBuffer(buffer.Get())
                         usage:MTLResourceUsageRead];
    [buffer_encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [buffer_encoder endEncoding];
    [buffer_command commit];
    [buffer_command waitUntilCompleted];
    if (buffer_command.status != MTLCommandBufferStatusCompleted ||
        *static_cast<const uint32_t*>(buffer_readback.contents) != input_value) {
        throw std::runtime_error("Metal bindless GPU buffer read did not match the upload");
    }
    array->UnbindBuffer(buffer_handle);
    CommandList release_buffer(EQueueType::Graphics);
    release_buffer.UpdateBindlessArray(array);
    RHIExecutor::Get().Submit(EQueueType::Graphics, release_buffer.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (static_cast<const uint64_t*>(buffers.contents)[buffer_handle + 1] != 0) {
        throw std::runtime_error("Metal bindless buffer unbind left a GPU descriptor active");
    }
    std::cout << "bindless texture/buffer GPU access, update, drop, and reuse: success" << std::endl;
}

void Present(Moer::Render::SwapchainRef swapchain, Moer::Render::TextureRef texture) {
    using namespace Moer::Render;
    auto receipt = std::make_shared<PresentReceipt>();
    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Present(
        swapchain, texture->GetView(), receipt
    );
    const auto result = receipt->WaitForSubmission();
    if (!result.resolved || !result.submitted) {
        throw std::runtime_error("Metal Present was rejected");
    }
    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        GLFWwindow* window = nullptr;
        bool task_system_initialized = false;
        try {
            if (!glfwInit()) throw std::runtime_error("glfwInit failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(640, 480, "MoerEngine Metal RHI smoke", nullptr, nullptr);
            if (window == nullptr) throw std::runtime_error("glfwCreateWindow failed");
            NSWindow* native_window = glfwGetCocoaWindow(window);
            std::cout << "smoke window id: " << native_window.windowNumber << std::endl;

            using namespace Moer::Render;
            Moer::TaskSystem::Init();
            task_system_initialized = true;
            RenderDevice::Init(DeviceInitInfo{.rhi_type = ERHIType::Metal, .name = "MetalRHISmoke"});
            if (RenderDevice::Get().GetShaderPlatform() != EShaderPlatform::SP_METAL_MSL) {
                throw std::runtime_error("Metal device selected the wrong shader platform");
            }
            FenceRef rejected_fence = RenderDevice::Get().CreateFence();
            rejected_fence->Reject(1);
            bool rejected_dependency_failed = false;
            try {
                RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Wait(
                    WaitEvent{uint64_t(rejected_fence.Get()), 1}
                );
            } catch (const std::runtime_error&) {
                rejected_dependency_failed = true;
            }
            if (!rejected_dependency_failed) {
                throw std::runtime_error("Metal graphics queue accepted a rejected dependency");
            }
            CheckRasterTextureFormats();
            CheckGraphicsPipeline();
            CheckGraphicsUploads();
            std::vector<uint8_t> rgba8(7 * 5 * 4);
            for (size_t i = 0; i < rgba8.size(); ++i) rgba8[i] = static_cast<uint8_t>(i * 17);
            UploadAndCheckTexture(PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED, rgba8, 4);
            std::vector<uint8_t> r8(7 * 5);
            for (size_t i = 0; i < r8.size(); ++i) r8[i] = static_cast<uint8_t>(i * 7);
            UploadAndCheckTexture(PF_R8_UNORM, ETextureUsageFlags::SAMPLED, r8, 1);
            std::vector<uint8_t> rgba16f(7 * 5 * 8);
            for (size_t i = 0; i < rgba16f.size(); ++i) rgba16f[i] = static_cast<uint8_t>(i * 3);
            UploadAndCheckTexture(PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::SAMPLED, rgba16f, 8);
            std::vector<float> rgba32f(7 * 5 * 4);
            for (size_t i = 0; i < rgba32f.size(); ++i) rgba32f[i] = float(i) / 100.0f;
            UploadAndCheckTexture(
                PF_R32G32B32A32_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rgba32f.data()),
                                         rgba32f.size() * sizeof(float)),
                16
            );
            CheckBufferUpload();
            CheckCopyCompletionCallbacks();
            CheckBindlessTables();
            auto source = std::make_shared<SmokeWindowSource>(window);
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width <= 0 || height <= 0) throw std::runtime_error("window has no drawable");
            SwapchainCreateInfo info{
                .surface = SwapchainSurfaceInfo{source},
                .size = Extent2D(width, height),
                .preferred_format = PF_R8G8B8A8_UNORM,
            };
            SwapchainRef swapchain = RenderDevice::Get().CreateSwapchain(info);
            if (!swapchain || !swapchain->IsPresentationReady()) {
                throw std::runtime_error("Metal swapchain creation failed");
            }
            TextureRef texture = ClearAndCheckTexture(width, height);
            Present(swapchain, texture);
            std::cout << "first present: success" << std::endl;

            glfwSetWindowSize(window, 800, 500);
            glfwPollEvents();
            glfwGetFramebufferSize(window, &width, &height);
            if (width <= 0 || height <= 0) throw std::runtime_error("resize produced no drawable");
            info.size = Extent2D(width, height);
            if (!swapchain->Recreate(info)) throw std::runtime_error("Metal swapchain resize failed");
            texture = ClearAndCheckTexture(width, height);
            Present(swapchain, texture);
            std::cout << "resize present: success" << std::endl;

            const int hold_seconds = argc > 1 ? std::max(0, std::atoi(argv[1])) : 2;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
            while (std::chrono::steady_clock::now() < until && !glfwWindowShouldClose(window)) {
                glfwPollEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            texture = {};
            swapchain = {};
            source.reset();
            RenderDevice::Dispose();
            Moer::TaskSystem::ShutDown();
            task_system_initialized = false;
            glfwDestroyWindow(window);
            glfwTerminate();
            std::cout << "Metal RHI smoke passed" << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Metal RHI smoke failed: " << error.what() << std::endl;
            if (Moer::Render::RenderDevice::IsInitialized()) Moer::Render::RenderDevice::Dispose();
            if (task_system_initialized) Moer::TaskSystem::ShutDown();
            if (window != nullptr) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }
}
