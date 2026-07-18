#include "VulkanSerialGolden.h"

#include "VulkanRHIResource.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <tuple>
#include <type_traits>

namespace Moer::Render {
namespace {

template<typename T>
uint64_t NativeHandleKey(T _handle) {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(_handle));
    } else {
        return static_cast<uint64_t>(_handle);
    }
}

void AddUint3(StableRecordHash& _hash, const uint3& _value) {
    _hash.Add(_value.x);
    _hash.Add(_value.y);
    _hash.Add(_value.z);
}

void AddFloat(StableRecordHash& _hash, float _value) {
    _hash.Add(std::bit_cast<uint32_t>(_value));
}

void AppendLe(Array<uint8_t>& _bytes, uint64_t _value) {
    for (uint32_t index = 0; index < 8; ++index) {
        _bytes.push_back(static_cast<uint8_t>((_value >> (index * 8u)) & 0xffu));
    }
}

void AppendToken(Array<uint8_t>& _bytes, const StableSubmissionToken& _token) {
    AppendLe(_bytes, static_cast<uint64_t>(_token.kind));
    AppendLe(_bytes, _token.object_token);
    AppendLe(_bytes, _token.alias_token);
    AppendLe(_bytes, _token.alias_offset);
    AppendLe(_bytes, _token.extent);
    AppendLe(_bytes, _token.complete ? 1u : 0u);
}

} // namespace

struct VulkanSerialGoldenTrace::Impl {
    SubmissionTokenTable             tokens;
    SerialCommandLayerSectionBuilder command_builder;
    SerialBarrierSectionBuilder      barrier_builder;
    SerialDescriptorSectionBuilder   descriptor_builder;
    SerialQuerySectionBuilder        query_builder;
    UnorderedMap<uint64_t, StableSubmissionToken> native_buffers;
    UnorderedMap<uint64_t, StableSubmissionToken> native_images;
    UnorderedSet<const void*> expanded_bindless_arrays;
    UnorderedSet<const void*> failed_bindless_arrays;
    uint32_t current_command{std::numeric_limits<uint32_t>::max()};
    uint32_t descriptor_invocation{0};
    uint64_t descriptor_relative_begin{0};
    uint32_t query_event_ordinal{0};
    uint32_t opaque_count{0};
    uint32_t unresolved_count{0};
    uint64_t opaque_command_mask{0};
    uint64_t unresolved_command_mask{0};
    uint32_t unresolved_native_buffers{0};
    uint32_t unresolved_native_images{0};
    SerialBarrierItem first_unresolved_buffer_barrier{};
    bool has_unresolved_buffer_barrier{false};
    bool     finished{false};
    bool     priming{false};
    SerialGoldenSummary finished_summary{};

    StableSubmissionToken RegisterBuffer(Buffer* _buffer) {
        if (_buffer == nullptr) {
            return StableSubmissionToken::Null();
        }
        auto* const vk_buffer = ResourceCast(_buffer);
        const uint64_t native = NativeHandleKey(vk_buffer->GetHandle());
        const void* alias = native == 0 ? nullptr : reinterpret_cast<const void*>(uintptr_t(native));
        StableSubmissionToken token = tokens.Register(
            StableObjectKind::Buffer,
            _buffer,
            alias,
            0,
            _buffer->GetByteSize()
        );
        if (native == 0) {
            ++unresolved_count;
            return token;
        }
        const auto [found, inserted] = native_buffers.try_emplace(native, token);
        if (!inserted && found->second.object_token != token.object_token) {
            ++unresolved_count;
            found->second.complete = false;
        }
        return token;
    }

    StableSubmissionToken RegisterTexture(Texture* _texture) {
        if (_texture == nullptr) {
            return StableSubmissionToken::Null();
        }
        auto* const vk_texture = ResourceCast(_texture);
        const uint64_t native  = NativeHandleKey(vk_texture->GetHandle());
        const void* alias = native == 0 ? nullptr : reinterpret_cast<const void*>(uintptr_t(native));
        const uint3 extent = _texture->GetExtent();
        const uint64_t packed_extent = uint64_t(extent.x) * uint64_t(extent.y) * uint64_t(extent.z);
        StableSubmissionToken token = tokens.Register(
            StableObjectKind::Texture,
            _texture,
            alias,
            0,
            packed_extent
        );
        if (native == 0) {
            ++unresolved_count;
            return token;
        }
        const auto [found, inserted] = native_images.try_emplace(native, token);
        if (!inserted && found->second.object_token != token.object_token) {
            ++unresolved_count;
            found->second.complete = false;
        }
        return token;
    }

    StableSubmissionToken RegisterPipeline(const PipelineHandle& _pipeline) {
        if (!_pipeline.IsValid()) {
            ++unresolved_count;
            return tokens.Register(StableObjectKind::Opaque, nullptr);
        }
        const void* identity = reinterpret_cast<const void*>(uintptr_t(_pipeline.handle));
        return tokens.Register(StableObjectKind::Pipeline, identity);
    }

