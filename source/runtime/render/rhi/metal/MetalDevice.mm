#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/metal/MetalDevice.h"
#include "log/LogSystem.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Moer::Render {
namespace {

[[noreturn]] void Unsupported(const char* operation) {
    throw std::runtime_error(std::string("Metal RHI has not implemented ") + operation);
}

MTLPixelFormat ToMetalFormat(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return MTLPixelFormatR8Unorm;
        case PF_R8G8_UNORM:
            return MTLPixelFormatRG8Unorm;
        case PF_R8G8B8A8_UNORM:
            return MTLPixelFormatRGBA8Unorm;
        case PF_R8G8B8A8_SRGB:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case PF_B8G8R8A8_UNORM:
            return MTLPixelFormatBGRA8Unorm;
        case PF_B8G8R8A8_SRGB:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case PF_A2R10G10B10_UNORM_PACK32:
            return MTLPixelFormatBGR10A2Unorm;
        case PF_B10G11R11_UFLOAT_PACK32:
            return MTLPixelFormatRG11B10Float;
        case PF_R16_SFLOAT:
            return MTLPixelFormatR16Float;
        case PF_R16G16_SFLOAT:
            return MTLPixelFormatRG16Float;
        case PF_R16G16B16A16_SFLOAT:
            return MTLPixelFormatRGBA16Float;
        case PF_R32_SFLOAT:
            return MTLPixelFormatR32Float;
        case PF_R32G32B32A32_SFLOAT:
            return MTLPixelFormatRGBA32Float;
        case PF_D16_UNORM:
            return MTLPixelFormatDepth16Unorm;
        case PF_D32_SFLOAT:
            return MTLPixelFormatDepth32Float;
        case PF_D32_SFLOAT_S8_UINT:
            return MTLPixelFormatDepth32Float_Stencil8;
        default:
            throw std::runtime_error("Metal RHI has not implemented texture format " +
                                     std::to_string(static_cast<uint>(format)));
    }
}

NSUInteger PixelStride(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return 1;
        case PF_R8G8_UNORM:
        case PF_R16_SFLOAT:
        case PF_D16_UNORM:
            return 2;
        case PF_R16G16B16A16_SFLOAT:
        case PF_D32_SFLOAT_S8_UINT:
            return 8;
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_SRGB:
        case PF_B8G8R8A8_UNORM:
        case PF_B8G8R8A8_SRGB:
        case PF_A2R10G10B10_UNORM_PACK32:
        case PF_B10G11R11_UFLOAT_PACK32:
        case PF_R16G16_SFLOAT:
        case PF_R32_SFLOAT:
        case PF_D32_SFLOAT:
            return 4;
        case PF_R32G32B32A32_SFLOAT:
            return 16;
        default:
            Unsupported("this texture format");
    }
}

MTLVertexFormat ToMetalVertexFormat(EPixelFormat format) {
    switch (format) {
        case PF_R32_SFLOAT: return MTLVertexFormatFloat;
        case PF_R32G32_SFLOAT: return MTLVertexFormatFloat2;
        case PF_R32G32B32_SFLOAT: return MTLVertexFormatFloat3;
        case PF_R32G32B32A32_SFLOAT: return MTLVertexFormatFloat4;
        case PF_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
        default: Unsupported("this vertex attribute format");
    }
}

MTLBlendOperation ToMetalBlendOperation(EBlendOperation operation) {
    switch (operation) {
        case BO_ADD: return MTLBlendOperationAdd;
        case BO_SUBTRACT: return MTLBlendOperationSubtract;
        case BO_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case BO_MIN: return MTLBlendOperationMin;
        case BO_MAX: return MTLBlendOperationMax;
        default: Unsupported("this blend operation");
    }
}

MTLBlendFactor ToMetalBlendFactor(EBlendFactor factor) {
    switch (factor) {
        case BF_ZERO: return MTLBlendFactorZero;
        case BF_ONE: return MTLBlendFactorOne;
        case BF_SRC_COLOR: return MTLBlendFactorSourceColor;
        case BF_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case BF_DST_COLOR: return MTLBlendFactorDestinationColor;
        case BF_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case BF_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case BF_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case BF_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case BF_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case BF_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
        case BF_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
        case BF_SRC1_COLOR: return MTLBlendFactorSource1Color;
        case BF_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
        case BF_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
        case BF_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
        default: Unsupported("this blend factor");
    }
}

MTLCompareFunction ToMetalCompare(ECompareOption compare) {
    switch (compare) {
        case CO_NEVER: return MTLCompareFunctionNever;
        case CO_LESS: return MTLCompareFunctionLess;
        case CO_EQUAL: return MTLCompareFunctionEqual;
        case CO_LESS_OR_EQUAL: return MTLCompareFunctionLessEqual;
        case CO_GREATER: return MTLCompareFunctionGreater;
        case CO_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case CO_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
        case CO_ALWAYS: return MTLCompareFunctionAlways;
        default: Unsupported("this depth comparison");
    }
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
        if (mip >= GetNumMips()) Unsupported("texture mip sizes");
        return std::max(1u, GetWidth() >> mip) *
               std::max(1u, GetHeight() >> mip) * PixelStride(GetFormat());
    }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        texture_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLTexture> texture_;
};

void ValidateTextureUpload(const UploadTextureCmd& upload) {
    auto* texture = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
    const uint3 size = upload.Size();
    const uint mip = upload.MipLevel();
    if (texture == nullptr || upload.Format() != texture->GetFormat() ||
        mip >= texture->GetNumMips() || upload.ArrayLayer() >= texture->GetNumArray() ||
        upload.Offset() != uint3{0, 0, 0} ||
        size != uint3{std::max(1u, texture->GetWidth() >> mip),
                      std::max(1u, texture->GetHeight() >> mip), 1} ||
        upload.Data().size_bytes() != size_t(size.x) * size.y * PixelStride(upload.Format())) {
        Unsupported("this texture upload layout");
    }
}

void ValidateBufferUpload(const UploadBufferCmd& upload) {
    auto* buffer = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
    if (buffer == nullptr || upload.ByteSize() == 0 ||
        upload.Offset() > buffer->GetByteSize() ||
        upload.ByteSize() > buffer->GetByteSize() - upload.Offset() ||
        upload.Data().size_bytes() != upload.ByteSize()) {
        Unsupported("this buffer upload layout");
    }
}

void EncodeTextureUpload(
    id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadTextureCmd& upload
) {
    auto* texture = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
    const NSUInteger width = upload.Size().x;
    const NSUInteger height = upload.Size().y;
    const NSUInteger source_row_bytes = width * PixelStride(upload.Format());
    const NSUInteger staging_row_bytes = (source_row_bytes + 255) & ~NSUInteger(255);
    id<MTLBuffer> staging = [device newBufferWithLength:staging_row_bytes * height
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
              toTexture:texture->Native() destinationSlice:upload.ArrayLayer()
       destinationLevel:upload.MipLevel()
     destinationOrigin:MTLOriginMake(0, 0, 0)];
}

void EncodeBufferUpload(
    id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadBufferCmd& upload
) {
    auto* buffer = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
    id<MTLBuffer> staging = [device newBufferWithBytes:upload.Data().data()
                                              length:upload.ByteSize()
                                             options:MTLResourceStorageModeShared];
    if (staging == nil) throw std::runtime_error("Cannot allocate Metal buffer staging");
    [staging_buffers addObject:staging];
    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer->Native()
      destinationOffset:upload.Offset() size:upload.ByteSize()];
}

struct MetalComputeBinding {
    uint set{0};
    uint binding{0};
    EShaderArgType kind{SDA_Num};
    bool active{false};
    bool constant{false};
};

struct MetalRenderBindings {
    std::vector<std::pair<NSUInteger, uint>> bindless_sets;
    NSUInteger constant_buffer_index{0};
};

