#include "rhi/metal/MetalBindless.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace Moer::Render {

MetalBindlessArray::MetalBindlessArray(id<MTLDevice> device, uint capacity) :
    device_(device), capacity_(capacity) {
    if (capacity < 2 || capacity > (1u << 23)) {
        throw std::runtime_error("Metal bindless capacity is outside the shader handle range");
    }
    slots_.resize(capacity);
    applied_slots_.resize(capacity);
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

uint MetalBindlessArray::AllocateTexture(const TextureView& view, Sampler sampler) {
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

uint MetalBindlessArray::AllocateBuffer(BufferView view) {
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
        reference, index, index, view.format, generation, generation, 0, false,
        view.GetByteOffset()
    });
    slot.buffer = std::move(reference);
    slot.texture = {};
    slot.texture_view = nil;
    slot.buffer_offset = view.GetByteOffset();
    slot.kind = Kind::Buffer;
    slot.active = true;
    return index;
}

void MetalBindlessArray::UnbindTexture(uint index) { Unbind(index, Kind::Texture); }

void MetalBindlessArray::UnbindBuffer(uint index) { Unbind(index, Kind::Buffer); }

void MetalBindlessArray::Apply(const Array<UpdateCmd>& updates) {
    std::lock_guard lock(mutex_);
    auto* indices = static_cast<uint32_t*>(indices_.contents);
    auto* buffers = static_cast<uint64_t*>(buffer_arguments_.contents);
    auto* textures = static_cast<uint64_t*>(texture_arguments_.contents);
    for (const UpdateCmd& update : updates) {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                if (item.array_idx == 0 || item.array_idx >= capacity_) return;
                AppliedSlot& applied = applied_slots_[item.array_idx];
                if (applied.generation > item.array_generation) return;
                if (item.free) {
                    indices[item.array_idx] = 0;
                    textures[item.array_idx] = 0;
                    buffers[item.array_idx + 1] = 0;
                    applied = AppliedSlot{};
                    applied.generation = item.array_generation;
                    Slot& slot = slots_[item.array_idx];
                    if (slot.generation == item.array_generation && !slot.active &&
                        slot.kind != Kind::Empty) {
                        slot.texture = {};
                        slot.texture_view = nil;
                        slot.buffer = {};
                        slot.kind = Kind::Empty;
                        free_slots_.push_back(item.array_idx);
                    }
                } else if constexpr (std::is_same_v<T, TextureUpdateInfo>) {
                    auto* texture = dynamic_cast<MetalTexture*>(item.texture.Get());
                    if (texture == nullptr) Unsupported("a foreign bindless texture update");
                    id<MTLTexture> native_view = texture->Native();
                    if (item.mip_level != 0 || item.num_mips != texture->GetNumMips() ||
                        item.array_layer != 0 || item.array_count != texture->GetNumArray()) {
                        const bool cube = texture->GetDimension() == ETextureDimension::TEX_CUBE;
                        const MTLTextureType type = cube && item.array_count == 6 ?
                            MTLTextureTypeCube : item.array_count > 1 ?
                            MTLTextureType2DArray : MTLTextureType2D;
                        native_view = [texture->Native()
                            newTextureViewWithPixelFormat:ToMetalFormat(item.format)
                                              textureType:type
                                                   levels:NSMakeRange(item.mip_level, item.num_mips)
                                                   slices:NSMakeRange(item.array_layer, item.array_count)];
                        if (native_view == nil) Unsupported("this bindless texture update view");
                    }
                    applied.texture = item.texture;
                    applied.texture_view = native_view;
                    applied.buffer = {};
                    applied.generation = item.array_generation;
                    const uint sampler_index = SamplerIndex(item.sampler);
                    textures[item.array_idx] = native_view.gpuResourceID._impl;
                    buffers[item.array_idx + 1] = 0;
                    indices[item.array_idx] = (item.array_idx << 8) | sampler_index;
                } else {
                    auto* buffer = dynamic_cast<MetalBuffer*>(item.buffer.Get());
                    if (buffer == nullptr || item.byte_offset >= buffer->GetByteSize()) {
                        Unsupported("this bindless buffer update");
                    }
                    applied.buffer = item.buffer;
                    applied.texture = {};
                    applied.texture_view = nil;
                    applied.generation = item.array_generation;
                    buffers[item.array_idx + 1] =
                        buffer->Native().gpuAddress + item.byte_offset;
                    textures[item.array_idx] = 0;
                    indices[item.array_idx] = item.array_idx;
                }
            }
        }, update);
    }
}