    StableSubmissionToken RegisterBindless(const BindlessArrayRef& _array) {
        if (!_array) {
            return StableSubmissionToken::Null();
        }
        StableSubmissionToken token =
            tokens.Register(StableObjectKind::Descriptor, _array.Get());
        if (failed_bindless_arrays.contains(_array.Get())) {
            ++unresolved_count;
            token.complete = false;
            return token;
        }
        if (expanded_bindless_arrays.contains(_array.Get())) {
            return token;
        }
        auto* const bindless = static_cast<VulkanBindlessArray*>(_array.Get());
        const uint32_t unresolved_before = unresolved_count;
        bool locked             = false;
        bool expansion_complete = true;
        try {
            bindless->Lock();
            locked = true;
            if (bindless->bindless_array_buffer == nullptr ||
                bindless->bindless_buffer_descs == nullptr ||
                bindless->bindless_texture_descs == nullptr) {
                expansion_complete = false;
            } else {
                RegisterBuffer(bindless->bindless_array_buffer);
                RegisterBuffer(bindless->bindless_buffer_descs);
                RegisterBuffer(bindless->bindless_texture_descs);
            }
            for (const VulkanBindlessArray::Handle& handle : bindless->handles) {
                if (handle.Ptr() == 0) {
                    continue;
                }
                if (handle.IsTexture()) {
                    RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(handle.Ptr())));
                } else if (handle.IsBuffer()) {
                    RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(handle.Ptr())));
                } else {
                    expansion_complete = false;
                }
            }
            expansion_complete &= unresolved_count == unresolved_before;
            if (expansion_complete) {
                expanded_bindless_arrays.insert(_array.Get());
            } else {
                failed_bindless_arrays.insert(_array.Get());
            }
        } catch (...) {
            if (locked) {
                bindless->Unlock();
            }
            expanded_bindless_arrays.erase(_array.Get());
            failed_bindless_arrays.insert(_array.Get());
            ++unresolved_count;
            token.complete = false;
            return token;
        }
        bindless->Unlock();
        if (!expansion_complete) {
            if (unresolved_count == unresolved_before) {
                ++unresolved_count;
            }
            token.complete = false;
        }
        return token;
    }

    StableSubmissionToken RegisterTlas(const RaytracingTlasRef& _tlas) {
        if (!_tlas) {
            return StableSubmissionToken::Null();
        }
        StableSubmissionToken token =
            tokens.Register(StableObjectKind::AccelerationStructure, _tlas.Get());
        auto* const acceleration = static_cast<VulkanAccelerationStructure*>(_tlas.Get());
        RegisterBuffer(acceleration->underlying_buffer.Get());
        return token;
    }

    void HashBufferView(
        StableRecordHash& _hash,
        const BufferView& _view,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(static_cast<uint64_t>(_view.format));
        _hash.Add(_view.GetByteOffset());
        _hash.Add(_view.GetByteSize());
        _hash.Add(_view.GetNumElements());
        _hash.Add(_view.GetStride());
        _resources.push_back(RegisterBuffer(_view.GetBuffer()));
    }

    void HashTextureView(
        StableRecordHash& _hash,
        const TextureView& _view,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(static_cast<uint64_t>(_view.format));
        AddUint3(_hash, _view.offset);
        AddUint3(_hash, _view.extent);
        _hash.Add(_view.mip_level);
        _hash.Add(_view.num_mips);
        _hash.Add(_view.array_layer);
        _hash.Add(_view.num_array);
        _resources.push_back(RegisterTexture(_view.GetTexture()));
    }

    void HashPipeline(
        StableRecordHash& _hash,
        const PipelineHandle& _pipeline,
        Array<StableSubmissionToken>& _resources
    ) {
        _resources.push_back(RegisterPipeline(_pipeline));
        _hash.Add(_pipeline.valid_bits);
        _hash.Add(static_cast<uint64_t>(static_cast<int64_t>(_pipeline.constant_idx)));
        _hash.Add(_pipeline.binding_infos.size());
        for (const ParamInfoFlags& info : _pipeline.binding_infos) {
            _hash.Add(info.state_flags);
            _hash.Add(info.pipeline_flags);
        }
    }

    void HashArgument(
        StableRecordHash& _hash,
        const TArg& _arg,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(_arg.index());
        std::visit(
            Overload{
                [&](TInvalidArg _invalid) { _hash.Add(_invalid); },
                [&](const BufferView& _view) { HashBufferView(_hash, _view, _resources); },
                [&](const TextureView& _view) { HashTextureView(_hash, _view, _resources); },
                [&](const TextureViewArray& _views) {
                    _hash.Add(_views.size());
                    for (const TextureView& view : _views) {
                        HashTextureView(_hash, view, _resources);
                    }
                },
                [&](const BufferViewArray& _views) {
                    _hash.Add(_views.size());
                    for (const BufferView& view : _views) {
                        HashBufferView(_hash, view, _resources);
                    }
                },
                [&](const Sampler& _sampler) {
                    _hash.Add(static_cast<uint64_t>(_sampler.filter));
                    _hash.Add(static_cast<uint64_t>(_sampler.address_mode));
                    _hash.Add(static_cast<uint64_t>(_sampler.compare_function));
                },
                [&](const BindlessArrayRef& _array) {
                    _resources.push_back(RegisterBindless(_array));
                },
                [&](const RaytracingTlasRef& _tlas) {
                    _resources.push_back(RegisterTlas(_tlas));
                }
            },
            _arg
        );
    }

    void HashArguments(
        StableRecordHash& _hash,
        const ArrayArguments& _args,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(_args.args.size());
        _hash.Add(_args.constants.size());
        _hash.Add(_args.b_use_bindless ? 1u : 0u);
        for (const TArg& arg : _args.args) {
            HashArgument(_hash, arg, _resources);
        }
    }

    const ArrayArguments* ResolveArguments(
        const TShaderArgArray& _args,
        const TCachedArgArray& _cached_args,
        StableRecordHash& _hash
    ) {
        _hash.Add(_args.index());
        if (std::holds_alternative<ArrayArguments>(_args)) {
            return &std::get<ArrayArguments>(_args);
        }
        if (std::holds_alternative<ArrayArgReference>(_args)) {
            const uint32_t index = std::get<ArrayArgReference>(_args).handle;
            _hash.Add(index);
            if (index < _cached_args.size()) {
                return &_cached_args[index];
            }
            ++unresolved_count;
            return nullptr;
        }
        return nullptr;
    }

    void HashRenderPass(
        StableRecordHash& _hash,
        const RenderPassInfo& _pass,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(_pass.color_attachments.size());
        for (const ColorAttachment& attachment : _pass.color_attachments) {
            _resources.push_back(RegisterTexture(attachment.target));
            _hash.Add(static_cast<uint64_t>(attachment.action));
            AddFloat(_hash, attachment.clear_color.x);
            AddFloat(_hash, attachment.clear_color.y);
            AddFloat(_hash, attachment.clear_color.z);
            AddFloat(_hash, attachment.clear_color.w);
            _hash.Add(attachment.mip_level);
            _hash.Add(attachment.array_layer);
        }
        const DepthAttachment& depth = _pass.depth_attachment;
        _hash.Add(depth.Valid() ? 1u : 0u);
        if (depth.Valid()) {
            _resources.push_back(RegisterTexture(depth.target));
            _hash.Add(depth.array_layer);
            _hash.Add(depth.mip_level);
            _hash.Add(static_cast<uint64_t>(depth.action));
            AddFloat(_hash, depth.clear_depth);
            _hash.Add(depth.clear_stencil);
        }
        _hash.Add(static_cast<uint64_t>(static_cast<int64_t>(_pass.render_area.offset.x)));
        _hash.Add(static_cast<uint64_t>(static_cast<int64_t>(_pass.render_area.offset.y)));
        _hash.Add(_pass.render_area.extent.width);
        _hash.Add(_pass.render_area.extent.height);
        _hash.Add(_pass.viewport_cnt);
    }

    void HashIndirect(
        StableRecordHash& _hash,
        const IndirectDrawParam& _indirect,
        Array<StableSubmissionToken>& _resources
    ) {
        HashBufferView(_hash, _indirect.buffer, _resources);
        _hash.Add(_indirect.count_buffer.has_value() ? 1u : 0u);
        if (_indirect.count_buffer.has_value()) {
            HashBufferView(_hash, *_indirect.count_buffer, _resources);
        }
        _hash.Add(_indirect.count);
        _hash.Add(_indirect.stride);
    }

    void HashMeshData(
        StableRecordHash& _hash,
        const MeshDrawData& _mesh,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(_mesh.vtx_views.size());
        for (const VertexBuffer& vertex : _mesh.vtx_views) {
            _resources.push_back(RegisterBuffer(vertex.buffer));
            _hash.Add(vertex.offset);
        }
        _hash.Add(_mesh.idx_view.index());
        if (std::holds_alternative<IndexBuffer>(_mesh.idx_view)) {
            const IndexBuffer& index = std::get<IndexBuffer>(_mesh.idx_view);
            HashBufferView(_hash, index.buffer, _resources);
            _hash.Add(static_cast<uint64_t>(index.stride));
        } else {
            _hash.Add(std::get<uint>(_mesh.idx_view));
        }
        _hash.Add(_mesh.draw_params.size());
        for (const SingleDrawParam& draw : _mesh.draw_params) {
            _hash.Add(draw.index_cnt);
            _hash.Add(draw.instance_cnt);
            _hash.Add(draw.first_index);
            _hash.Add(draw.vertex_offset);
            _hash.Add(draw.first_instance);
        }
        _hash.Add(_mesh.indirect_draw_param.has_value() ? 1u : 0u);
        if (_mesh.indirect_draw_param.has_value()) {
            HashIndirect(_hash, *_mesh.indirect_draw_param, _resources);
        }
    }

    void HashDispatchMesh(
        StableRecordHash& _hash,
        const DispatchMeshData& _mesh,
        Array<StableSubmissionToken>& _resources
    ) {
        _hash.Add(_mesh.draw_param.index());
        if (std::holds_alternative<IndirectDrawParam>(_mesh.draw_param)) {
            HashIndirect(_hash, std::get<IndirectDrawParam>(_mesh.draw_param), _resources);
        } else {
            AddUint3(_hash, std::get<Vector3ui>(_mesh.draw_param));
        }
    }

    void AddOpaqueResource(Array<StableSubmissionToken>& _resources) {
        ++opaque_count;
        _resources.push_back(tokens.Register(StableObjectKind::Opaque, nullptr));
    }

    void RecordCommand(
        const Command* _command,
        uint32_t _original_ordinal,
        const TCachedArgArray& _cached_args
    ) {
        if (_command == nullptr) {
            ++unresolved_count;
            if (!priming) {
                AddOpaqueResourceScratch(Command::EType::Custom, _original_ordinal);
            }
            return;
        }

        const uint32_t opaque_before     = opaque_count;
        const uint32_t unresolved_before = unresolved_count;
        StableRecordHash hash;
        Array<StableSubmissionToken> resources;
        hash.Add(_original_ordinal);
        hash.Add(static_cast<uint64_t>(_command->GetQueueType()));

        switch (_command->Type()) {
            case Command::EType::UploadBuffer: {
                const auto& command = *static_cast<const UploadBufferCmd*>(_command);
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.Handle()))));
                hash.Add(command.Offset());
                hash.Add(command.ByteSize());
                hash.Add(command.Data().size());
                break;
            }
            case Command::EType::CopyBackBuffer: {
                const auto& command = *static_cast<const CopyBackBufferCmd*>(_command);
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.Handle()))));
                hash.Add(command.Offset());
                hash.Add(command.ByteSize());
                break;
            }
            case Command::EType::BufferToBuffer: {
                const auto& command = *static_cast<const CopyBufferCmd*>(_command);
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.SrcHandle()))));
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.DstHandle()))));
                hash.Add(command.SrcOffset());
                hash.Add(command.DstOffset());
                hash.Add(command.ByteSize());
                break;
            }
            case Command::EType::BufferToTexture: {
                const auto& command = *static_cast<const CopyBufferToTextureCmd*>(_command);
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.SrcHandle()))));
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.DstHandle()))));
                hash.Add(static_cast<uint64_t>(command.Format()));
                hash.Add(command.SrcOffset());
                AddUint3(hash, command.DstOffset());
                AddUint3(hash, command.Size());
                hash.Add(command.ByteSize());
                hash.Add(command.MipLevel());
                hash.Add(command.ArrayLayer());
                break;
            }
            case Command::EType::TextureToBuffer: {
                const auto& command = *static_cast<const CopyTextureToBufferCmd*>(_command);
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.SrcHandle()))));
                resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(command.DstHandle()))));
                hash.Add(static_cast<uint64_t>(command.Format()));
                AddUint3(hash, command.SrcOffset());
                hash.Add(command.DstOffset());
                AddUint3(hash, command.Size());
                hash.Add(command.MipLevel());
                hash.Add(command.ArrayLayer());
                if (command.ArrayLayer() != 0) {
                    AddOpaqueResource(resources);
                }
                break;
            }
            case Command::EType::UploadTexture: {
                const auto& command = *static_cast<const UploadTextureCmd*>(_command);
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.Handle()))));
                hash.Add(static_cast<uint64_t>(command.Format()));
                hash.Add(command.MipLevel());
                hash.Add(command.ArrayLayer());
                AddUint3(hash, command.Offset());
                AddUint3(hash, command.Size());
                hash.Add(command.Data().size());
                break;
            }
            case Command::EType::TextureToTexture: {
                const auto& command = *static_cast<const CopyTextureCmd*>(_command);
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.SrcHandle()))));
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.DstHandle()))));
                hash.Add(static_cast<uint64_t>(command.Format()));
                hash.Add(command.SrcMipLevel());
                hash.Add(command.DstMipLevel());
                AddUint3(hash, command.SrcOffset());
                AddUint3(hash, command.DstOffset());
                AddUint3(hash, command.Size());
                break;
            }
            case Command::EType::CopyBackTexture: {
                const auto& command = *static_cast<const CopyBackTextureCmd*>(_command);
                resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(command.Handle()))));
                hash.Add(command.MipLevel());
                AddUint3(hash, command.Offset());
                AddUint3(hash, command.Size());
                hash.Add(command.Data().size());
                break;
            }
            case Command::EType::ShaderDispatch: {
                const auto& command = *static_cast<const DispatchCmd*>(_command);
                HashPipeline(hash, command.Pipeline(), resources);
                const ArrayArguments* args = nullptr;
                try {
                    args = &command.Args(_cached_args);
                } catch (...) {
                    ++unresolved_count;
                }
                if (args != nullptr) {
                    HashArguments(hash, *args, resources);
                } else {
                    AddOpaqueResource(resources);
                }
                const auto param = command.Param();
                hash.Add(param.index());
                if (std::holds_alternative<uint3>(param)) {
                    AddUint3(hash, std::get<uint3>(param));
                } else {
                    HashBufferView(hash, std::get<DispatchIndirectParam>(param).indirect, resources);
                }
                break;
            }
            case Command::EType::BuildAccel:
            case Command::EType::BuildTLAS:
            case Command::EType::TraceRay:
            case Command::EType::SetGeometryPassDrawState:
            case Command::EType::Custom:
                AddOpaqueResource(resources);
                break;
            case Command::EType::Barrier: {
                const auto& command = *static_cast<const BarrierCmd*>(_command);
                hash.Add(command.IsQueueTransition() ? 1u : 0u);
                hash.Add(static_cast<uint64_t>(command.GetSrcQueue()));
                hash.Add(static_cast<uint64_t>(command.GetDstQueue()));
                auto hash_texture_barriers = [&](const auto& barriers) {
                    hash.Add(barriers.size());
                    for (const TextureBarrier& barrier : barriers) {
                        resources.push_back(RegisterTexture(reinterpret_cast<Texture*>(uintptr_t(barrier.handle))));
                        hash.Add(static_cast<uint64_t>(barrier.state));
                        hash.Add(static_cast<uint64_t>(barrier.pass_type));
                        hash.Add(barrier.mip_level);
                        hash.Add(barrier.mip_cnt);
                        hash.Add(barrier.array_layer);
                        hash.Add(barrier.array_cnt);
                    }
                };
                auto hash_buffer_barriers = [&](const auto& barriers) {
                    hash.Add(barriers.size());
                    for (const BufferBarrier& barrier : barriers) {
                        resources.push_back(RegisterBuffer(reinterpret_cast<Buffer*>(uintptr_t(barrier.handle))));
                        hash.Add(static_cast<uint64_t>(barrier.state));
                        hash.Add(static_cast<uint64_t>(barrier.pass_type));
                        hash.Add(barrier.offset);
                        hash.Add(barrier.byte_size);
                    }
                };
                hash_texture_barriers(command.ReadTextures());
                hash_texture_barriers(command.WriteTextures());
                hash_buffer_barriers(command.ReadBuffers());
                hash_buffer_barriers(command.WriteBuffers());
                break;
            }
            case Command::EType::QueueTransfer: {
                const auto& command = *static_cast<const QueueTransferCmd*>(_command);
                hash.Add(command.IsImport() ? 1u : 0u);
                hash.Add(static_cast<uint64_t>(
                    command.IsImport() ? command.src_queue : command.dst_queue
                ));
                hash.Add(command.ImportTextures().size());
                for (const ImportTexture& item : command.ImportTextures()) {
                    HashTextureView(hash, item.texture, resources);
                    hash.Add(static_cast<uint64_t>(item.state));
                }
                hash.Add(command.ExportTextures().size());
                for (const ExportTexture& item : command.ExportTextures()) {
                    HashTextureView(hash, item.texture, resources);
                    hash.Add(static_cast<uint64_t>(item.state));
                }
                hash.Add(command.ImportBuffers().size());
                for (const ImportBuffer& item : command.ImportBuffers()) {
                    HashBufferView(hash, item.buffer, resources);
                    hash.Add(static_cast<uint64_t>(item.state));
                }
                hash.Add(command.ExportBuffers().size());
                for (const ExportBuffer& item : command.ExportBuffers()) {
                    HashBufferView(hash, item.buffer, resources);
                    hash.Add(static_cast<uint64_t>(item.state));
                }
                break;
            }
            case Command::EType::SetDrawState: {
                const auto& command = *static_cast<const SetDrawStateCmd*>(_command);
                HashPipeline(hash, command.Pipeline(), resources);
                HashArguments(hash, command.Args(), resources);
                HashRenderPass(hash, command.RenderPassInfo(), resources);
                hash.Add(command.DrawData().size());
                for (const MeshDrawData& mesh : command.DrawData()) {
                    HashMeshData(hash, mesh, resources);
                }
                break;
            }
            case Command::EType::MultiDraw: {
                const auto& command = *static_cast<const MultiDrawCmd*>(_command);
                HashRenderPass(hash, command.RenderPassInfo(), resources);
                hash.Add(command.draw_batch.draw_cmds.size());
                for (const DrawBatchElement& draw : command.draw_batch.draw_cmds) {
                    HashPipeline(hash, draw.handle, resources);
                    if (const ArrayArguments* args = ResolveArguments(draw.args, _cached_args, hash)) {
                        HashArguments(hash, *args, resources);
                    } else {
                        AddOpaqueResource(resources);
                    }
                    hash.Add(draw.mesh_dispatch_data.index());
                    if (std::holds_alternative<Array<MeshDrawData>>(draw.mesh_dispatch_data)) {
                        const auto& meshes = std::get<Array<MeshDrawData>>(draw.mesh_dispatch_data);
                        hash.Add(meshes.size());
                        for (const MeshDrawData& mesh : meshes) {
                            HashMeshData(hash, mesh, resources);
                        }
                    } else {
                        const auto& meshes = std::get<Array<DispatchMeshData>>(draw.mesh_dispatch_data);
                        hash.Add(meshes.size());
                        for (const DispatchMeshData& mesh : meshes) {
                            HashDispatchMesh(hash, mesh, resources);
                        }
                    }
                }
                break;
            }
            case Command::EType::UpdateBindlessArray: {
                const auto& command = *static_cast<const UpdateBindlessArrayCmd*>(_command);
                resources.push_back(tokens.Register(StableObjectKind::Descriptor, command.Handle()));
                hash.Add(command.UpdateCommands().size());
                for (const BindlessArray::UpdateCmd& update : command.UpdateCommands()) {
                    hash.Add(update.index());
                    std::visit(
                        Overload{
                            [&](const BindlessArray::TextureUpdateInfo& info) {
                                resources.push_back(RegisterTexture(info.texture.Get()));
                                hash.Add(static_cast<uint64_t>(info.sampler.filter));
                                hash.Add(static_cast<uint64_t>(info.sampler.address_mode));
                                hash.Add(static_cast<uint64_t>(info.sampler.compare_function));
                                hash.Add(static_cast<uint64_t>(info.format));
                                hash.Add(info.array_idx);
                                hash.Add(info.slot);
                                hash.Add(info.mip_level);
                                hash.Add(info.num_mips);
                                hash.Add(info.array_layer);
                                hash.Add(info.array_count);
                                hash.Add(info.free ? 1u : 0u);
                            },
                            [&](const BindlessArray::BufferUpdateInfo& info) {
                                resources.push_back(RegisterBuffer(info.buffer.Get()));
                                hash.Add(info.array_idx);
                                hash.Add(info.slot);
                                hash.Add(static_cast<uint64_t>(info.format));
                                hash.Add(info.free ? 1u : 0u);
                            },
                            [&](const BindlessArray::InvalidUpdateInfo& info) {
                                hash.Add(info.array_idx);
                            }
                        },
                        update
                    );
                }
                auto hash_indices = [&](const auto& indices) {
                    hash.Add(indices.size());
                    for (const auto& [array_index, data_offset] : indices) {
                        hash.Add(array_index);
                        hash.Add(data_offset);
                    }
                };
                hash_indices(command.ArrayIndicesData());
                hash_indices(command.BufferIndicesData());
                hash_indices(command.TextureIndicesData());
                hash.Add(command.ArrayDataSize());
                hash.Add(command.BufferDataSize());
                hash.Add(command.TextureDataSize());
                break;
            }
            case Command::EType::ClearResource: {
                const auto& command = *static_cast<const ClearResourceCmd*>(_command);
                hash.Add(command.Resource().index());
                if (command.IsBuffer()) {
                    HashBufferView(hash, command.Buffer(), resources);
                } else {
                    HashTextureView(hash, command.Texture(), resources);
                }
                hash.Add(command.ClearValue().index());
                if (command.IsUInt()) {
                    hash.Add(command.UIntValue());
                } else {
                    AddFloat(hash, command.Float4Value().x);
                    AddFloat(hash, command.Float4Value().y);
                    AddFloat(hash, command.Float4Value().z);
                    AddFloat(hash, command.Float4Value().w);
                }
                break;
            }
            case Command::EType::Scope: {
                const auto& command = *static_cast<const ScopeCmd*>(_command);
                hash.Add(command.IsPush() ? 1u : 0u);
                hash.Add(command.QueryTimestamp() ? 1u : 0u);
                hash.AddString(command.ScopeName());
                AddFloat(hash, command.Color().x);
                AddFloat(hash, command.Color().y);
                AddFloat(hash, command.Color().z);
                AddFloat(hash, command.Color().w);
                break;
            }
            case Command::EType::Count:
                AddOpaqueResource(resources);
                break;
        }

        if (!priming) {
            command_builder.AddCommand(_command->Type(), resources, hash.Value());
            const uint32_t type_index = static_cast<uint32_t>(_command->Type());
            if (type_index < 64 && opaque_count != opaque_before) {
                opaque_command_mask |= uint64_t(1) << type_index;
            }
            if (type_index < 64 && unresolved_count != unresolved_before) {
                unresolved_command_mask |= uint64_t(1) << type_index;
            }
        }
    }

    void AddOpaqueResourceScratch(Command::EType _type, uint32_t _ordinal) {
        StableRecordHash hash;
        hash.Add(_ordinal);
        Array<StableSubmissionToken> resources;
        AddOpaqueResource(resources);
        command_builder.AddCommand(_type, resources, hash.Value());
    }

    Array<uint8_t> DescriptorPrefix(const StableSubmissionToken& _pipeline) const {
        Array<uint8_t> bytes;
        bytes.reserve(80);
        AppendLe(bytes, current_command);
        AppendLe(bytes, descriptor_invocation);
        AppendLe(bytes, descriptor_relative_begin);
        AppendToken(bytes, _pipeline);
        return bytes;
    }

    void AddDescriptorViewBytes(Array<uint8_t>& _bytes, const TextureView& _view) const {
        AppendLe(_bytes, static_cast<uint64_t>(_view.format));
        AppendLe(_bytes, _view.mip_level);
        AppendLe(_bytes, _view.num_mips);
        AppendLe(_bytes, _view.array_layer);
        AppendLe(_bytes, _view.num_array);
        AppendLe(_bytes, _view.offset.x);
        AppendLe(_bytes, _view.offset.y);
        AppendLe(_bytes, _view.offset.z);
        AppendLe(_bytes, _view.extent.x);
        AppendLe(_bytes, _view.extent.y);
        AppendLe(_bytes, _view.extent.z);
    }

    void AddDescriptorItem(
        uint32_t _bind_point,
        uint32_t _set,
        uint32_t _binding,
        uint32_t _array_element,
        uint32_t _descriptor_type,
        uint32_t _param_idx,
        uint32_t _declared_descriptor_count,
        const TArg* _arg,
        uint32_t _arg_array_element,
        const StableSubmissionToken& _pipeline,
        bool _complete = true
    ) {
        StableSubmissionToken resource = StableSubmissionToken::Null();
        uint64_t offset = 0;
        uint64_t range  = 0;
        Array<uint8_t> bytes = DescriptorPrefix(_pipeline);
        AppendLe(bytes, _arg_array_element);

        if (_arg == nullptr) {
            ++unresolved_count;
            resource = tokens.Register(StableObjectKind::Opaque, nullptr);
            _complete = false;
        } else if (std::holds_alternative<BufferView>(*_arg)) {
            const BufferView& view = std::get<BufferView>(*_arg);
            resource = RegisterBuffer(view.GetBuffer());
            offset   = view.GetByteOffset();
            range    = view.GetByteSize();
            AppendLe(bytes, view.GetStride());
            AppendLe(bytes, static_cast<uint64_t>(view.format));
        } else if (std::holds_alternative<BufferViewArray>(*_arg)) {
            const BufferViewArray& views = std::get<BufferViewArray>(*_arg);
            if (_arg_array_element >= views.size()) {
                ++unresolved_count;
                resource = tokens.Register(StableObjectKind::Opaque, nullptr);
                _complete = false;
            } else {
                const BufferView& view = views[_arg_array_element];
                resource = RegisterBuffer(view.GetBuffer());
                offset   = view.GetByteOffset();
                range    = view.GetByteSize();
                AppendLe(bytes, view.GetStride());
                AppendLe(bytes, static_cast<uint64_t>(view.format));
            }
        } else if (std::holds_alternative<TextureView>(*_arg)) {
            const TextureView& view = std::get<TextureView>(*_arg);
            resource = RegisterTexture(view.GetTexture());
            AddDescriptorViewBytes(bytes, view);
        } else if (std::holds_alternative<TextureViewArray>(*_arg)) {
            const TextureViewArray& views = std::get<TextureViewArray>(*_arg);
            if (_arg_array_element >= views.size()) {
                ++unresolved_count;
                resource = tokens.Register(StableObjectKind::Opaque, nullptr);
                _complete = false;
            } else {
                const TextureView& view = views[_arg_array_element];
                resource = RegisterTexture(view.GetTexture());
                AddDescriptorViewBytes(bytes, view);
            }
        } else if (std::holds_alternative<Sampler>(*_arg)) {
            const Sampler& sampler = std::get<Sampler>(*_arg);
            AppendLe(bytes, static_cast<uint64_t>(sampler.filter));
            AppendLe(bytes, static_cast<uint64_t>(sampler.address_mode));
            AppendLe(bytes, static_cast<uint64_t>(sampler.compare_function));
        } else if (std::holds_alternative<BindlessArrayRef>(*_arg)) {
            resource = RegisterBindless(std::get<BindlessArrayRef>(*_arg));
        } else if (std::holds_alternative<RaytracingTlasRef>(*_arg)) {
            resource = RegisterTlas(std::get<RaytracingTlasRef>(*_arg));
        } else {
            ++opaque_count;
            resource = tokens.Register(StableObjectKind::Opaque, nullptr);
            _complete = false;
        }

        descriptor_builder.Add(
            descriptor_invocation,
            _bind_point,
            _set,
            _binding,
            _array_element,
            _descriptor_type,
            _param_idx,
            _declared_descriptor_count,
            resource,
            offset,
            range,
            bytes,
            _complete
        );
    }

    void RecordDescriptorBind(
        const PipelineHandle& _pipeline,
        const ArrayArguments& _args,
        const VulkanPipelineParamBinder& _binder
    ) {
        const StableSubmissionToken pipeline_token = RegisterPipeline(_pipeline);
        const uint32_t invalid_layout_value = std::numeric_limits<uint32_t>::max();
        uint32_t pipeline_bind_point = invalid_layout_value;
        if (_pipeline.IsValid()) {
            pipeline_bind_point = static_cast<uint32_t>(
                reinterpret_cast<VulkanPipelineState*>(_pipeline.handle)->GetPipelineBindPoint()
            );
        }

        auto bindless_bind_point = [&](uint32_t _set) {
            for (const DescBufferOffsetInfo& offset : _binder.desc_buffer_offsets) {
                if (offset.set == _set) {
                    return static_cast<uint32_t>(offset.bind_point);
                }
            }
            ++unresolved_count;
            return pipeline_bind_point;
        };

        // One sentinel per actual BindDescriptors call preserves empty binds and
        // multiple binds issued by a single MultiDraw command.
        Array<uint8_t> sentinel = DescriptorPrefix(pipeline_token);
        AppendLe(sentinel, _pipeline.valid_bits);
        AppendLe(sentinel, _args.constants.size());
        descriptor_builder.Add(
            descriptor_invocation,
            pipeline_bind_point,
            std::numeric_limits<uint32_t>::max(),
            std::numeric_limits<uint32_t>::max(),
            0,
            std::numeric_limits<uint32_t>::max() - 1u,
            invalid_layout_value,
            0,
            StableSubmissionToken::Null(),
            0,
            0,
            sentinel
        );

        Array<uint32_t> sets;
        sets.reserve(_binder.set_binders.size());
        for (const auto& [set, unused] : _binder.set_binders) {
            (void)unused;
            sets.push_back(set);
        }
        std::sort(sets.begin(), sets.end());

        for (const uint32_t set : sets) {
            const TBinder& binder = _binder.set_binders.at(set);
            std::visit(
                Overload{
                    [&](const VulkanBindlessSetArray& bindless) {
                        const TArg* arg = bindless.param_idx < _args.args.size()
                                              ? &_args.args[bindless.param_idx]
                                              : nullptr;
                        AddDescriptorItem(
                            bindless_bind_point(set),
                            set, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            bindless.param_idx, bindless.descriptor_count,
                            arg, 0, pipeline_token
                        );
                    },
                    [&](const VulkanBindlessSetSampler& bindless) {
                        const TArg* arg = bindless.param_idx < _args.args.size()
                                              ? &_args.args[bindless.param_idx]
                                              : nullptr;
                        AddDescriptorItem(
                            bindless_bind_point(set),
                            set, 0, 0, VK_DESCRIPTOR_TYPE_SAMPLER,
                            bindless.param_idx, bindless.descriptor_count,
                            arg, 0, pipeline_token
                        );
                    },
                    [&](const VulkanBindlessSetImage& bindless) {
                        const TArg* arg = bindless.param_idx < _args.args.size()
                                              ? &_args.args[bindless.param_idx]
                                              : nullptr;
                        AddDescriptorItem(
                            bindless_bind_point(set),
                            set, 0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                            bindless.param_idx, bindless.descriptor_count,
                            arg, 0, pipeline_token
                        );
                    },
                    [&](const VulkanDescriptorSetBinder& descriptors) {
                        const size_t count = std::min({
                            descriptors.writers.size(),
                            descriptors.bind_infos.size(),
                            descriptors.binding_infos.size()
                        });
                        if (count != descriptors.writers.size() ||
                            count != descriptors.bind_infos.size() ||
                            count != descriptors.binding_infos.size()) {
                            ++unresolved_count;
                        }
                        for (size_t index = 0; index < count; ++index) {
                            const VkWriteDescriptorSet& writer = descriptors.writers[index];
                            if (writer.descriptorCount == 0) {
                                continue;
                            }
                            const VulkanDescriptorInfo& info = descriptors.bind_infos[index];
                            if (info.param_idx >= _args.args.size() ||
                                info.param_idx >= 64 ||
                                !(_pipeline.valid_bits & (uint64_t(1) << info.param_idx))) {
                                if (info.param_idx >= _args.args.size() || info.param_idx >= 64) {
                                    ++unresolved_count;
                                }
                                continue;
                            }
                            const TArg& arg = _args.args[info.param_idx];
                            if (writer.descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
                                ++opaque_count;
                                AddDescriptorItem(
                                    static_cast<uint32_t>(descriptors.bind_point),
                                    set,
                                    descriptors.binding_infos[index].binding,
                                    0,
                                    writer.descriptorType,
                                    info.param_idx,
                                    writer.descriptorCount,
                                    nullptr,
                                    0,
                                    pipeline_token,
                                    false
                                );
                                continue;
                            }
                            uint32_t actual_count = writer.descriptorCount;
                            if (std::holds_alternative<TextureViewArray>(arg)) {
                                actual_count = std::min<uint32_t>(
                                    actual_count,
                                    static_cast<uint32_t>(std::get<TextureViewArray>(arg).size())
                                );
                            }
                            for (uint32_t array_index = 0; array_index < actual_count; ++array_index) {
                                AddDescriptorItem(
                                    static_cast<uint32_t>(descriptors.bind_point),
                                    set,
                                    descriptors.binding_infos[index].binding,
                                    array_index,
                                    writer.descriptorType,
                                    info.param_idx,
                                    writer.descriptorCount,
                                    &arg,
                                    array_index,
                                    pipeline_token
                                );
                            }
                        }
                    }
                },
                binder
            );
        }

        if (_binder.push_constants_info.size > 0) {
            Array<uint8_t> bytes = DescriptorPrefix(pipeline_token);
            AppendLe(bytes, _binder.push_constants_info.stageFlags);
            AppendLe(bytes, _binder.push_constants_info.offset);
            AppendLe(bytes, _binder.push_constants_info.size);
            AppendLe(bytes, _args.constants.size());
            descriptor_builder.Add(
                descriptor_invocation,
                pipeline_bind_point,
                std::numeric_limits<uint32_t>::max(),
                std::numeric_limits<uint32_t>::max(),
                0,
                std::numeric_limits<uint32_t>::max(),
                invalid_layout_value,
                0,
                StableSubmissionToken::Null(),
                0,
                _binder.push_constants_info.size,
                bytes
            );
        }
        ++descriptor_invocation;
    }

    void RecordDescriptorsForCommand(
        const Command* _command,
        const TCachedArgArray& _cached_args,
        uint64_t _relative_descriptor_begin,
        uint64_t _actual_descriptor_bytes
    ) {
        const uint32_t opaque_before     = opaque_count;
        const uint32_t unresolved_before = unresolved_count;
        if (_command == nullptr) {
            ++unresolved_count;
            return;
        }
        descriptor_relative_begin = _relative_descriptor_begin;
        uint64_t expected_descriptor_bytes = 0;
        bool     validates_descriptor_bytes = false;
        auto record = [&](const PipelineHandle& pipeline, const ArrayArguments* args) {
            validates_descriptor_bytes = true;
            if (args == nullptr || !pipeline.IsValid()) {
                ++unresolved_count;
                return;
            }
            auto* const vk_pipeline = reinterpret_cast<VulkanPipelineState*>(pipeline.handle);
            if (vk_pipeline->bind_template == nullptr) {
                ++unresolved_count;
                return;
            }
            uint64_t bind_descriptor_bytes = 0;
            for (const auto& [set, binder] : vk_pipeline->bind_template->set_binders) {
                (void)set;
                if (std::holds_alternative<VulkanDescriptorSetBinder>(binder)) {
                    bind_descriptor_bytes +=
                        std::get<VulkanDescriptorSetBinder>(binder).size;
                }
            }
            descriptor_relative_begin = _relative_descriptor_begin + expected_descriptor_bytes;
            RecordDescriptorBind(pipeline, *args, *vk_pipeline->bind_template);
            expected_descriptor_bytes += bind_descriptor_bytes;
        };

        switch (_command->Type()) {
            case Command::EType::ShaderDispatch: {
                const auto& command = *static_cast<const DispatchCmd*>(_command);
                const ArrayArguments* args = nullptr;
                try {
                    args = &command.Args(_cached_args);
                } catch (...) {
                    ++unresolved_count;
                }
                record(command.Pipeline(), args);
                break;
            }
            case Command::EType::SetDrawState: {
                const auto& command = *static_cast<const SetDrawStateCmd*>(_command);
                record(command.Pipeline(), &command.Args());
                break;
            }
            case Command::EType::MultiDraw: {
                const auto& command = *static_cast<const MultiDrawCmd*>(_command);
                for (const DrawBatchElement& draw : command.draw_batch.draw_cmds) {
                    StableRecordHash ignored;
                    record(draw.handle, ResolveArguments(draw.args, _cached_args, ignored));
                }
                break;
            }
            case Command::EType::TraceRay: {
                const auto& command = *static_cast<const TraceRayCmd*>(_command);
                record(command.Pipeline(), &command.Args());
                break;
            }
            default:
                break;
        }
        if (validates_descriptor_bytes && expected_descriptor_bytes != _actual_descriptor_bytes) {
            ++unresolved_count;
        }
        const uint32_t type_index = static_cast<uint32_t>(_command->Type());
        if (type_index < 64 && opaque_count != opaque_before) {
            opaque_command_mask |= uint64_t(1) << type_index;
        }
        if (type_index < 64 && unresolved_count != unresolved_before) {
            unresolved_command_mask |= uint64_t(1) << type_index;
        }
    }
};