class MetalPipelineState final : public PipelineState {
public:
    MetalPipelineState(
        id<MTLRenderPipelineState> pipeline,
        std::vector<MTLPixelFormat> color_formats,
        uint vertex_bindings,
        RHIRasterizeInfo rasterizer,
        MTLPixelFormat depth_format,
        id<MTLDepthStencilState> depth_state,
        MetalRenderBindings vertex_bindings_layout,
        MetalRenderBindings fragment_bindings_layout
    ) : render_(pipeline), color_formats_(std::move(color_formats)),
        vertex_bindings_(vertex_bindings), rasterizer_(rasterizer),
        depth_format_(depth_format), depth_state_(depth_state),
        vertex_bindings_layout_(std::move(vertex_bindings_layout)),
        fragment_bindings_layout_(std::move(fragment_bindings_layout)) {}
    MetalPipelineState(
        id<MTLComputePipelineState> pipeline, id<MTLFunction> function,
        std::vector<MetalComputeBinding> bindings, MTLSize local_size,
        NSUInteger constant_buffer_index
    ) : compute_(pipeline), compute_function_(function),
        compute_bindings_(std::move(bindings)), local_size_(local_size),
        constant_buffer_index_(constant_buffer_index) {}
    id<MTLRenderPipelineState> NativeRender() const noexcept { return render_; }
    id<MTLComputePipelineState> NativeCompute() const noexcept { return compute_; }
    id<MTLFunction> ComputeFunction() const noexcept { return compute_function_; }
    const std::vector<MetalComputeBinding>& ComputeBindings() const noexcept { return compute_bindings_; }
    MTLSize LocalSize() const noexcept { return local_size_; }
    NSUInteger ConstantBufferIndex() const noexcept { return constant_buffer_index_; }
    const std::vector<MTLPixelFormat>& ColorFormats() const noexcept { return color_formats_; }
    uint VertexBindingCount() const noexcept { return vertex_bindings_; }
    const RHIRasterizeInfo& Rasterizer() const noexcept { return rasterizer_; }
    MTLPixelFormat DepthFormat() const noexcept { return depth_format_; }
    id<MTLDepthStencilState> DepthState() const noexcept { return depth_state_; }
    const MetalRenderBindings& VertexBindingsLayout() const noexcept {
        return vertex_bindings_layout_;
    }
    const MetalRenderBindings& FragmentBindingsLayout() const noexcept {
        return fragment_bindings_layout_;
    }

private:
    id<MTLRenderPipelineState> render_{nil};
    id<MTLComputePipelineState> compute_{nil};
    id<MTLFunction> compute_function_{nil};
    std::vector<MetalComputeBinding> compute_bindings_;
    MTLSize local_size_{MTLSizeMake(0, 0, 0)};
    NSUInteger constant_buffer_index_{0};
    std::vector<MTLPixelFormat> color_formats_;
    uint vertex_bindings_{0};
    RHIRasterizeInfo rasterizer_{};
    MTLPixelFormat depth_format_{MTLPixelFormatInvalid};
    id<MTLDepthStencilState> depth_state_{nil};
    MetalRenderBindings vertex_bindings_layout_{};
    MetalRenderBindings fragment_bindings_layout_{};
};

id<MTLFunction> CompileMetalFunction(id<MTLDevice> device, const SingleShaderInfo& shader) {
    if (shader.shader_data.empty() || shader.entry_point.empty()) {
        throw std::runtime_error("Metal pipeline shader source or entry point is empty");
    }
    NSString* source = [[NSString alloc]
        initWithBytes:shader.shader_data.data()
               length:shader.shader_data.size()
             encoding:NSUTF8StringEncoding];
    if (source == nil) throw std::runtime_error("Metal pipeline shader source is not UTF-8 MSL");
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (library == nil) {
        throw std::runtime_error("Metal shader compilation failed: " +
            std::string(error.localizedDescription.UTF8String ?: "unknown error"));
    }
    NSString* entry = [[NSString alloc] initWithBytes:shader.entry_point.data()
                                              length:shader.entry_point.size()
                                            encoding:NSUTF8StringEncoding];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    if (function == nil) {
        throw std::runtime_error("Metal shader entry point was not found: " +
            std::string(shader.entry_point));
    }
    return function;
}

PipelineHandle MetalPipelineMetadata(
    const PipelineShaderInfo& shader_info,
    std::initializer_list<const SingleShaderInfo*> stages
) {
    if (shader_info.layout_hash.size() != shader_info.arg_cpp_info.size() ||
        shader_info.layout_hash.size() > 64) {
        Unsupported("this pipeline argument layout");
    }
    PipelineHandle handle{};
    handle.binding_infos.resize(shader_info.layout_hash.size());
    for (uint index = 0; index < shader_info.layout_hash.size(); ++index) {
        handle.hash_2_info_index[GetHash(shader_info.layout_hash[index])] = index;
        for (const SingleShaderInfo* shader : stages) {
            if (shader->shader_param_map == nullptr) continue;
            const auto& reflection = shader->shader_param_map->reflect_map;
            const bool bindless = shader_info.arg_cpp_info[index].type == SDA_BindlessArray;
            const auto found = reflection.find(std::string(
                bindless ? ReflectParamInfo::bdls_name : shader_info.layout_hash[index]
            ));
            if (found == reflection.end()) continue;
            bool active = false;
            if (bindless) {
                const auto& resources = found->second.spirv.bindless;
                active = (resources.array && resources.array->custom_flag.active) ||
                         (resources.buffer && resources.buffer->custom_flag.active) ||
                         (resources.image && resources.image->custom_flag.active) ||
                         (resources.sampler && resources.sampler->custom_flag.active);
            } else if (const auto* resource = std::get_if<ReflectParamInfo::Resource>(
                           &found->second.spirv.resources.data)) {
                active = resource->custom_flag.active;
            } else if (const auto* constant = std::get_if<ReflectParamInfo::Constant>(
                           &found->second.spirv.resources.data)) {
                active = constant->custom_flag.active;
            }
            if (active) handle.valid_bits |= uint64(1) << index;
        }
        if (shader_info.arg_cpp_info[index].type == SDA_Constant &&
            (handle.valid_bits & (uint64(1) << index))) {
            handle.constant_idx = index;
        }
    }
    return handle;
}

MetalRenderBindings MetalRenderStageBindings(const SingleShaderInfo& shader) {
    MetalRenderBindings layout;
    if (shader.shader_param_map == nullptr) return layout;
    const auto& reflection = shader.shader_param_map->reflect_map;
    const auto found = reflection.find(std::string(ReflectParamInfo::bdls_name));
    if (found == reflection.end()) return layout;
    const auto& bindless = found->second.spirv.bindless;
    const auto add = [&](const std::optional<ReflectParamInfo::Bindless>& resource, uint table) {
        if (!resource || !resource->custom_flag.active) return;
        for (const auto& [set, existing_table] : layout.bindless_sets) {
            if (set == resource->set) {
                if (existing_table != table) Unsupported("overlapping Metal bindless sets");
                return;
            }
        }
        layout.bindless_sets.emplace_back(resource->set, table);
        layout.constant_buffer_index = std::max(
            layout.constant_buffer_index, NSUInteger(resource->set) + 1);
    };
    add(bindless.array, 1);
    add(bindless.buffer, 1);
    add(bindless.image, 2);
    add(bindless.sampler, 3);
    if (bindless.acceleration_structure &&
        bindless.acceleration_structure->custom_flag.active) {
        Unsupported("Metal bindless acceleration structures");
    }
    return layout;
}

std::vector<MetalComputeBinding> MetalComputeBindings(
    const PipelineShaderInfo& shader_info, const SingleShaderInfo& shader,
    NSUInteger& constant_buffer_index
) {
    std::vector<MetalComputeBinding> bindings(shader_info.layout_hash.size());
    if (shader.shader_param_map == nullptr) return bindings;
    const auto& reflection = shader.shader_param_map->reflect_map;
    NSUInteger highest_set = 0;
    bool has_set = false;
    for (uint index = 0; index < bindings.size(); ++index) {
        MetalComputeBinding& binding = bindings[index];
        binding.kind = shader_info.arg_cpp_info[index].type;
        binding.constant = shader_info.arg_cpp_info[index].type == SDA_Constant;
        if (binding.constant) continue;
        const auto found = reflection.find(std::string(shader_info.layout_hash[index]));
        if (found == reflection.end()) continue;
        const auto* resource = std::get_if<ReflectParamInfo::Resource>(
            &found->second.spirv.resources.data);
        if (resource == nullptr || !resource->custom_flag.active) continue;
        binding.set = resource->set;
        binding.binding = resource->binding;
        binding.active = true;
        highest_set = std::max(highest_set, NSUInteger(binding.set));
        has_set = true;
    }
    constant_buffer_index = has_set ? highest_set + 1 : 0;
    return bindings;
}

