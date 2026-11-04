#include "D3D12Backend.h"
#include <algorithm>
#include <cstring>

namespace Moer::Render::D3D12 {
namespace {
bool IsUav(ED3D12ShaderVariableType type) {
    return (type >= ED3D12ShaderVariableType::RWByteAddressBuffer && type <= ED3D12ShaderVariableType::RWTypedBuffer) ||
           (type >= ED3D12ShaderVariableType::RWTexture1D && type <= ED3D12ShaderVariableType::RWTexture3D);
}
ED3D12ShaderVariableType TextureType(TextureView view, bool write) {
    if (view.texture->GetDimension() == ETextureDimension::TEX_3D)
        return write ? ED3D12ShaderVariableType::RWTexture3D : ED3D12ShaderVariableType::Texture3D;
    if (!write && (view.texture->GetDimension() == ETextureDimension::TEX_CUBE || view.texture->GetDimension() == ETextureDimension::TEX_CUBE_ARRAY) && view.num_array >= 6) {
        return view.num_array == 6 ? ED3D12ShaderVariableType::TextureCube : ED3D12ShaderVariableType::TextureCubeArray;
    }
    return view.num_array > 1 ? (write ? ED3D12ShaderVariableType::RWTexture2DArray : ED3D12ShaderVariableType::Texture2DArray) :
                                (write ? ED3D12ShaderVariableType::RWTexture2D : ED3D12ShaderVariableType::Texture2D);
}
} // namespace
uint32 Recorder::Descriptor(DescriptorHeap& heap, uint32 count) {
    const uint32 index = heap.Allocate(count);
    try {
        packet.descriptors.push_back({&heap, {index, count}});
    } catch (...) {
        heap.Free(index, count);
        throw;
    }
    return index;
}
BufferRef Recorder::Upload(std::span<const byte> bytes, uint64 min_size) {
    auto              buffer = context.Buffer(Align(std::max<uint64>(bytes.size(), min_size), 256), EBufferUsageFlags::NONE, D3D12_HEAP_TYPE_UPLOAD);
    auto&             native = *static_cast<GpuBuffer*>(buffer.Get());
    void*             mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    DX_CHECK_HRESULT(native.resource->Map(0, &empty, &mapped));
    std::memset(mapped, 0, SIZE_T(native.GetByteSize()));
    if (!bytes.empty())
        std::memcpy(mapped, bytes.data(), bytes.size());
    native.resource->Unmap(0, nullptr);
    packet.buffers.push_back(buffer);
    return buffer;
}
uint32 Recorder::View(BufferView view, ED3D12ShaderVariableType type, uint32 index) {
    if (!view.buffer || view.GetByteOffset() + view.GetByteSize() > view.buffer->GetByteSize())
        throw std::invalid_argument("Invalid D3D12 buffer descriptor range");
    if (index == UINT32_MAX)
        index = Descriptor(*context.views);
    auto&      buffer = *static_cast<GpuBuffer*>(view.buffer);
    const auto cpu    = context.views->Cpu(index);
    if (type == ED3D12ShaderVariableType::ConstantBuffer) {
        if (view.GetByteOffset() % 256)
            throw std::invalid_argument("D3D12 CBV offset must be 256-byte aligned");
        const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{buffer.Address(view.GetByteOffset()), uint32(Align(view.GetByteSize(), 256))};
        context.device->CreateConstantBufferView(&desc, cpu);
    } else {
        const bool   raw        = type == ED3D12ShaderVariableType::ByteAddressBuffer || type == ED3D12ShaderVariableType::RWByteAddressBuffer;
        const bool   structured = type == ED3D12ShaderVariableType::StructuredBuffer || type == ED3D12ShaderVariableType::RWStructuredBuffer;
        const uint32 stride     = raw ? 4 : structured ? view.GetStride() :
                                                         uint32(GetSizeFromImageFormat(view.format, {1, 1, 1}));
        if (!stride || view.GetByteOffset() % stride || view.GetByteSize() % stride)
            throw std::invalid_argument("Invalid D3D12 buffer element alignment");
        const DXGI_FORMAT format = raw ? DXGI_FORMAT_R32_TYPELESS : structured ? DXGI_FORMAT_UNKNOWN :
                                                                                 Format(view.format);
        if (IsUav(type)) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format                     = format;
            desc.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.FirstElement        = view.GetByteOffset() / stride;
            desc.Buffer.NumElements         = uint32(view.GetByteSize() / stride);
            desc.Buffer.StructureByteStride = structured ? stride : 0;
            desc.Buffer.Flags               = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
            context.device->CreateUnorderedAccessView(buffer.resource.Get(), nullptr, &desc, cpu);
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format                     = format;
            desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.FirstElement        = view.GetByteOffset() / stride;
            desc.Buffer.NumElements         = uint32(view.GetByteSize() / stride);
            desc.Buffer.StructureByteStride = structured ? stride : 0;
            desc.Buffer.Flags               = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
            context.device->CreateShaderResourceView(buffer.resource.Get(), &desc, cpu);
        }
    }
    return index;
}
uint32 Recorder::View(TextureView view, ED3D12ShaderVariableType type, uint32 index) {
    if (!view.texture)
        throw std::invalid_argument("Null D3D12 texture descriptor");
    if (index == UINT32_MAX)
        index = Descriptor(*context.views);
    auto&      texture = *static_cast<GpuTexture*>(view.texture);
    const auto cpu     = context.views->Cpu(index);
    if (IsUav(type)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = StorageFormat(view.format);
        if (type == ED3D12ShaderVariableType::RWTexture3D) {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            desc.Texture3D     = {view.mip_level, 0, std::max(1u, texture.GetDepth() >> view.mip_level)};
        } else if (texture.GetNumArray() > 1) {
            desc.ViewDimension  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {view.mip_level, view.array_layer, view.num_array, 0};
        } else {
            desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = view.mip_level;
        }
        context.device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &desc, cpu);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format                  = SampleFormat(view.format);
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (type == ED3D12ShaderVariableType::TextureCube) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube   = {view.mip_level, view.num_mips, 0};
        } else if (type == ED3D12ShaderVariableType::TextureCubeArray) {
            desc.ViewDimension    = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            desc.TextureCubeArray = {view.mip_level, view.num_mips, view.array_layer, view.num_array / 6u, 0};
        } else if (type == ED3D12ShaderVariableType::Texture3D) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            desc.Texture3D     = {view.mip_level, view.num_mips, 0};
        } else if (texture.GetNumSamples() > 1) {
            desc.ViewDimension    = texture.GetNumArray() > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMSArray = {view.array_layer, view.num_array};
        } else if (texture.GetNumArray() > 1) {
            desc.ViewDimension  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {view.mip_level, view.num_mips, view.array_layer, view.num_array, 0, 0};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D     = {view.mip_level, view.num_mips, 0, 0};
        }
        context.device->CreateShaderResourceView(texture.resource.Get(), &desc, cpu);
    }
    return index;
}
void Recorder::Bind(PipelineHandle handle, const ArrayArguments& args) {
    auto& pipeline = *static_cast<Pipeline*>(handle.GetState());
    list->SetPipelineState(pipeline.state.Get());
    if (pipeline.compute)
        list->SetComputeRootSignature(pipeline.signature.Get());
    else {
        list->SetGraphicsRootSignature(pipeline.signature.Get());
        list->IASetPrimitiveTopology(pipeline.topology);
    }
    auto table = [&](uint32 root, D3D12_GPU_DESCRIPTOR_HANDLE descriptor) {
        if (pipeline.compute)
            list->SetComputeRootDescriptorTable(root, descriptor);
        else
            list->SetGraphicsRootDescriptorTable(root, descriptor);
    };
    for (const auto& binding : pipeline.bindings) {
        if (binding.constants) {
            Array<byte> bytes(binding.constant_size);
            for (auto copy : binding.constant_copies) {
                if (copy.source + copy.size > args.constants.size() * sizeof(uint32))
                    throw std::invalid_argument("D3D12 constant upload range mismatch");
                std::memcpy(bytes.data() + copy.destination, reinterpret_cast<const byte*>(args.constants.data()) + copy.source, copy.size);
            }
            const auto buffer  = Upload(bytes);
            const auto address = static_cast<GpuBuffer*>(buffer.Get())->Address();
            if (pipeline.compute)
                list->SetComputeRootConstantBufferView(binding.root, address);
            else
                list->SetGraphicsRootConstantBufferView(binding.root, address);
            continue;
        }
        if (binding.argument >= args.args.size())
            throw std::invalid_argument("Missing D3D12 pipeline argument");
        const auto& arg = args.args[binding.argument];
        if (const auto* bindless = std::get_if<BindlessArrayRef>(&arg)) {
            auto* array  = static_cast<Bindless*>(bindless->Get());
            auto  cached = bindless_tables.find(array);
            if (cached == bindless_tables.end()) {
                const auto    entries = array->Snapshot();
                Array<uint32> indices(entries.size(), 0);
                for (uint32 i = 1; i < entries.size(); i += 2) {
                    const auto& entry = entries[i];
                    if (entry.buffer) {
                        auto& buffer = *static_cast<GpuBuffer*>(entry.buffer.Get());
                        if (buffer.state == D3D12_RESOURCE_STATE_COMMON || buffer.state == D3D12_RESOURCE_STATE_COPY_DEST || buffer.state == D3D12_RESOURCE_STATE_COPY_SOURCE)
                            Transition(buffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        indices[i] = View(entry.buffer_view, ED3D12ShaderVariableType::ByteAddressBuffer);
                        if (EnumHasAnyFlag(buffer.GetUsage(), EBufferUsageFlags::UNORDERED_ACCESS))
                            indices[i + 1] = View(entry.buffer_view, ED3D12ShaderVariableType::RWByteAddressBuffer);
                        packet.resources.emplace_back(entry.buffer.Get());
                    } else if (entry.texture) {
                        auto& texture = *static_cast<GpuTexture*>(entry.texture.Get());
                        // Explicit graph/legacy barriers decide UAV versus SRV
                        // access. Only first-use/copy states need a fallback.
                        for (uint32 mip = entry.texture_view.mip_level; mip < entry.texture_view.mip_level + entry.texture_view.num_mips; ++mip)
                            for (uint32 layer = entry.texture_view.array_layer; layer < entry.texture_view.array_layer + entry.texture_view.num_array; ++layer) {
                                const auto state = texture.states[texture.Subresource(mip, layer)];
                                if (state == D3D12_RESOURCE_STATE_COMMON || state == D3D12_RESOURCE_STATE_COPY_DEST || state == D3D12_RESOURCE_STATE_COPY_SOURCE)
                                    Transition(texture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, mip, 1, layer, 1);
                            }
                        const uint32 sampler = context.SamplerIndex(entry.sampler);
                        indices[i]           = (View(entry.texture_view, TextureType(entry.texture_view, false)) << 8) | sampler;
                        if (EnumHasAnyFlag(texture.GetUsage(), ETextureUsageFlags::UNORDERED_ACCESS))
                            indices[i + 1] = (View(entry.texture_view, TextureType(entry.texture_view, true)) << 8) | sampler;
                        packet.resources.emplace_back(entry.texture.Get());
                    }
                }
                const auto buffer = Upload({reinterpret_cast<const byte*>(indices.data()), indices.size() * sizeof(uint32)});
                cached            = bindless_tables.emplace(array, static_cast<GpuBuffer*>(buffer.Get())->Address()).first;
            }
            if (pipeline.compute)
                list->SetComputeRootShaderResourceView(binding.root, cached->second);
            else
                list->SetGraphicsRootShaderResourceView(binding.root, cached->second);
        } else if (const auto* sampler = std::get_if<Sampler>(&arg)) {
            table(binding.root, context.samplers->Gpu(context.SamplerIndex(*sampler)));
        } else {
            const uint32 start = Descriptor(*context.views, binding.count);
            uint32       count = 0;
            auto         write = [&](const auto& view) {
                using T = std::decay_t<decltype(view)>;
                if (count >= binding.count)
                    throw std::invalid_argument("Too many D3D12 descriptor array elements");
                if constexpr (std::is_same_v<T, BufferView>) {
                    auto&      buffer = *static_cast<GpuBuffer*>(view.buffer);
                    const auto state  = binding.type == ED3D12ShaderVariableType::ConstantBuffer ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
                                                IsUav(binding.type)                                      ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
                                                                                                           D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    Transition(buffer, state);
                    View(view, binding.type, start + count++);
                } else {
                    Transition(*static_cast<GpuTexture*>(view.texture), IsUav(binding.type) ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, view.mip_level, view.num_mips, view.array_layer, view.num_array);
                    View(view, binding.type, start + count++);
                }
            };
            if (const auto* view = std::get_if<BufferView>(&arg))
                write(*view);
            else if (const auto* view = std::get_if<TextureView>(&arg))
                write(*view);
            else if (const auto* views = std::get_if<BufferViewArray>(&arg))
                for (const auto& view : *views)
                    write(view);
            else if (const auto* views = std::get_if<TextureViewArray>(&arg))
                for (const auto& view : *views)
                    write(view);
            else if (const auto* tlas = std::get_if<RaytracingTlasRef>(&arg)) {
                auto* buffer = static_cast<GpuBuffer*>((*tlas)->GetUnderlyingBuffer());
                if (!buffer)
                    throw std::invalid_argument("D3D12 TLAS has not been built");
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.ViewDimension                            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                desc.Shader4ComponentMapping                  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.RaytracingAccelerationStructure.Location = buffer->Address();
                context.device->CreateShaderResourceView(nullptr, &desc, context.views->Cpu(start));
                packet.resources.emplace_back(buffer);
                count = 1;
            } else
                throw std::invalid_argument("D3D12 reflected argument type mismatch");
            if (count != binding.count)
                throw std::invalid_argument("D3D12 descriptor array length mismatch");
            table(binding.root, context.views->Gpu(start));
        }
    }
}
} // namespace Moer::Render::D3D12
