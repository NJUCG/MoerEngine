#include "D3D12Backend.h"
#include <WinPixEventRuntime/pix3.h>
#include <algorithm>
#include <cstring>

namespace Moer::Render::D3D12 {
D3D12_RESOURCE_STATES AccessState(ERHIAccessFlags access, ETextureLayout layout) {
    using A                     = ERHIAccessFlags;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (EnumHasAnyFlag(access, A::TRANSFER_WRITE))
        return D3D12_RESOURCE_STATE_COPY_DEST;
    if (EnumHasAnyFlag(access, A::TRANSFER_READ))
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (EnumHasAnyFlag(access, A::COLOR_ATTACHMENT_WRITE | A::COLOR_ATTACHMENT_READ))
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (EnumHasAnyFlag(access, A::DEPTH_STENCIL_WRITE))
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (EnumHasAnyFlag(access, A::SHADER_WRITE))
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (EnumHasAnyFlag(access, A::DEPTH_STENCIL_READ))
        state |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (EnumHasAnyFlag(access, A::SHADER_READ | A::SHADER_SAMPLED_READ | A::SHADER_RESOURCE_VIEW | A::INPUT_ATTACHMENT_READ))
        state |= D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    if (EnumHasAnyFlag(access, A::UNIFORM_READ | A::VERTEX_ATTRIBUTE_READ))
        state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (EnumHasAnyFlag(access, A::INDEX_READ))
        state |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (EnumHasAnyFlag(access, A::INDIRECT_COMMAND_READ))
        state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (state != D3D12_RESOURCE_STATE_COMMON)
        return state;
    switch (layout) {
        case TEXTURE_LAYOUT_COLOR_ATTACHMENT:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case TEXTURE_LAYOUT_DEPTH_STENCIL_READ:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case TEXTURE_LAYOUT_TRANSFER_SRC:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case TEXTURE_LAYOUT_TRANSFER_DST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case TEXTURE_LAYOUT_PRESENT_SRC:
            return D3D12_RESOURCE_STATE_PRESENT;
        default:
            return state;
    }
}
D3D12_RESOURCE_STATES BufferState(EBufferState state, bool write) {
    switch (state) {
        case EBufferState::TRANSFER:
            return write ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        case EBufferState::VERTEX:
            return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        case EBufferState::INDEX:
            return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case EBufferState::INDIRECT:
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case EBufferState::SHADER_RESOURCE:
            return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case EBufferState::UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}
D3D12_RESOURCE_STATES TextureState(ETextureState state, bool write) {
    switch (state) {
        case ETextureState::TRANSFER:
            return write ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ETextureState::SHADER_RESOURCE:
        case ETextureState::SAMPLE:
            return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case ETextureState::RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ETextureState::DEPTH_STENCIL:
            return write ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_DEPTH_READ;
        case ETextureState::UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

Recorder::Recorder(Context& context, Packet& packet) :
    context(context),
    packet(packet) {
    DX_CHECK_HRESULT(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&packet.allocator)));
    DX_CHECK_HRESULT(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, packet.allocator.Get(), nullptr, IID_PPV_ARGS(&packet.list)));
    list                          = packet.list.Get();
    ID3D12DescriptorHeap* heaps[] = {context.views->Native(), context.samplers->Native()};
    list->SetDescriptorHeaps(2, heaps);
}
void Recorder::Transition(GpuBuffer& buffer, D3D12_RESOURCE_STATES state) {
    if (buffer.heap_type != D3D12_HEAP_TYPE_DEFAULT || buffer.state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        return;
    if (buffer.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(buffer.resource.Get());
            list->ResourceBarrier(1, &barrier);
        }
        return;
    }
    packet.rollback.push_back([&buffer, old = buffer.state] {
        buffer.state = old;
    });
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(), buffer.state, state);
    list->ResourceBarrier(1, &barrier);
    buffer.state = state;
}
void Recorder::Transition(GpuTexture& texture, D3D12_RESOURCE_STATES state, uint32 mip, uint32 mips, uint32 layer, uint32 layers) {
    if (mips == UINT32_MAX || mips == kRemainingSubresource)
        mips = texture.GetNumMips() - mip;
    if (layers == UINT32_MAX || layers == kRemainingSubresource)
        layers = texture.GetNumArray() - layer;
    if (texture.GetDimension() == ETextureDimension::TEX_3D) {
        layer  = 0;
        layers = 1;
    }
    if (mip + mips > texture.GetNumMips() || layer + layers > texture.GetNumArray())
        throw std::out_of_range("D3D12 texture transition range");
    bool         uav    = false;
    const uint32 planes = uint32(texture.states.size()) / (texture.GetNumMips() * texture.GetNumArray());
    for (uint32 plane = 0; plane < planes; ++plane)
        for (uint32 a = layer; a < layer + layers; ++a)
            for (uint32 m = mip; m < mip + mips; ++m) {
                const uint32 index = texture.Subresource(m, a, plane);
                auto&        old   = texture.states[index];
                if (old == state) {
                    uav |= state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    continue;
                }
                packet.rollback.push_back([&texture, index, value = old] {
                    texture.states[index] = value;
                });
                const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.resource.Get(), old, state, index);
                list->ResourceBarrier(1, &barrier);
                old = state;
            }
    if (uav) {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(texture.resource.Get());
        list->ResourceBarrier(1, &barrier);
    }
}
void Recorder::Barrier(const BarrierCmd& cmd) {
    for (const auto& b : cmd.ReadBuffers())
        Transition(*reinterpret_cast<GpuBuffer*>(b.handle), BufferState(b.state, false));
    for (const auto& b : cmd.WriteBuffers())
        Transition(*reinterpret_cast<GpuBuffer*>(b.handle), BufferState(b.state, true));
    for (const auto& b : cmd.ReadTextures())
        Transition(*reinterpret_cast<GpuTexture*>(b.handle), TextureState(b.state, false), b.mip_level, b.mip_cnt, b.array_layer, b.array_cnt);
    for (const auto& b : cmd.WriteTextures())
        Transition(*reinterpret_cast<GpuTexture*>(b.handle), TextureState(b.state, true), b.mip_level, b.mip_cnt, b.array_layer, b.array_cnt);
    for (const auto& b : cmd.ExplicitBuffers())
        Transition(*reinterpret_cast<GpuBuffer*>(b.handle), AccessState(b.dst_state.access));
    for (const auto& b : cmd.ExplicitTextures())
        Transition(*reinterpret_cast<GpuTexture*>(b.handle), AccessState(b.dst_state.access, b.dst_state.texture_layout), b.mip_level, b.mip_count, b.array_layer, b.array_count);
}
void Recorder::RenderPass(const RenderPassInfo& pass) {
    Array<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    uint32                             width = pass.render_area.extent.x, height = pass.render_area.extent.y;
    for (const auto& attachment : pass.color_attachments) {
        auto& texture = *static_cast<GpuTexture*>(attachment.target);
        Transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, attachment.mip_level, 1, attachment.array_layer, attachment.array_count);
        D3D12_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = Format(texture.GetFormat());
        if (texture.GetNumSamples() > 1) {
            desc.ViewDimension    = texture.GetNumArray() > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMSArray = {attachment.array_layer, attachment.array_count};
        } else if (texture.GetNumArray() > 1) {
            desc.ViewDimension  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {attachment.mip_level, attachment.array_layer, attachment.array_count, 0};
        } else {
            desc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = attachment.mip_level;
        }
        const auto handle = context.rtvs->Cpu(Descriptor(*context.rtvs));
        context.device->CreateRenderTargetView(texture.resource.Get(), &desc, handle);
        rtvs.push_back(handle);
        if (GetLoadOp(attachment.action) == EAttachmentLoadOp::CLEAR)
            list->ClearRenderTargetView(handle, &attachment.clear_color.x, 0, nullptr);
        if (!width)
            width = std::max(1u, texture.GetWidth() >> attachment.mip_level);
        if (!height)
            height = std::max(1u, texture.GetHeight() >> attachment.mip_level);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (pass.depth_attachment.Valid()) {
        const auto& attachment = pass.depth_attachment;
        auto&       texture    = *static_cast<GpuTexture*>(attachment.target);
        Transition(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE, attachment.mip_level, 1, attachment.array_layer, attachment.array_count);
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = Format(texture.GetFormat());
        if (texture.GetNumSamples() > 1) {
            desc.ViewDimension    = texture.GetNumArray() > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMSArray = {attachment.array_layer, attachment.array_count};
        } else if (texture.GetNumArray() > 1) {
            desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {attachment.mip_level, attachment.array_layer, attachment.array_count};
        } else {
            desc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = attachment.mip_level;
        }
        dsv = context.dsvs->Cpu(Descriptor(*context.dsvs));
        context.device->CreateDepthStencilView(texture.resource.Get(), &desc, dsv);
        uint32 clear = 0;
        if (GetLoadOp(GetDepthAction(attachment.action)) == EAttachmentLoadOp::CLEAR)
            clear |= D3D12_CLEAR_FLAG_DEPTH;
        if (GetLoadOp(GetStencilAction(attachment.action)) == EAttachmentLoadOp::CLEAR &&
            EnumHasAnyFlag(texture.GetAspectFlags(), ETextureAspectFlags::STENCIL_SLICE))
            clear |= D3D12_CLEAR_FLAG_STENCIL;
        if (clear)
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAGS(clear), attachment.clear_depth, uint8(attachment.clear_stencil), 0, nullptr);
        if (!width)
            width = std::max(1u, texture.GetWidth() >> attachment.mip_level);
        if (!height)
            height = std::max(1u, texture.GetHeight() >> attachment.mip_level);
    }
    list->OMSetRenderTargets(uint32(rtvs.size()), rtvs.data(), FALSE, pass.depth_attachment.Valid() ? &dsv : nullptr);
    const D3D12_VIEWPORT viewport{float(pass.render_area.offset.x), float(pass.render_area.offset.y), float(width), float(height), 0, 1};
    const D3D12_RECT     scissor{pass.render_area.offset.x, pass.render_area.offset.y, LONG(pass.render_area.offset.x + width), LONG(pass.render_area.offset.y + height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
}
void Recorder::Indirect(D3D12_INDIRECT_ARGUMENT_TYPE type, const IndirectDrawParam& param) {
    auto& buffer = *static_cast<GpuBuffer*>(param.buffer.buffer);
    Transition(buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    GpuBuffer* count = nullptr;
    if (param.count_buffer) {
        count = static_cast<GpuBuffer*>(param.count_buffer->buffer);
        Transition(*count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    const D3D12_INDIRECT_ARGUMENT_DESC argument{.Type = type};
    const D3D12_COMMAND_SIGNATURE_DESC desc{param.stride, 1, &argument, 0};
    ComPtr<ID3D12CommandSignature>     signature;
    DX_CHECK_HRESULT(context.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)));
    list->ExecuteIndirect(signature.Get(), param.count, buffer.resource.Get(), param.buffer.byte_offset, count ? count->resource.Get() : nullptr, count ? param.count_buffer->byte_offset : 0);
    packet.signatures.push_back(std::move(signature));
}
void Recorder::Mesh(const Pipeline& pipeline, const MeshDrawData& mesh) {
    Array<D3D12_VERTEX_BUFFER_VIEW> vertices;
    for (uint32 i = 0; i < mesh.vtx_views.size(); ++i) {
        const auto& view   = mesh.vtx_views[i];
        auto&       buffer = *static_cast<GpuBuffer*>(view.buffer);
        Transition(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        vertices.push_back({buffer.Address(view.offset), uint32(buffer.GetByteSize() - view.offset), pipeline.vertex_strides.at(i)});
    }
    if (!vertices.empty())
        list->IASetVertexBuffers(0, uint32(vertices.size()), vertices.data());
    const bool indexed = std::holds_alternative<IndexBuffer>(mesh.idx_view);
    if (indexed) {
        const auto& view   = std::get<IndexBuffer>(mesh.idx_view);
        auto&       buffer = *static_cast<GpuBuffer*>(view.buffer.buffer);
        Transition(buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        const D3D12_INDEX_BUFFER_VIEW native{buffer.Address(view.buffer.byte_offset), uint32(view.buffer.GetByteSize()), view.stride == IET_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&native);
    }
    if (mesh.indirect_draw_param)
        Indirect(indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, *mesh.indirect_draw_param);
    else
        for (const auto& draw : mesh.draw_params) {
            if (indexed)
                list->DrawIndexedInstanced(draw.index_cnt, draw.instance_cnt, draw.first_index, int32(draw.vertex_offset), draw.first_instance);
            else
                list->DrawInstanced(draw.index_cnt, draw.instance_cnt, draw.vertex_offset, draw.first_instance);
        }
}
void Recorder::Clear(const ClearResourceCmd& command) {
    if (command.IsBuffer()) {
        const auto    view   = command.Buffer();
        auto&         buffer = *static_cast<GpuBuffer*>(view.buffer);
        Array<uint32> values((view.GetByteSize() + 3) / 4, command.UIntValue());
        const auto    upload = Upload({reinterpret_cast<const byte*>(values.data()), view.GetByteSize()});
        Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(buffer.resource.Get(), view.byte_offset, static_cast<GpuBuffer*>(upload.Get())->resource.Get(), 0, view.GetByteSize());
        return;
    }
    const auto view    = command.Texture();
    auto&      texture = *static_cast<GpuTexture*>(view.texture);
    if (EnumHasAnyFlag(texture.GetUsage(), ETextureUsageFlags::UNORDERED_ACCESS)) {
        for (uint32 mip = view.mip_level; mip < view.mip_level + view.num_mips; ++mip) {
            Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mip, 1, view.array_layer, view.num_array);
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = StorageFormat(view.format);
            if (texture.GetDimension() == ETextureDimension::TEX_3D) {
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                desc.Texture3D     = {mip, 0, std::max(1u, texture.GetDepth() >> mip)};
            } else if (texture.GetNumArray() > 1) {
                desc.ViewDimension  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {mip, view.array_layer, view.num_array, 0};
            } else {
                desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = mip;
            }
            const uint32 gpu = Descriptor(*context.views);
            const uint32 cpu = Descriptor(*context.cpu_views);
            context.device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &desc, context.views->Cpu(gpu));
            context.device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &desc, context.cpu_views->Cpu(cpu));
            if (command.IsUInt()) {
                const uint32 values[]{command.UIntValue(), command.UIntValue(), command.UIntValue(), command.UIntValue()};
                list->ClearUnorderedAccessViewUint(context.views->Gpu(gpu), context.cpu_views->Cpu(cpu), texture.resource.Get(), values, 0, nullptr);
            } else
                list->ClearUnorderedAccessViewFloat(context.views->Gpu(gpu), context.cpu_views->Cpu(cpu), texture.resource.Get(), &command.Float4Value().x, 0, nullptr);
        }
        return;
    }
    if (EnumHasAnyFlag(texture.GetUsage(), ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT)) {
        RenderPassInfo pass{};
        pass.depth_attachment             = DepthAttachment(view);
        pass.depth_attachment.clear_depth = command.IsFloat4() ? command.Float4Value().x : float(command.UIntValue());
        RenderPass(pass);
        return;
    }
    if (EnumHasAnyFlag(texture.GetUsage(), ETextureUsageFlags::COLOR_ATTACHMENT) && command.IsFloat4()) {
        RenderPassInfo pass{};
        pass.color_attachments.push_back({&texture, AC_CLEAR_STORE, command.Float4Value(), view.mip_level, view.array_layer, view.num_array});
        RenderPass(pass);
        return;
    }
    // Clear through a tiny compute-free upload. This also supports sampled-only
    // textures; the tightly packed RHI bytes go through the normal row packer.
    const auto info = GetPixelFormatInfo(view.format);
    if (info.IsCompressed() || !info.IsDefined())
        throw std::invalid_argument("Cannot clear a compressed D3D12 texture");
    for (uint32 mip = view.mip_level; mip < view.mip_level + view.num_mips; ++mip) {
        const uint3 extent{std::max(1u, texture.GetWidth() >> mip), std::max(1u, texture.GetHeight() >> mip), std::max(1u, texture.GetDepth() >> mip)};
        Array<byte> bytes(GetSizeFromImageFormat(view.format, extent));
        if (command.IsUInt()) {
            const uint32 value = command.UIntValue();
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = reinterpret_cast<const byte*>(&value)[i % 4];
        } else {
            const auto value = command.Float4Value();
            if (view.format != PF_R32_SFLOAT && view.format != PF_R32G32_SFLOAT && view.format != PF_R32G32B32A32_SFLOAT &&
                (value.x != 0 || value.y != 0 || value.z != 0 || value.w != 0))
                throw std::invalid_argument("Nonzero float clear requires a render target or float32 texture");
            if (view.format == PF_R32_SFLOAT || view.format == PF_R32G32_SFLOAT || view.format == PF_R32G32B32A32_SFLOAT)
                for (size_t i = 0; i < bytes.size(); i += info.bytes_per_block)
                    std::memcpy(bytes.data() + i, &value, info.bytes_per_block);
        }
        for (uint32 layer = view.array_layer; layer < view.array_layer + view.num_array; ++layer) {
            UploadTextureCmd upload(view.format, uint64(&texture), mip, layer, {0, 0, 0}, extent, bytes.data());
            Copy(upload);
        }
    }
}
void Recorder::Query(const QueryCmd& command) {
    auto it = std::find_if(packet.queries.begin(), packet.queries.end(), [&](const QueryRecord& query) {
        return query.token.Id() == command.Token().Id();
    });
    if (command.IsBegin()) {
        if (it != packet.queries.end())
            throw std::invalid_argument("Duplicate D3D12 query begin");
        packet.queries.push_back({command.Token(), uint32(packet.queries.size() * 2), false});
        it = std::prev(packet.queries.end());
        if (command.IsTimestamp())
            list->EndQuery(packet.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, it->index);
        else
            list->BeginQuery(packet.occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, it->index);
    } else {
        if (it == packet.queries.end())
            throw std::invalid_argument("D3D12 query end without begin");
        it->ended = true;
        list->EndQuery(command.IsTimestamp() ? packet.timestamps.Get() : packet.occlusion.Get(), command.IsTimestamp() ? D3D12_QUERY_TYPE_TIMESTAMP : D3D12_QUERY_TYPE_OCCLUSION, it->index + (command.IsTimestamp() ? 1u : 0u));
    }
}
void Recorder::Record() {
    if (!packet.submit.query_tokens.empty()) {
        D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, uint32(packet.submit.query_tokens.size() * 2), 0};
        DX_CHECK_HRESULT(context.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&packet.timestamps)));
        desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        DX_CHECK_HRESULT(context.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&packet.occlusion)));
    }
    auto args = [&](const TShaderArgArray& arguments) -> const ArrayArguments& {
        if (const auto* value = std::get_if<ArrayArguments>(&arguments))
            return *value;
        if (const auto* cached = std::get_if<ArrayArgReference>(&arguments))
            return packet.submit.cached_args.at(cached->handle);
        static const ArrayArguments empty;
        return empty;
    };
    for (const auto& command : packet.submit.cmds) {
        using Type = Command::EType;
        switch (command->Type()) {
            case Type::UploadBuffer:
            case Type::CopyBackBuffer:
            case Type::BufferToBuffer:
            case Type::BufferToTexture:
            case Type::TextureToBuffer:
            case Type::UploadTexture:
            case Type::TextureToTexture:
            case Type::CopyBackTexture:
                Copy(*command);
                break;
            case Type::Barrier:
                Barrier(static_cast<const BarrierCmd&>(*command));
                break;
            case Type::UpdateBindlessArray: {
                const auto& cmd   = static_cast<const UpdateBindlessArrayCmd&>(*command);
                auto*       array = static_cast<Bindless*>(cmd.Handle());
                packet.rollback.push_back(array->Apply(cmd));
                bindless_tables.erase(array);
                break;
            }
            case Type::ShaderDispatch: {
                const auto& cmd = static_cast<const DispatchCmd&>(*command);
                Bind(cmd.Pipeline(), cmd.Args(packet.submit.cached_args));
                std::visit(Overload{[&](uint3 count) {
                                        list->Dispatch(count.x, count.y, count.z);
                                    },
                                    [&](const DispatchIndirectParam& indirect) {
                                        Indirect(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, {indirect.indirect, std::nullopt, 1, 12});
                                    }},
                           cmd.Param());
                break;
            }
            case Type::SetDrawState: {
                const auto& cmd = static_cast<const SetDrawStateCmd&>(*command);
                RenderPass(cmd.RenderPassInfo());
                Bind(cmd.Pipeline(), cmd.Args());
                for (const auto& mesh : cmd.DrawData())
                    Mesh(*static_cast<Pipeline*>(cmd.Pipeline().GetState()), mesh);
                break;
            }
            case Type::MultiDraw: {
                const auto& cmd = static_cast<const MultiDrawCmd&>(*command);
                RenderPass(cmd.render_pass_info);
                for (const auto& draw : cmd.draw_batch.draw_cmds) {
                    Bind(draw.handle, args(draw.args));
                    if (const auto* meshes = std::get_if<Array<MeshDrawData>>(&draw.mesh_dispatch_data)) {
                        for (const auto& mesh : *meshes)
                            Mesh(*static_cast<Pipeline*>(draw.handle.GetState()), mesh);
                    } else
                        for (const auto& mesh : std::get<Array<DispatchMeshData>>(draw.mesh_dispatch_data)) {
                            std::visit(Overload{[&](Vector3ui count) {
                                                    list->DispatchMesh(count.x, count.y, count.z);
                                                },
                                                [&](const IndirectDrawParam& indirect) {
                                                    Indirect(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, indirect);
                                                }},
                                       mesh.draw_param);
                        }
                }
                break;
            }
            case Type::ClearResource:
                Clear(static_cast<const ClearResourceCmd&>(*command));
                break;
            case Type::BuildAccel:
                Build(static_cast<const BuildAccelerationStructuresCmd&>(*command));
                break;
            case Type::BuildTLAS:
                Build(static_cast<const UpdateRaytracingSceneCmd&>(*command));
                break;
            case Type::Query:
                Query(static_cast<const QueryCmd&>(*command));
                break;
            case Type::Scope: {
                const auto& cmd = static_cast<const ScopeCmd&>(*command);
                if (cmd.IsPush())
                    PIXBeginEvent(list, PIX_COLOR_DEFAULT, "%s", cmd.ScopeName().c_str());
                else
                    PIXEndEvent(list);
                break;
            }
            case Type::QueueTransfer: {
                const auto& cmd = static_cast<const QueueTransferCmd&>(*command);
                if (cmd.IsImport()) {
                    for (const auto& b : cmd.ImportBuffers())
                        Transition(*static_cast<GpuBuffer*>(b.buffer.buffer), BufferState(b.state, false));
                    for (const auto& t : cmd.ImportTextures())
                        Transition(*static_cast<GpuTexture*>(t.texture.texture), TextureState(t.state, false), t.texture.mip_level, t.texture.num_mips, t.texture.array_layer, t.texture.num_array);
                }
                break; // Logical queues share one native queue; no ownership transfer.
            }
            case Type::TraceRay:
                throw std::runtime_error("D3D12 shader-table ray pipelines are not exposed by the current RHI; use inline RayQuery compute pipelines");
            default:
                throw std::runtime_error("Unsupported D3D12 command: " + command->name);
        }
    }
    if (!packet.queries.empty()) {
        packet.query_readback = context.Buffer(packet.queries.size() * 16, EBufferUsageFlags::NONE, D3D12_HEAP_TYPE_READBACK);
        for (const auto& query : packet.queries) {
            if (!query.ended)
                throw std::invalid_argument("Unclosed D3D12 query");
            const bool timestamp = query.token.Kind() == QueryKind::Timestamp;
            list->ResolveQueryData(timestamp ? packet.timestamps.Get() : packet.occlusion.Get(), timestamp ? D3D12_QUERY_TYPE_TIMESTAMP : D3D12_QUERY_TYPE_OCCLUSION, query.index, timestamp ? 2 : 1, static_cast<GpuBuffer*>(packet.query_readback.Get())->resource.Get(), uint64(query.index) * 8);
        }
    }
    DX_CHECK_HRESULT(list->Close());
}
} // namespace Moer::Render::D3D12