// The table layout mirrors the SPIRV-Cross MSL argument-buffer sets: set 1
// contains the indirect uint table and buffer addresses, set 2 texture IDs,
// and set 3 sampler IDs. Index zero stays unbound, as on Vulkan.
class MetalBindlessArray final : public BindlessArray {
public:
    static constexpr uint kSamplerCount = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
    static_assert(kSamplerCount <= 256);
    static_assert(sizeof(MTLResourceID) == sizeof(uint64_t));
    static_assert(sizeof(MTLGPUAddress) == sizeof(uint64_t));

    MetalBindlessArray(id<MTLDevice> device, uint capacity) :
        device_(device), capacity_(capacity) {
        if (capacity < 2 || capacity > (1u << 23)) {
            throw std::runtime_error("Metal bindless capacity is outside the shader handle range");
        }
        slots_.resize(capacity);
        const auto new_shared_buffer = [device](NSUInteger size) {
            id<MTLBuffer> buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
            if (buffer == nil) throw std::runtime_error("Cannot allocate Metal bindless table");
            std::memset(buffer.contents, 0, size);
            return buffer;
        };
        indices_ = new_shared_buffer(NSUInteger(capacity) * sizeof(uint32_t));
        buffer_arguments_ = new_shared_buffer(NSUInteger(capacity + 1) * sizeof(uint64_t));
        texture_arguments_ = new_shared_buffer(NSUInteger(capacity) * sizeof(uint64_t));
        sampler_arguments_ = new_shared_buffer(NSUInteger(kSamplerCount) * sizeof(uint64_t));
        const uint64_t index_address = indices_.gpuAddress;
        std::memcpy(buffer_arguments_.contents, &index_address, sizeof(index_address));
        free_slots_.reserve(capacity);
    }

    uint AllocateTexture(const TextureView& view, Sampler sampler) override {
        auto* texture = dynamic_cast<MetalTexture*>(view.GetTexture());
        if (texture == nullptr || view.format != texture->GetFormat() ||
            view.num_mips == 0 || view.mip_level + view.num_mips > texture->GetNumMips() ||
            view.num_array == 0 || view.array_layer + view.num_array > texture->GetNumArray() ||
            view.offset != uint3{0, 0, 0} ||
            view.extent != texture->GetExtent()) {
            Unsupported("this bindless texture view");
        }
        const bool is_cube = texture->GetDimension() == ETextureDimension::TEX_CUBE;
        if (is_cube && view.num_array != 1 &&
            (view.array_layer != 0 || view.num_array != 6)) {
            Unsupported("this cube bindless texture view");
        }
        id<MTLTexture> native_view = texture->Native();
        if (view.mip_level != 0 || view.num_mips != texture->GetNumMips() ||
            view.array_layer != 0 || view.num_array != texture->GetNumArray()) {
            MTLTextureType type = MTLTextureType2D;
            if (is_cube && view.num_array == 6) type = MTLTextureTypeCube;
            else if (view.num_array > 1) type = MTLTextureType2DArray;
            native_view = [texture->Native()
                newTextureViewWithPixelFormat:ToMetalFormat(view.format)
                                  textureType:type
                                       levels:NSMakeRange(view.mip_level, view.num_mips)
                                       slices:NSMakeRange(view.array_layer, view.num_array)];
            if (native_view == nil) Unsupported("this Metal texture subresource view");
        }
        const uint sampler_index = SamplerIndex(sampler);
        EnsureSampler(sampler, sampler_index);
        std::lock_guard lock(mutex_);
        const uint index = NextSlot();
        Slot& slot = slots_[index];
        const uint64 generation = NextGeneration(slot);
        TextureRef reference(view.GetTexture());
        pending_.emplace_back(TextureUpdateInfo{
            reference, sampler, view.format, index, index, view.mip_level, view.num_mips,
            view.array_layer, view.num_array, generation, generation, 0, false
        });
        slot.texture = std::move(reference);
        slot.texture_view = native_view;
        slot.buffer = {};
        slot.sampler_index = sampler_index;
        slot.kind = Kind::Texture;
        slot.active = true;
        return index;
    }

    uint AllocateBuffer(BufferView view) override {
        auto* buffer = dynamic_cast<MetalBuffer*>(view.GetBuffer());
        if (buffer == nullptr || view.GetByteSize() == 0 ||
            view.GetByteOffset() > buffer->GetByteSize() ||
            view.GetByteSize() > buffer->GetByteSize() - view.GetByteOffset()) {
            Unsupported("this bindless buffer view");
        }
        if (view.format != PF_UNDEFINED) Unsupported("formatted bindless buffers");
        std::lock_guard lock(mutex_);
        const uint index = NextSlot();
        Slot& slot = slots_[index];
        const uint64 generation = NextGeneration(slot);
        BufferRef reference(view.GetBuffer());
        pending_.emplace_back(BufferUpdateInfo{
            reference, index, index, view.format, generation, generation, 0, false
        });
        slot.buffer = std::move(reference);
        slot.texture = {};
        slot.texture_view = nil;
        slot.buffer_offset = view.GetByteOffset();
        slot.kind = Kind::Buffer;
        slot.active = true;
        return index;
    }

    void UnbindTexture(uint index) override { Unbind(index, Kind::Texture); }
    void UnbindBuffer(uint index) override { Unbind(index, Kind::Buffer); }
    uint64 ArrayHandle() const override { return reinterpret_cast<uint64>(this); }

    void Apply(const Array<UpdateCmd>& updates) {
        std::lock_guard lock(mutex_);
        auto* indices = static_cast<uint32_t*>(indices_.contents);
        auto* buffers = static_cast<uint64_t*>(buffer_arguments_.contents);
        auto* textures = static_cast<uint64_t*>(texture_arguments_.contents);
        for (const UpdateCmd& update : updates) {
            std::visit([&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                    if (item.array_idx == 0 || item.array_idx >= capacity_) return;
                    Slot& slot = slots_[item.array_idx];
                    if (slot.generation != item.array_generation) return;
                    if (item.free) {
                        if (slot.active || slot.kind == Kind::Empty) return;
                        indices[item.array_idx] = 0;
                        textures[item.array_idx] = 0;
                        buffers[item.array_idx + 1] = 0;
                        slot.texture = {};
                        slot.texture_view = nil;
                        slot.buffer = {};
                        slot.kind = Kind::Empty;
                        free_slots_.push_back(item.array_idx);
                    } else if (slot.active) {
                        if constexpr (std::is_same_v<T, TextureUpdateInfo>) {
                            textures[item.array_idx] =
                                slot.texture_view.gpuResourceID._impl;
                            indices[item.array_idx] = (item.array_idx << 8) | slot.sampler_index;
                        } else {
                            buffers[item.array_idx + 1] =
                                static_cast<MetalBuffer*>(slot.buffer.Get())->Native().gpuAddress +
                                slot.buffer_offset;
                            indices[item.array_idx] = item.array_idx;
                        }
                    }
                }
            }, update);
        }
    }

    id<MTLBuffer> NativeIndices() const noexcept { return indices_; }
    id<MTLBuffer> NativeArguments(uint set) const noexcept {
        switch (set) {
            case 1: return buffer_arguments_;
            case 2: return texture_arguments_;
            case 3: return sampler_arguments_;
            default: return nil;
        }
    }

    void UseRenderResources(id<MTLRenderCommandEncoder> encoder) {
        std::lock_guard lock(mutex_);
        constexpr MTLRenderStages stages = MTLRenderStageVertex | MTLRenderStageFragment;
        [encoder useResource:indices_ usage:MTLResourceUsageRead stages:stages];
        [encoder useResource:buffer_arguments_ usage:MTLResourceUsageRead stages:stages];
        [encoder useResource:texture_arguments_ usage:MTLResourceUsageRead stages:stages];
        [encoder useResource:sampler_arguments_ usage:MTLResourceUsageRead stages:stages];
        for (uint index = 1; index < next_slot_; ++index) {
            const Slot& slot = slots_[index];
            if (!slot.active) continue;
            if (slot.kind == Kind::Texture && slot.texture_view != nil) {
                [encoder useResource:slot.texture_view usage:MTLResourceUsageRead stages:stages];
            } else if (slot.kind == Kind::Buffer && slot.buffer) {
                auto* buffer = static_cast<MetalBuffer*>(slot.buffer.Get());
                [encoder useResource:buffer->Native() usage:MTLResourceUsageRead stages:stages];
            }
        }
    }