VulkanSerialGoldenTrace::VulkanSerialGoldenTrace() : impl(std::make_unique<Impl>()) {}
VulkanSerialGoldenTrace::~VulkanSerialGoldenTrace() = default;

void VulkanSerialGoldenTrace::BeginLayer(uint32_t _layer_ordinal) {
    impl->command_builder.BeginLayer(_layer_ordinal);
}

void VulkanSerialGoldenTrace::PrimeCommandResources(
    const Command* _command,
    uint32_t _original_ordinal,
    const TCachedArgArray& _cached_args
) {
    const uint32_t opaque_before      = impl->opaque_count;
    const uint32_t unresolved_before  = impl->unresolved_count;
    bool priming_exception = false;
    impl->priming = true;
    try {
        impl->RecordCommand(_command, _original_ordinal, _cached_args);
    } catch (...) {
        priming_exception = true;
    }
    impl->priming = false;
    // Priming establishes deterministic token order. The reordered layer pass
    // owns issue accounting so each opaque/unresolved command is counted once.
    impl->opaque_count     = opaque_before;
    impl->unresolved_count = unresolved_before;
    if (priming_exception) {
        ++impl->unresolved_count;
        if (_command != nullptr) {
            const uint32_t type_index = static_cast<uint32_t>(_command->Type());
            if (type_index < 64) {
                impl->unresolved_command_mask |= uint64_t(1) << type_index;
            }
        }
    }
}