id<MTLBuffer> MetalBindlessArray::NativeIndices() const noexcept { return indices_; }

id<MTLBuffer> MetalBindlessArray::NativeArguments(uint set) const noexcept {
    switch (set) {
        case 1: return buffer_arguments_;
        case 2: return texture_arguments_;
        case 3: return sampler_arguments_;
        default: return nil;
    }
}

void MetalBindlessArray::UseRenderResources(id<MTLRenderCommandEncoder> encoder) {
    std::lock_guard lock(mutex_);
    constexpr MTLRenderStages stages = MTLRenderStageVertex | MTLRenderStageFragment;
    [encoder useResource:indices_ usage:MTLResourceUsageRead stages:stages];
    [encoder useResource:buffer_arguments_ usage:MTLResourceUsageRead stages:stages];
    [encoder useResource:texture_arguments_ usage:MTLResourceUsageRead stages:stages];
    [encoder useResource:sampler_arguments_ usage:MTLResourceUsageRead stages:stages];
    for (uint index = 1; index < next_slot_; ++index) {
        const AppliedSlot& slot = applied_slots_[index];
        if (slot.texture_view != nil) {
            [encoder useResource:slot.texture_view usage:MTLResourceUsageRead stages:stages];
        } else if (slot.buffer) {
            auto* buffer = static_cast<MetalBuffer*>(slot.buffer.Get());
            [encoder useResource:buffer->Native() usage:MTLResourceUsageRead stages:stages];
        }
    }
}

void MetalBindlessArray::UseComputeResources(id<MTLComputeCommandEncoder> encoder) {
    std::lock_guard lock(mutex_);
    [encoder useResource:indices_ usage:MTLResourceUsageRead];
    [encoder useResource:buffer_arguments_ usage:MTLResourceUsageRead];
    [encoder useResource:texture_arguments_ usage:MTLResourceUsageRead];
    [encoder useResource:sampler_arguments_ usage:MTLResourceUsageRead];
    for (uint index = 1; index < next_slot_; ++index) {
        const AppliedSlot& slot = applied_slots_[index];
        if (slot.texture_view != nil) {
            MTLResourceUsage usage = MTLResourceUsageRead;
            if ((slot.texture->GetUsage() & ETextureUsageFlags::UNORDERED_ACCESS) !=
                ETextureUsageFlags::UNDEFINED) {
                usage |= MTLResourceUsageWrite;
            }
            [encoder useResource:slot.texture_view usage:usage];
        } else if (slot.buffer) {
            auto* buffer = static_cast<MetalBuffer*>(slot.buffer.Get());
            MTLResourceUsage usage = MTLResourceUsageRead;
            if ((buffer->GetUsage() & EBufferUsageFlags::UNORDERED_ACCESS) !=
                EBufferUsageFlags::NONE) {
                usage |= MTLResourceUsageWrite;
            }
            [encoder useResource:buffer->Native() usage:usage];
        }
    }
}

UniquePtr<Command> MetalBindlessArray::CreateUpdateCommand() {
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

void MetalBindlessArray::DiscardUpdateCommand(const Array<UpdateCmd>& updates) {
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

uint MetalBindlessArray::NextSlot() {
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

uint64 MetalBindlessArray::NextGeneration(Slot& slot) {
    if (slot.generation == std::numeric_limits<uint64>::max()) {
        throw std::runtime_error("Metal bindless slot generation exhausted");
    }
    return ++slot.generation;
}

uint MetalBindlessArray::SamplerIndex(Sampler sampler) {
    if (sampler.filter >= SF_Num || sampler.address_mode >= SAM_Num ||
        sampler.compare_function >= SCF_Num) {
        Unsupported("this bindless sampler");
    }
    return (uint(SF_Num) * uint(SAM_Num)) * uint(sampler.compare_function) +
           uint(SF_Num) * uint(sampler.address_mode) + uint(sampler.filter);
}

void MetalBindlessArray::EnsureSampler(Sampler sampler, uint index) {
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

void MetalBindlessArray::Unbind(uint index, Kind kind) {
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
            slot.generation, slot.generation, 0, true, slot.buffer_offset
        });
    }
    slot.active = false;
}

void* GetMetalNativeBindlessIndexBuffer(BindlessArray* array) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeIndices();
}

void* GetMetalNativeBindlessArgumentBuffer(BindlessArray* array, uint set) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeArguments(set);
}
} // namespace Moer::Render