protected:
    UniquePtr<Command> CreateUpdateCommand() override {
        std::lock_guard lock(mutex_);
        Array<UpdateCmd> snapshot = pending_;
        auto command = MakeUnique<UpdateBindlessArrayCmd>(
            this, std::move(snapshot), Array<byte>{}, Array<std::pair<uint, uint>>{},
            Array<byte>{}, Array<std::pair<uint, uint>>{}, Array<byte>{},
            Array<std::pair<uint, uint>>{}
        );
        pending_.clear();
        return command;
    }

    void DiscardUpdateCommand(const Array<UpdateCmd>& updates) override {
        std::lock_guard lock(mutex_);
        Array<UpdateCmd> recovered;
        for (const UpdateCmd& update : updates) {
            std::visit([&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                    if (item.array_idx > 0 && item.array_idx < capacity_) {
                        const Slot& slot = slots_[item.array_idx];
                        if (slot.generation == item.array_generation && slot.active != item.free) {
                            recovered.emplace_back(update);
                        }
                    }
                }
            }, update);
        }
        for (const UpdateCmd& update : pending_) recovered.emplace_back(update);
        pending_ = std::move(recovered);
    }

private:
    enum class Kind { Empty, Texture, Buffer };
    struct Slot {
        TextureRef texture{};
        id<MTLTexture> texture_view{nil};
        BufferRef buffer{};
        uint64 generation{0};
        uint64 buffer_offset{0};
        uint sampler_index{0};
        Kind kind{Kind::Empty};
        bool active{false};
    };

    uint NextSlot() {
        if (!free_slots_.empty()) {
            const uint index = free_slots_.back();
            if (slots_[index].generation == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("Metal bindless slot generation exhausted");
            }
            free_slots_.pop_back();
            return index;
        }
        if (next_slot_ >= capacity_) throw std::runtime_error("Metal bindless capacity exhausted");
        if (slots_[next_slot_].generation == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("Metal bindless slot generation exhausted");
        }
        return next_slot_++;
    }

    static uint64 NextGeneration(Slot& slot) {
        if (slot.generation == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("Metal bindless slot generation exhausted");
        }
        return ++slot.generation;
    }

    static uint SamplerIndex(Sampler sampler) {
        if (sampler.filter >= SF_Num || sampler.address_mode >= SAM_Num ||
            sampler.compare_function >= SCF_Num) {
            Unsupported("this bindless sampler");
        }
        return (uint(SF_Num) * uint(SAM_Num)) * uint(sampler.compare_function) +
               uint(SF_Num) * uint(sampler.address_mode) + uint(sampler.filter);
    }

    void EnsureSampler(Sampler sampler, uint index) {
        std::lock_guard lock(mutex_);
        if (samplers_[index] != nil) return;
        MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
        const bool nearest = sampler.filter == SF_NEAREST || sampler.filter == SF_ANISOTROPIC_NEAREST;
        desc.minFilter = nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        desc.magFilter = nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        desc.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        desc.maxAnisotropy = sampler.filter == SF_ANISOTROPIC_NEAREST ||
                             sampler.filter == SF_ANISOTROPIC_LINEAR ? 16 : 1;
        MTLSamplerAddressMode address = MTLSamplerAddressModeRepeat;
        switch (sampler.address_mode) {
            case SAM_REPEAT: break;
            case SAM_MIRRORED_REPEAT: address = MTLSamplerAddressModeMirrorRepeat; break;
            case SAM_CLAMP_TO_EDGE: address = MTLSamplerAddressModeClampToEdge; break;
            case SAM_CLAMP_TO_BORDER: address = MTLSamplerAddressModeClampToBorderColor; break;
            default: Unsupported("this bindless sampler address mode");
        }
        desc.sAddressMode = address;
        desc.tAddressMode = address;
        desc.rAddressMode = address;
        desc.supportArgumentBuffers = YES;
        if (sampler.compare_function != SCF_NEVER) {
            static constexpr MTLCompareFunction kCompare[] = {
                MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
                MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual,
                MTLCompareFunctionAlways
            };
            desc.compareFunction = kCompare[sampler.compare_function];
        }
        id<MTLSamplerState> native = [device_ newSamplerStateWithDescriptor:desc];
        if (native == nil) throw std::runtime_error("Cannot create Metal bindless sampler");
        samplers_[index] = native;
        static_cast<uint64_t*>(sampler_arguments_.contents)[index] = native.gpuResourceID._impl;
    }

    void Unbind(uint index, Kind kind) {
        std::lock_guard lock(mutex_);
        if (index == 0 || index >= capacity_ || !slots_[index].active ||
            slots_[index].kind != kind) {
            throw std::runtime_error("Metal bindless handle is not allocated with this resource type");
        }
        Slot& slot = slots_[index];
        if (kind == Kind::Texture) {
            pending_.emplace_back(TextureUpdateInfo{
                slot.texture, Sampler(SF_NEAREST, SAM_REPEAT), slot.texture->GetFormat(),
                index, index, 0, 1, 0, 1, slot.generation, slot.generation, 0, true
            });
        } else {
            pending_.emplace_back(BufferUpdateInfo{
                slot.buffer, index, index, PF_UNDEFINED,
                slot.generation, slot.generation, 0, true
            });
        }
        slot.active = false;
    }

    id<MTLDevice> device_;
    id<MTLBuffer> indices_;
    id<MTLBuffer> buffer_arguments_;
    id<MTLBuffer> texture_arguments_;
    id<MTLBuffer> sampler_arguments_;
    id<MTLSamplerState> samplers_[kSamplerCount]{};
    std::vector<Slot> slots_;
    std::vector<uint> free_slots_;
    Array<UpdateCmd> pending_;
    const uint capacity_;
    uint next_slot_{1};
    std::mutex mutex_;
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