void VulkanSerialGoldenTrace::RecordCommand(
    const Command* _command,
    uint32_t _original_ordinal,
    const TCachedArgArray& _cached_args
) {
    impl->RecordCommand(_command, _original_ordinal, _cached_args);
}

void VulkanSerialGoldenTrace::RegisterDerivedResources(const Command* _command) {
    if (_command == nullptr) {
        MarkUnresolved();
        return;
    }
    switch (_command->Type()) {
        case Command::EType::UploadBuffer:
            impl->RegisterBuffer(static_cast<const UploadBufferCmd*>(_command)->staging_buffer.GetBuffer());
            break;
        case Command::EType::UploadTexture:
            impl->RegisterBuffer(static_cast<const UploadTextureCmd*>(_command)->staging_buffer.GetBuffer());
            break;
        case Command::EType::CopyBackBuffer:
            impl->RegisterBuffer(static_cast<const CopyBackBufferCmd*>(_command)->staging_buffer.GetBuffer());
            break;
        case Command::EType::CopyBackTexture:
            impl->RegisterBuffer(static_cast<const CopyBackTextureCmd*>(_command)->staging_buffer.GetBuffer());
            break;
        case Command::EType::BuildAccel:
            impl->RegisterBuffer(static_cast<const BuildAccelerationStructuresCmd*>(_command)->Scratch().GetBuffer());
            break;
        default:
            break;
    }
}