MetalPipelineState* ValidateSimpleDraw(const SetDrawStateCmd& draw) {
    auto* pipeline = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(draw.Pipeline().handle));
    const RenderPassInfo& pass = draw.RenderPassInfo();
    if (pipeline == nullptr || pipeline->NativeRender() == nil ||
        pipeline->ColorFormats().empty() ||
        pipeline->ColorFormats().size() != pass.color_attachments.size() ||
        pass.view_mask != 0 || pass.viewport_cnt != 1 || !pass.render_area.IsValid() ||
        draw.DrawData().empty()) {
        throw std::runtime_error(
            "Metal RHI has not implemented graphics draw layout " + draw.name +
            " (pipeline_colors=" +
            std::to_string(pipeline == nullptr ? 0 : pipeline->ColorFormats().size()) +
            ", pass_colors=" + std::to_string(pass.color_attachments.size()) +
            ", args=" + std::to_string(draw.Args().args.size()) +
            ", constants=" + std::to_string(draw.Args().constants.size()) +
            ", view_mask=" + std::to_string(pass.view_mask) +
            ", viewports=" + std::to_string(pass.viewport_cnt) +
            ", meshes=" + std::to_string(draw.DrawData().size()) + ")"
        );
    }
    MetalBindlessArray* bindless = nullptr;
    for (const TArg& argument : draw.Args().args) {
        if (const auto* array = std::get_if<BindlessArrayRef>(&argument)) {
            if (bindless != nullptr) Unsupported("multiple graphics bindless arrays");
            bindless = dynamic_cast<MetalBindlessArray*>(array->Get());
            if (bindless == nullptr) Unsupported("a foreign graphics bindless array");
        } else if (!std::holds_alternative<TInvalidArg>(argument)) {
            Unsupported("this graphics shader argument kind");
        }
    }
    if ((!pipeline->VertexBindingsLayout().bindless_sets.empty() ||
         !pipeline->FragmentBindingsLayout().bindless_sets.empty()) && bindless == nullptr) {
        Unsupported("missing graphics bindless array");
    }
    if (draw.Args().constants.size() * sizeof(uint) > 4096) {
        Unsupported("this graphics push constant size");
    }
    const ColorAttachment& attachment = pass.color_attachments[0];
    auto* target = dynamic_cast<MetalTexture*>(attachment.target);
    if (target == nullptr || pipeline->ColorFormats().size() > 8 ||
        pipeline->ColorFormats()[0] != ToMetalFormat(target->GetFormat()) ||
        attachment.mip_level >= target->GetNumMips() ||
        attachment.array_layer >= target->GetNumArray() || attachment.array_count != 1 ||
        pass.render_area.offset.x < 0 || pass.render_area.offset.y < 0 ||
        uint(pass.render_area.offset.x) + pass.render_area.extent.width >
            std::max(1u, target->GetWidth() >> attachment.mip_level) ||
        uint(pass.render_area.offset.y) + pass.render_area.extent.height >
            std::max(1u, target->GetHeight() >> attachment.mip_level) ||
        GetStoreOp(attachment.action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE) {
        Unsupported("this graphics color attachment");
    }
    for (uint index = 1; index < pass.color_attachments.size(); ++index) {
        const ColorAttachment& color = pass.color_attachments[index];
        auto* color_target = dynamic_cast<MetalTexture*>(color.target);
        if (color_target == nullptr ||
            pipeline->ColorFormats()[index] != ToMetalFormat(color_target->GetFormat()) ||
            color_target->GetWidth() != target->GetWidth() ||
            color_target->GetHeight() != target->GetHeight() ||
            color.mip_level != attachment.mip_level ||
            color.array_layer != attachment.array_layer || color.array_count != 1 ||
            color.mip_level >= color_target->GetNumMips() ||
            color.array_layer >= color_target->GetNumArray() ||
            GetStoreOp(color.action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE) {
            Unsupported("this graphics MRT attachment");
        }
    }
    const DepthAttachment& depth = pass.depth_attachment;
    if (depth.Valid()) {
        auto* depth_target = dynamic_cast<MetalTexture*>(depth.target);
        const EAttachmentAction action = GetDepthAction(depth.action);
        if (depth_target == nullptr ||
            pipeline->DepthFormat() != ToMetalFormat(depth_target->GetFormat()) ||
            depth_target->GetWidth() != target->GetWidth() ||
            depth_target->GetHeight() != target->GetHeight() ||
            depth.mip_level != attachment.mip_level ||
            depth.array_layer != attachment.array_layer || depth.array_count != 1 ||
            depth.mip_level >= depth_target->GetNumMips() ||
            depth.array_layer >= depth_target->GetNumArray() ||
            GetStoreOp(action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE ||
            (depth_target->GetFormat() == PF_D32_SFLOAT_S8_UINT &&
             GetStoreOp(GetStencilAction(depth.action)) ==
                 EAttachmentStoreOp::MULTISAMPLE_RESOLVE)) {
            Unsupported("this graphics depth attachment");
        }
    } else if (pipeline->DepthFormat() != MTLPixelFormatInvalid) {
        Unsupported("this graphics depth attachment");
    }
    const RHIRasterizeInfo& raster = pipeline->Rasterizer();
    if (raster.cull_mode == RCM_FRONT_AND_BACK ||
        (raster.fill_mode != FM_FILL && raster.fill_mode != FM_LINE) ||
        raster.b_depth_clamp_enable || raster.b_depth_bias) {
        Unsupported("this graphics raster state");
    }
    for (const MeshDrawData& mesh : draw.DrawData()) {
        if (mesh.vtx_views.size() != pipeline->VertexBindingCount() ||
            (mesh.indirect_draw_param.has_value() == !mesh.draw_params.empty())) {
            Unsupported("this mesh draw layout");
        }
        if (mesh.indirect_draw_param) {
            const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
            auto* buffer = dynamic_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
            const bool indexed = std::holds_alternative<IndexBuffer>(mesh.idx_view);
            const uint argument_size = indexed ?
                sizeof(MTLDrawIndexedPrimitivesIndirectArguments) :
                sizeof(MTLDrawPrimitivesIndirectArguments);
            if (buffer == nullptr || indirect.stride < argument_size ||
                indirect.stride % alignof(uint32_t) != 0 ||
                indirect.buffer.GetByteOffset() % alignof(uint32_t) != 0 ||
                indirect.buffer.GetByteOffset() > buffer->GetByteSize() ||
                indirect.buffer.GetByteSize() >
                    buffer->GetByteSize() - indirect.buffer.GetByteOffset() ||
                indirect.count > indirect.buffer.GetByteSize() / indirect.stride) {
                Unsupported("this indirect draw buffer view");
            }
            if (indirect.count_buffer) {
                const BufferView& count_view = *indirect.count_buffer;
                auto* count_buffer = dynamic_cast<MetalBuffer*>(count_view.GetBuffer());
                if (count_buffer == nullptr || count_view.GetByteSize() != sizeof(uint32_t) ||
                    count_view.GetByteOffset() % alignof(uint32_t) != 0 ||
                    count_view.GetByteOffset() > count_buffer->GetByteSize() ||
                    count_view.GetByteSize() >
                        count_buffer->GetByteSize() - count_view.GetByteOffset()) {
                    Unsupported("this indirect draw count view");
                }
            }
        }
        for (const VertexBuffer& vertex : mesh.vtx_views) {
            auto* buffer = dynamic_cast<MetalBuffer*>(vertex.buffer);
            if (buffer == nullptr || vertex.offset >= buffer->GetByteSize()) {
                Unsupported("this vertex buffer view");
            }
        }
        if (const auto* indexed = std::get_if<IndexBuffer>(&mesh.idx_view)) {
            auto* buffer = dynamic_cast<MetalBuffer*>(indexed->buffer.GetBuffer());
            if (buffer == nullptr ||
                (indexed->stride != IET_UINT16 && indexed->stride != IET_UINT32)) {
                Unsupported("this index buffer view");
            }
            const uint64 stride = indexed->stride == IET_UINT16 ? 2 : 4;
            if (indexed->buffer.GetByteOffset() > buffer->GetByteSize() ||
                indexed->buffer.GetByteSize() >
                    buffer->GetByteSize() - indexed->buffer.GetByteOffset() ||
                indexed->buffer.GetByteOffset() % stride != 0 ||
                indexed->buffer.GetByteSize() % stride != 0) {
                Unsupported("this index buffer range");
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                if (uint64(param.first_index) + param.index_cnt >
                    indexed->buffer.GetByteSize() / stride) {
                    Unsupported("this indexed draw range");
                }
            }
        }
    }
    return pipeline;
}

void EncodeSimpleDraw(
    id<MTLCommandBuffer> command_buffer, const SetDrawStateCmd& draw,
    std::span<const uint> indirect_counts
) {
    auto* pipeline = static_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(draw.Pipeline().handle));
    const RenderPassInfo& pass = draw.RenderPassInfo();
    MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    for (uint index = 0; index < pass.color_attachments.size(); ++index) {
        const ColorAttachment& attachment = pass.color_attachments[index];
        auto* target = static_cast<MetalTexture*>(attachment.target);
        auto* color = descriptor.colorAttachments[index];
        color.texture = target->Native();
        color.level = attachment.mip_level;
        color.slice = attachment.array_layer;
        switch (GetLoadOp(attachment.action)) {
            case EAttachmentLoadOp::CLEAR: color.loadAction = MTLLoadActionClear; break;
            case EAttachmentLoadOp::LOAD: color.loadAction = MTLLoadActionLoad; break;
            default: color.loadAction = MTLLoadActionDontCare; break;
        }
        color.storeAction = GetStoreOp(attachment.action) == EAttachmentStoreOp::STORE ?
            MTLStoreActionStore : MTLStoreActionDontCare;
        const float4 clear = attachment.clear_color;
        color.clearColor = MTLClearColorMake(clear.x, clear.y, clear.z, clear.w);
    }
    if (pass.depth_attachment.Valid()) {
        const DepthAttachment& depth = pass.depth_attachment;
        auto* depth_target = static_cast<MetalTexture*>(depth.target);
        auto* native_depth = descriptor.depthAttachment;
        native_depth.texture = depth_target->Native();
        native_depth.level = depth.mip_level;
        native_depth.slice = depth.array_layer;
        const EAttachmentAction action = GetDepthAction(depth.action);
        switch (GetLoadOp(action)) {
            case EAttachmentLoadOp::CLEAR: native_depth.loadAction = MTLLoadActionClear; break;
            case EAttachmentLoadOp::LOAD: native_depth.loadAction = MTLLoadActionLoad; break;
            default: native_depth.loadAction = MTLLoadActionDontCare; break;
        }
        native_depth.storeAction = GetStoreOp(action) == EAttachmentStoreOp::STORE ?
            MTLStoreActionStore : MTLStoreActionDontCare;
        native_depth.clearDepth = depth.clear_depth;
        if (depth_target->GetFormat() == PF_D32_SFLOAT_S8_UINT) {
            auto* native_stencil = descriptor.stencilAttachment;
            native_stencil.texture = depth_target->Native();
            native_stencil.level = depth.mip_level;
            native_stencil.slice = depth.array_layer;
            const EAttachmentAction stencil_action = GetStencilAction(depth.action);
            switch (GetLoadOp(stencil_action)) {
                case EAttachmentLoadOp::CLEAR: native_stencil.loadAction = MTLLoadActionClear; break;
                case EAttachmentLoadOp::LOAD: native_stencil.loadAction = MTLLoadActionLoad; break;
                default: native_stencil.loadAction = MTLLoadActionDontCare; break;
            }
            native_stencil.storeAction =
                GetStoreOp(stencil_action) == EAttachmentStoreOp::STORE ?
                    MTLStoreActionStore : MTLStoreActionDontCare;
            native_stencil.clearStencil = depth.clear_stencil;
        }
    }
    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (encoder == nil) throw std::runtime_error("Cannot encode Metal draw render pass");
    [encoder setRenderPipelineState:pipeline->NativeRender()];
    if (pipeline->DepthState() != nil) [encoder setDepthStencilState:pipeline->DepthState()];
    MetalBindlessArray* bindless = nullptr;
    for (const TArg& argument : draw.Args().args) {
        if (const auto* array = std::get_if<BindlessArrayRef>(&argument)) {
            bindless = static_cast<MetalBindlessArray*>(array->Get());
        }
    }
    if (bindless != nullptr) {
        bindless->UseRenderResources(encoder);
        for (const auto& [set, table] : pipeline->VertexBindingsLayout().bindless_sets) {
            [encoder setVertexBuffer:bindless->NativeArguments(table) offset:0 atIndex:set];
        }
        for (const auto& [set, table] : pipeline->FragmentBindingsLayout().bindless_sets) {
            [encoder setFragmentBuffer:bindless->NativeArguments(table) offset:0 atIndex:set];
        }
    }
    if (!draw.Args().constants.empty()) {
        const void* data = draw.Args().constants.data();
        const NSUInteger length = draw.Args().constants.size() * sizeof(uint);
        [encoder setVertexBytes:data length:length
                       atIndex:pipeline->VertexBindingsLayout().constant_buffer_index];
        [encoder setFragmentBytes:data length:length
                         atIndex:pipeline->FragmentBindingsLayout().constant_buffer_index];
    }
    const Rect2D& rect = pass.render_area;
    [encoder setViewport:MTLViewport{double(rect.offset.x), double(rect.offset.y),
                                     double(rect.extent.width), double(rect.extent.height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{NSUInteger(rect.offset.x), NSUInteger(rect.offset.y),
                                          rect.extent.width, rect.extent.height}];
    const RHIRasterizeInfo& raster = pipeline->Rasterizer();
    [encoder setFrontFacingWinding:raster.b_front_counter_clockwise ?
        MTLWindingCounterClockwise : MTLWindingClockwise];
    [encoder setCullMode:raster.cull_mode == RCM_FRONT ? MTLCullModeFront :
                         raster.cull_mode == RCM_BACK ? MTLCullModeBack : MTLCullModeNone];
    [encoder setTriangleFillMode:raster.fill_mode == FM_LINE ?
        MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    for (uint mesh_index = 0; mesh_index < draw.DrawData().size(); ++mesh_index) {
        const MeshDrawData& mesh = draw.DrawData()[mesh_index];
        for (uint index = 0; index < mesh.vtx_views.size(); ++index) {
            const VertexBuffer& vertex = mesh.vtx_views[index];
            auto* buffer = static_cast<MetalBuffer*>(vertex.buffer);
            [encoder setVertexBuffer:buffer->Native() offset:vertex.offset atIndex:16 + index];
        }
        if (const auto* indexed = std::get_if<IndexBuffer>(&mesh.idx_view)) {
            auto* buffer = static_cast<MetalBuffer*>(indexed->buffer.GetBuffer());
            const uint stride = indexed->stride == IET_UINT16 ? 2 : 4;
            const MTLIndexType type = indexed->stride == IET_UINT16 ?
                MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            if (mesh.indirect_draw_param) {
                const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
                auto* arguments = static_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
                for (uint i = 0; i < indirect_counts[mesh_index]; ++i) {
                    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:type
                        indexBuffer:buffer->Native()
                        indexBufferOffset:indexed->buffer.GetByteOffset()
                        indirectBuffer:arguments->Native()
                        indirectBufferOffset:indirect.buffer.GetByteOffset() + i * indirect.stride];
                }
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:param.index_cnt indexType:type
                                   indexBuffer:buffer->Native()
                             indexBufferOffset:indexed->buffer.GetByteOffset() + param.first_index * stride
                                instanceCount:param.instance_cnt
                                   baseVertex:param.vertex_offset baseInstance:param.first_instance];
            }
        } else {
            if (mesh.indirect_draw_param) {
                const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
                auto* arguments = static_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
                for (uint i = 0; i < indirect_counts[mesh_index]; ++i) {
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        indirectBuffer:arguments->Native()
                        indirectBufferOffset:indirect.buffer.GetByteOffset() + i * indirect.stride];
                }
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:param.vertex_offset vertexCount:param.index_cnt
                         instanceCount:param.instance_cnt baseInstance:param.first_instance];
            }
        }
    }
    [encoder endEncoding];
}

MetalPipelineState* ValidateComputeDispatch(
    const DispatchCmd& dispatch, const TCachedArgArray& cached_args
) {
    auto* pipeline = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(dispatch.Pipeline().handle));
    if (pipeline == nullptr || pipeline->NativeCompute() == nil ||
        !std::holds_alternative<uint3>(dispatch.Param())) {
        Unsupported("this compute dispatch pipeline or indirect layout");
    }
    const MTLSize local = pipeline->LocalSize();
    if (local.width == 0 || local.height == 0 || local.depth == 0 ||
        local.width * local.height * local.depth >
            pipeline->NativeCompute().maxTotalThreadsPerThreadgroup) {
        Unsupported("this compute shader workgroup size");
    }
    const ArrayArguments& args = dispatch.Args(cached_args);
    const auto& bindings = pipeline->ComputeBindings();
    if (args.args.size() != bindings.size()) Unsupported("this compute argument count");
    for (uint index = 0; index < bindings.size(); ++index) {
        const MetalComputeBinding& binding = bindings[index];
        if (binding.constant) {
            if ((dispatch.Pipeline().valid_bits & (uint64(1) << index)) &&
                args.constants.empty()) {
                Unsupported("empty compute push constants");
            }
            continue;
        }
        if (!binding.active) continue;
        if (binding.set >= pipeline->ConstantBufferIndex() || binding.set >= 16 ||
            (binding.kind != SDA_Buffer && binding.kind != SDA_ConstantBuffer &&
             binding.kind != SDA_Texture)) {
            throw std::runtime_error(
                "Metal RHI has not implemented compute shader argument " + dispatch.name +
                " (index=" + std::to_string(index) +
                ", kind=" + std::to_string(static_cast<uint>(binding.kind)) +
                ", set=" + std::to_string(binding.set) +
                ", binding=" + std::to_string(binding.binding) + ")");
        }
        if (binding.kind == SDA_Texture) {
            const auto* view = std::get_if<TextureView>(&args.args[index]);
            auto* texture = view == nullptr ? nullptr :
                dynamic_cast<MetalTexture*>(view->GetTexture());
            if (texture == nullptr || view->format != texture->GetFormat() ||
                view->num_mips == 0 ||
                view->mip_level + view->num_mips > texture->GetNumMips() ||
                view->num_array == 0 ||
                view->array_layer + view->num_array > texture->GetNumArray() ||
                view->offset != uint3{0, 0, 0} ||
                view->extent != texture->GetExtent()) {
                Unsupported("this compute texture argument view");
            }
        } else {
            const auto* view = std::get_if<BufferView>(&args.args[index]);
            auto* buffer = view == nullptr ? nullptr :
                dynamic_cast<MetalBuffer*>(view->GetBuffer());
            if (buffer == nullptr || view->GetByteSize() == 0 ||
                view->GetByteOffset() > buffer->GetByteSize() ||
                view->GetByteSize() > buffer->GetByteSize() - view->GetByteOffset()) {
                Unsupported("this compute buffer argument view");
            }
        }
    }
    return pipeline;
}