void VulkanSerialGoldenTrace::EndLayer() {
    impl->command_builder.EndLayer();
}

void VulkanSerialGoldenTrace::SetCurrentCommand(uint32_t _original_ordinal) {
    impl->current_command       = _original_ordinal;
    impl->descriptor_invocation = 0;
}

StableSubmissionToken VulkanSerialGoldenTrace::ResolveNativeBuffer(uint64_t _native_handle) {
    if (const auto found = impl->native_buffers.find(_native_handle);
        found != impl->native_buffers.end()) {
        return found->second;
    }
    MarkUnresolved();
    ++impl->unresolved_native_buffers;
    return {StableObjectKind::Buffer, 0, 0, 0, 0, false};
}

StableSubmissionToken VulkanSerialGoldenTrace::ResolveNativeImage(uint64_t _native_handle) {
    if (const auto found = impl->native_images.find(_native_handle);
        found != impl->native_images.end()) {
        return found->second;
    }
    MarkUnresolved();
    ++impl->unresolved_native_images;
    return {StableObjectKind::Texture, 0, 0, 0, 0, false};
}

void VulkanSerialGoldenTrace::AddBarrier(const SerialBarrierItem& _item) {
    impl->barrier_builder.Add(_item);
}

void VulkanSerialGoldenTrace::RecordUnresolvedBufferBarrier(const SerialBarrierItem& _item) {
    if (!impl->has_unresolved_buffer_barrier) {
        impl->first_unresolved_buffer_barrier = _item;
        impl->has_unresolved_buffer_barrier   = true;
    }
}