void EncodeComputeDispatch(
    id<MTLCommandBuffer> command_buffer, NSMutableArray<id<MTLBuffer>>* staging_buffers,
    NSMutableArray<id<MTLTexture>>* texture_views,
    const DispatchCmd& dispatch, const TCachedArgArray& cached_args
) {
    auto* pipeline = static_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(dispatch.Pipeline().handle));
    const uint3 groups = std::get<uint3>(dispatch.Param());
    if (groups.x == 0 || groups.y == 0 || groups.z == 0) return;
    const ArrayArguments& args = dispatch.Args(cached_args);
    const auto& bindings = pipeline->ComputeBindings();
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) throw std::runtime_error("Cannot encode Metal compute dispatch");
    [encoder setComputePipelineState:pipeline->NativeCompute()];
    for (NSUInteger set = 0; set < pipeline->ConstantBufferIndex(); ++set) {
        bool needed = false;
        for (const auto& binding : bindings) {
            needed |= binding.active && binding.set == set;
        }
        if (!needed) continue;
        id<MTLArgumentEncoder> arguments = [pipeline->ComputeFunction()
            newArgumentEncoderWithBufferIndex:set];
        if (arguments == nil) Unsupported("this Metal compute argument buffer layout");
        id<MTLBuffer> table = [command_buffer.device
            newBufferWithLength:arguments.encodedLength
            options:MTLResourceStorageModeShared];
        if (table == nil) throw std::runtime_error("Cannot allocate Metal compute arguments");
        [staging_buffers addObject:table];
        [arguments setArgumentBuffer:table offset:0];
        for (uint index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            if (!binding.active || binding.set != set) continue;
            if (binding.kind == SDA_Texture) {
                const TextureView& view = std::get<TextureView>(args.args[index]);
                auto* texture = static_cast<MetalTexture*>(view.GetTexture());
                id<MTLTexture> native_view = texture->Native();
                if (view.mip_level != 0 || view.num_mips != texture->GetNumMips() ||
                    view.array_layer != 0 || view.num_array != texture->GetNumArray()) {
                    MTLTextureType type = view.num_array > 1 ?
                        MTLTextureType2DArray : MTLTextureType2D;
                    native_view = [texture->Native()
                        newTextureViewWithPixelFormat:ToMetalFormat(view.format)
                                          textureType:type
                                               levels:NSMakeRange(view.mip_level, view.num_mips)
                                               slices:NSMakeRange(view.array_layer, view.num_array)];
                    if (native_view == nil) Unsupported("this Metal compute texture view");
                    [texture_views addObject:native_view];
                }
                [arguments setTexture:native_view atIndex:binding.binding];
                [encoder useResource:native_view
                            usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            } else {
                const BufferView& view = std::get<BufferView>(args.args[index]);
                auto* buffer = static_cast<MetalBuffer*>(view.GetBuffer());
                [arguments setBuffer:buffer->Native() offset:view.GetByteOffset()
                             atIndex:binding.binding];
                [encoder useResource:buffer->Native()
                            usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            }
        }
        [encoder setBuffer:table offset:0 atIndex:set];
    }
    if (!args.constants.empty()) {
        [encoder setBytes:args.constants.data()
                  length:args.constants.size() * sizeof(uint)
                 atIndex:pipeline->ConstantBufferIndex()];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(groups.x, groups.y, groups.z)
            threadsPerThreadgroup:pipeline->LocalSize()];
    [encoder endEncoding];
}

class MetalCommandQueue final : public CommandQueue {
public:
    MetalCommandQueue(id<MTLCommandQueue> queue, MetalCompletionDispatcher& completions) :
        queue_(queue), completions_(completions) {}

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
        for (const auto& command : submit.cmds) {
            if (command->Type() != Command::EType::UpdateBindlessArray) continue;
            const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
            if (update.HandOffUpdates()) {
                static_cast<MetalBindlessArray*>(update.Handle())->Apply(update.UpdateCommands());
            }
        }
        if (has_gpu_work) @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            if (command_buffer == nil) throw std::runtime_error("Cannot create Metal command buffer");
            NSMutableArray<id<MTLBuffer>>* staging_buffers = [NSMutableArray array];
            NSMutableArray<id<MTLTexture>>* texture_views = [NSMutableArray array];
            struct PendingReadback {
                NSUInteger staging_index;
                void* destination;
                uint64 byte_size;
            };
            std::vector<PendingReadback> readbacks;
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::Scope ||
                    command->Type() == Command::EType::QueueTransfer ||
                    command->Type() == Command::EType::UpdateBindlessArray) continue;
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
                                std::string([command_buffer.error.localizedDescription UTF8String]
                                    ?: "unknown GPU error"));
                        }
                        for (const auto& [mesh_index, staging] : pending_counts) {
                            uint32_t count = 0;
                            std::memcpy(&count, staging.contents, sizeof(count));
                            indirect_counts[mesh_index] = std::min(count, indirect_counts[mesh_index]);
                        }
                        command_buffer = [queue_ commandBuffer];
                        if (command_buffer == nil) {
                            throw std::runtime_error("Cannot create Metal command buffer");
                        }
                    }
                    EncodeSimpleDraw(command_buffer, draw, indirect_counts);
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
                    std::string([command_buffer.error.localizedDescription UTF8String] ?: "unknown GPU error"));
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
    MetalCompletionDispatcher& completions_;
    std::mutex completion_enqueue_mutex_;
    std::atomic<uint64> next_callback_timeline_{0};
};

class MetalCopyQueue final : public CopyQueue {
public:
    MetalCopyQueue(id<MTLCommandQueue> queue, MetalCompletionDispatcher& completions) :
        queue_(queue), completions_(completions), timeline_fence_(MoerNew(MetalFence)()) {}

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
    MetalCompletionDispatcher& completions_;
    FenceRef timeline_fence_;
    uint64 next_timeline_{0};
};

} // namespace

struct MetalDevice::Native {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    MetalCompletionDispatcher graphics_completions;
    MetalCompletionDispatcher copy_completions;
    MetalCommandQueue graphics;
    MetalCopyQueue copy;