void VulkanSerialGoldenTrace::RecordDescriptorBind(
    const PipelineHandle& _pipeline,
    const ArrayArguments& _args,
    const VulkanPipelineParamBinder& _binder
) {
    impl->RecordDescriptorBind(_pipeline, _args, _binder);
}

void VulkanSerialGoldenTrace::RecordDescriptorsForCommand(
    const Command* _command,
    const TCachedArgArray& _cached_args,
    uint64_t _relative_descriptor_begin,
    uint64_t _actual_descriptor_bytes
) {
    impl->RecordDescriptorsForCommand(
        _command,
        _cached_args,
        _relative_descriptor_begin,
        _actual_descriptor_bytes
    );
}

void VulkanSerialGoldenTrace::RecordQueryEvent(
    SerialQueryEvent _event,
    std::string_view _name,
    uint64_t         _pipeline_stage_mask
) {
    impl->query_builder.AddEvent(
        _event,
        _name,
        impl->query_event_ordinal++,
        _pipeline_stage_mask,
        0,
        0,
        true,
        impl->current_command
    );
}

void VulkanSerialGoldenTrace::MarkOpaque() {
    ++impl->opaque_count;
}

void VulkanSerialGoldenTrace::MarkUnresolved() {
    ++impl->unresolved_count;
}