    Native(id<MTLDevice> metal_device, id<MTLCommandQueue> metal_queue) :
        device(metal_device), queue(metal_queue),
        graphics(metal_queue, graphics_completions), copy(metal_queue, copy_completions) {}
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
    const MTLPixelFormat native_format = ToMetalFormat(info.format);
    const bool is_depth = info.format == PF_D16_UNORM || info.format == PF_D32_SFLOAT ||
                          info.format == PF_D32_SFLOAT_S8_UINT;
    if ((is_depth && (info.usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (!is_depth && (info.usage & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (is_depth && (info.usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED)) {
        Unsupported("this texture format and usage combination");
    }
    MTLTextureDescriptor* desc = is_cube ?
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:native_format
                                                              size:info.extent.x
                                                         mipmapped:info.num_mips > 1] :
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:native_format
                                                            width:info.extent.x
                                                           height:info.extent.y
                                                        mipmapped:info.num_mips > 1];
    if (is_2d_array) {
        desc.textureType = MTLTextureType2DArray;
        desc.arrayLength = info.array_size;
    }
    desc.mipmapLevelCount = info.num_mips;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if ((info.usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageRenderTarget;
    }
    if ((info.usage & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) {
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
BindlessArrayRef MetalDevice::CreateBindlessArray(uint capacity) {
    return BindlessArrayRef(MoerNew(MetalBindlessArray)(native_->device, capacity));
}
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
PipelineHandle MetalDevice::CreatePipeline(GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info) {
    if (!std::holds_alternative<ShaderVsPs>(shader_info.shader_group) ||
        create_info.primitive_topology != EPrimitiveTopology::TRIANGLE_LIST ||
        create_info.view_mask != 0 || create_info.multi_view_count != 1 ||
        create_info.multisample_info.sample_count != 1 ||
        create_info.color_attachment_count > 8 ||
        shader_info.layout_hash.size() != shader_info.arg_cpp_info.size() ||
        shader_info.layout_hash.size() > 64 ||
        create_info.depth_stencil_info.b_enable_front_face_stencil ||
        create_info.depth_stencil_info.b_enable_back_face_stencil) {
        Unsupported("this graphics pipeline layout");
    }
    const auto& shaders = std::get<ShaderVsPs>(shader_info.shader_group);
    @autoreleasepool {
        id<MTLFunction> vertex = CompileMetalFunction(native_->device, shaders.vs);
        id<MTLFunction> fragment = CompileMetalFunction(native_->device, shaders.ps);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.rasterSampleCount = 1;
        descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;

        MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
        uint attribute_index = 0;
        for (uint binding_index = 0; binding_index < create_info.vertex_stream.bindings.size(); ++binding_index) {
            const VertexBinding& binding = create_info.vertex_stream.bindings[binding_index];
            const uint metal_buffer_index = 16 + binding_index; // Reserve low slots for MSL resource sets.
            if (metal_buffer_index >= 31) Unsupported("too many vertex streams");
            NSUInteger stride = 0;
            for (const VertexElement& element : binding.vertex_elements) {
                if (attribute_index >= 31) Unsupported("too many vertex attributes");
                auto* attribute = vertex_descriptor.attributes[attribute_index++];
                attribute.format = ToMetalVertexFormat(element.format);
                attribute.offset = stride;
                attribute.bufferIndex = metal_buffer_index;
                stride += GetByteFromPixelFormat(element.format);
            }
            auto* layout = vertex_descriptor.layouts[metal_buffer_index];
            layout.stride = stride;
            layout.stepFunction = binding.input_rate == VIR_INSTANCE ?
                MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            layout.stepRate = 1;
        }
        descriptor.vertexDescriptor = vertex_descriptor;

        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            auto* attachment = descriptor.colorAttachments[index];
            const RHIColorAttachmentInfo& info = create_info.color_attachments_info[index];
            attachment.pixelFormat = ToMetalFormat(info.pixel_format);
            const RHIBlendAttachmentInfo& blend = info.blend_state_info;
            attachment.blendingEnabled =
                blend.color_blend_op != BO_ADD || blend.color_src_blend_factor != BF_ONE ||
                blend.color_dst_blend_factor != BF_ZERO || blend.alpha_blend_op != BO_ADD ||
                blend.alpha_src_blend_factor != BF_ONE || blend.alpha_dst_blend_factor != BF_ZERO;
            attachment.rgbBlendOperation = ToMetalBlendOperation(blend.color_blend_op);
            attachment.alphaBlendOperation = ToMetalBlendOperation(blend.alpha_blend_op);
            attachment.sourceRGBBlendFactor = ToMetalBlendFactor(blend.color_src_blend_factor);
            attachment.destinationRGBBlendFactor = ToMetalBlendFactor(blend.color_dst_blend_factor);
            attachment.sourceAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_src_blend_factor);
            attachment.destinationAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_dst_blend_factor);
            MTLColorWriteMask write_mask = MTLColorWriteMaskNone;
            if (blend.color_write_mask & CW_RED) write_mask |= MTLColorWriteMaskRed;
            if (blend.color_write_mask & CW_GREEN) write_mask |= MTLColorWriteMaskGreen;
            if (blend.color_write_mask & CW_BLUE) write_mask |= MTLColorWriteMaskBlue;
            if (blend.color_write_mask & CW_ALPHA) write_mask |= MTLColorWriteMaskAlpha;
            attachment.writeMask = write_mask;
        }
        if (create_info.depth_stencil_format != PF_UNDEFINED) {
            descriptor.depthAttachmentPixelFormat = ToMetalFormat(create_info.depth_stencil_format);
            if (create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT) {
                descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> native_pipeline =
            [native_->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal render pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        id<MTLDepthStencilState> depth_state = nil;
        if (create_info.depth_stencil_format != PF_UNDEFINED) {
            MTLDepthStencilDescriptor* depth_descriptor = [MTLDepthStencilDescriptor new];
            depth_descriptor.depthCompareFunction =
                ToMetalCompare(create_info.depth_stencil_info.depth_test_op);
            depth_descriptor.depthWriteEnabled = create_info.depth_stencil_info.b_enable_depth_write;
            depth_state = [native_->device newDepthStencilStateWithDescriptor:depth_descriptor];
            if (depth_state == nil) throw std::runtime_error("Cannot create Metal depth state");
        }
        PipelineHandle handle = MetalPipelineMetadata(shader_info, {&shaders.vs, &shaders.ps});
        std::vector<MTLPixelFormat> color_formats;
        color_formats.reserve(create_info.color_attachment_count);
        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            color_formats.push_back(ToMetalFormat(create_info.color_attachments_info[index].pixel_format));
        }
        handle.handle = reinterpret_cast<uint64>(MoerNew(MetalPipelineState)(
            native_pipeline, std::move(color_formats),
            uint(create_info.vertex_stream.bindings.size()), create_info.rasterizer_info,
            descriptor.depthAttachmentPixelFormat, depth_state,
            MetalRenderStageBindings(shaders.vs), MetalRenderStageBindings(shaders.ps)
        ));
        return handle;
    }
}
PipelineHandle MetalDevice::CreatePipeline(PipelineShaderInfo&& shader_info) {
    if (!std::holds_alternative<ShaderCs>(shader_info.shader_group)) {
        Unsupported("this compute pipeline shader group");
    }
    const SingleShaderInfo& shader = std::get<ShaderCs>(shader_info.shader_group).cs;
    @autoreleasepool {
        id<MTLFunction> function = CompileMetalFunction(native_->device, shader);
        NSError* error = nil;
        id<MTLComputePipelineState> native_pipeline =
            [native_->device newComputePipelineStateWithFunction:function error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal compute pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        PipelineHandle handle = MetalPipelineMetadata(shader_info, {&shader});
        NSUInteger constant_buffer_index = 0;
        auto bindings = MetalComputeBindings(shader_info, shader, constant_buffer_index);
        const uint3 group = shader.compute_local_size;
        handle.handle = reinterpret_cast<uint64>(MoerNew(MetalPipelineState)(
            native_pipeline, function, std::move(bindings),
            MTLSizeMake(group.x, group.y, group.z), constant_buffer_index
        ));
        return handle;
    }
}
void MetalDevice::WaitIdle() { native_->graphics.Sync(); }

void* GetMetalNativeTexture(Texture* texture) noexcept {
    auto* metal_texture = dynamic_cast<MetalTexture*>(texture);
    return metal_texture == nullptr ? nullptr : (__bridge void*)metal_texture->Native();
}

void* GetMetalNativeBuffer(Buffer* buffer) noexcept {
    auto* metal_buffer = dynamic_cast<MetalBuffer*>(buffer);
    return metal_buffer == nullptr ? nullptr : (__bridge void*)metal_buffer->Native();
}

void* GetMetalNativeBindlessIndexBuffer(BindlessArray* array) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeIndices();
}

void* GetMetalNativeBindlessArgumentBuffer(BindlessArray* array, uint set) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeArguments(set);
}

void* GetMetalNativeComputePipeline(PipelineHandle pipeline) noexcept {
    auto* metal = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(pipeline.handle));
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeCompute();
}

} // namespace Moer::Render