uint32_t VulkanSerialGoldenTrace::OpaqueCount() const {
    return impl->opaque_count;
}

uint32_t VulkanSerialGoldenTrace::UnresolvedCount() const {
    return impl->unresolved_count;
}

uint64_t VulkanSerialGoldenTrace::OpaqueCommandMask() const {
    return impl->opaque_command_mask;
}

uint64_t VulkanSerialGoldenTrace::UnresolvedCommandMask() const {
    return impl->unresolved_command_mask;
}

uint32_t VulkanSerialGoldenTrace::UnresolvedNativeBufferCount() const {
    return impl->unresolved_native_buffers;
}

uint32_t VulkanSerialGoldenTrace::UnresolvedNativeImageCount() const {
    return impl->unresolved_native_images;
}

const SerialBarrierItem& VulkanSerialGoldenTrace::FirstUnresolvedBufferBarrier() const {
    return impl->first_unresolved_buffer_barrier;
}

bool VulkanSerialGoldenTrace::HasUnresolvedBufferBarrier() const {
    return impl->has_unresolved_buffer_barrier;
}

SerialGoldenSummary VulkanSerialGoldenTrace::Finish() {
    if (impl->finished) {
        return impl->finished_summary;
    }
    const SerialCommandLayerSection commands = impl->command_builder.Finish();
    const SerialGoldenSection barriers       = impl->barrier_builder.Finish();
    const SerialGoldenSection descriptors    = impl->descriptor_builder.Finish();
    const SerialGoldenSection queries        = impl->query_builder.Finish();
    impl->finished_summary = MakeSerialGoldenSummary(commands, barriers, descriptors, queries);
    impl->finished_summary.complete = impl->finished_summary.complete && impl->tokens.Complete() &&
                                      impl->opaque_count == 0 && impl->unresolved_count == 0;
    impl->finished_summary.combined_digest =
        CombineSerialGoldenDigests(impl->finished_summary);
    impl->finished = true;
    return impl->finished_summary;
}

} // namespace Moer::Render
