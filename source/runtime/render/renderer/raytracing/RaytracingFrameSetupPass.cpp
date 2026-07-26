#include "RaytracingFrameSetupPass.h"

#include "rhi/RHIImpl.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <stdexcept>

namespace Moer::Render::Raytracing {

namespace {

RenderGraph::TextureState PreferredReadState(const TextureRef& texture) {
    const auto usage = texture->GetUsage();
    const bool supports_uav =
        (usage & ETextureUsageFlags::UNORDERED_ACCESS) == ETextureUsageFlags::UNORDERED_ACCESS;
    return supports_uav ? RenderGraph::TextureState::ShaderResource :
                          RenderGraph::TextureState::Sampled;
}

template<typename RefType>
void AppendUnique(Array<RefType>& refs, RefType ref) {
    if (!ref) {
        return;
    }
    for (const RefType& existing : refs) {
        if (existing.Get() == ref.Get()) {
            return;
        }
    }
    refs.emplace_back(std::move(ref));
}

RenderGraph::BufferHandle ImportBuffer(
    RenderGraph&     graph,
    std::string_view name,
    const BufferRef& buffer
) {
    return graph.ImportBuffer(
        name,
        buffer,
        RenderGraph::BufferDesc{.byte_size = buffer->GetByteSize()}
    );
}

RenderGraph::TextureHandle ImportTexture(
    RenderGraph&      graph,
    std::string_view  name,
    const TextureRef& texture
) {
    RenderGraph::TextureAspect aspects = RenderGraph::TextureAspect::None;
    const auto                 rhi_aspects = texture->GetAspectFlags();
    if (uint32_t(rhi_aspects & ETextureAspectFlags::COLOR) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Color;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::DEPTH_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Depth;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::STENCIL_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Stencil;
    }
    return graph.ImportTexture(
        name,
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = texture->GetNumMips(),
            .layer_count = texture->GetNumArray(),
            .aspects     = aspects
        }
    );
}

} // namespace

RaytracingFrameSetupPass::PreparedCommand RaytracingFrameSetupPass::Prepare(
    const RTContext&   rt_ctx,
    RaytracingSceneRef scene,
    BindlessArrayRef   bindless_array,
    uint64             target_revision,
    bool               build_tlas,
    bool               scene_resources_refreshed,
    bool               external_tlas_built
) {
    auto payload = MakeShared<RecordPayload>();
    payload->bindless_array       = std::move(bindless_array);
    payload->scene                = std::move(scene);
    payload->target_revision      = target_revision;
    payload->build_tlas           = build_tlas;
    payload->scene_resources_refreshed = scene_resources_refreshed;
    payload->external_tlas_built  = external_tlas_built;

    if (!payload->bindless_array || !payload->scene) {
        return PreparedCommand{.record = std::move(payload)};
    }

    payload->scene_resources = rt_ctx.GetBindlessResources();

    if (build_tlas) {
        for (uint index = 0; index < payload->scene->GetInstanceCount(); ++index) {
            RaytracingInstance& instance = payload->scene->GetInstance(index);
            payload->scene->MarkModified(instance.instance_id);
        }

        payload->build_tlas_command = payload->scene->UpdateScene();
        if (!payload->build_tlas_command ||
            payload->build_tlas_command->Type() != Command::EType::BuildTLAS) {
            payload->build_tlas = false;
            // A backend may legitimately report a no-op update while keeping
            // an already-built TLAS alive. Preserve that accepted destination
            // instead of turning the no-op into a renderer-wide fallback.
            payload->destination_tlas = payload->scene->GetTlas();
            payload->destination_tlas_buffer =
                payload->destination_tlas ?
                    BufferRef(payload->destination_tlas->GetUnderlyingBuffer()) :
                    BufferRef{};
        } else {
            const auto* update =
                static_cast<const UpdateRaytracingSceneCmd*>(payload->build_tlas_command.get());
            payload->full_instance_upload =
                update->InstancesToUpdate().size() ==
                payload->scene->GetInstanceCount();
            assert(
                payload->full_instance_upload &&
                "Renderer-owned TLAS build must upload every live instance"
            );
            payload->destination_tlas         = update->Tlas();
            payload->instance_buffer         = update->InstanceBuffer();
            payload->scratch_buffer          = update->ScratchBuffer();
            payload->geometries              = update->GeometryRefs();
            payload->destination_tlas_buffer =
                payload->destination_tlas ?
                    BufferRef(payload->destination_tlas->GetUnderlyingBuffer()) :
                    BufferRef{};
            for (const RaytracingGeometryRef& geometry : payload->geometries) {
                if (geometry && geometry->GetUnderlyingBuffer()) {
                    AppendUnique(
                        payload->geometry_buffers,
                        BufferRef(geometry->GetUnderlyingBuffer())
                    );
                }
            }
        }
    } else {
        payload->destination_tlas = payload->scene->GetTlas();
        payload->destination_tlas_buffer =
            payload->destination_tlas ?
                BufferRef(payload->destination_tlas->GetUnderlyingBuffer()) :
                BufferRef{};
    }

    payload->previous_tlas = payload->scene->GetPrevTlas();
    payload->previous_tlas_buffer =
        payload->previous_tlas ?
            BufferRef(payload->previous_tlas->GetUnderlyingBuffer()) :
            BufferRef{};
    return PreparedCommand{.record = std::move(payload)};
}

bool RaytracingFrameSetupPass::AddPasses(
    RenderGraph&                graph,
    const PreparedCommand&      command,
    RTGraphFrameSetupResources& graph_resources
) const {
    graph_resources = {};
    const SharedPtr<RecordPayload> payload = command.record;
    if (!command || !payload->previous_tlas || !payload->previous_tlas_buffer) {
        return false;
    }
    if (payload->build_tlas &&
        (!payload->build_tlas_command || !payload->instance_buffer ||
         !payload->scratch_buffer || payload->geometry_buffers.empty())) {
        return false;
    }
    if (payload->build_tlas && !payload->full_instance_upload &&
        !accepted_instance_buffers.contains(payload->instance_buffer.Get())) {
        return false;
    }
    if (!payload->scene_resources_refreshed) {
        for (const BufferRef& buffer : {
                 payload->scene_resources.light_buf,
                 payload->scene_resources.material_buf,
                 payload->scene_resources.primitive_buf,
                 payload->scene_resources.instance_buf,
                 payload->scene_resources.position_buf,
                 payload->scene_resources.packed_normal_buf,
                 payload->scene_resources.packed_tangent_buf,
                 payload->scene_resources.texcoord0_buf,
                 payload->scene_resources.index_buf,
                 payload->scene_resources.rt_instance_buf,
                 payload->scene_resources.rt_primitive_table_buf,
             }) {
            if (buffer && !accepted_scene_buffers.contains(buffer.Get())) {
                return false;
            }
        }
    }
    if (!payload->build_tlas && !payload->external_tlas_built &&
        !accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get())) {
        return false;
    }
    if (payload->previous_tlas_buffer.Get() !=
            payload->destination_tlas_buffer.Get() &&
        !accepted_tlas_buffers.contains(payload->previous_tlas_buffer.Get())) {
        return false;
    }

    graph_resources.bindless =
        graph.ImportToken("RT.FrameSetup.bindless", payload->bindless_array.Get());
    graph_resources.ready =
        graph.CreateTransientToken("RT.FrameSetup.ready");
    graph_resources.current_tlas =
        ImportBuffer(graph, "RT.FrameSetup.current_tlas", payload->destination_tlas_buffer);
    graph_resources.previous_tlas = graph_resources.current_tlas;
    if (payload->previous_tlas_buffer.Get() !=
        payload->destination_tlas_buffer.Get()) {
        graph_resources.previous_tlas = ImportBuffer(
            graph,
            "RT.FrameSetup.previous_tlas",
            payload->previous_tlas_buffer
        );
        graph.SetInitialState(
            graph_resources.previous_tlas,
            RenderGraph::BufferState::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    const bool current_tlas_accepted =
        accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get());
    graph.SetInitialState(
        graph_resources.current_tlas,
        payload->external_tlas_built ?
            RenderGraph::BufferState::AccelerationStructureWrite :
            (current_tlas_accepted ?
                 RenderGraph::BufferState::AccelerationStructureRead :
                 RenderGraph::BufferState::Undefined),
        payload->external_tlas_built || current_tlas_accepted ?
            RenderGraph::QueueRole::Graphics :
            RenderGraph::QueueRole::None,
        payload->external_tlas_built ?
            RenderGraph::AccessMode::Write :
            (current_tlas_accepted ?
                 RenderGraph::AccessMode::Read :
                 RenderGraph::AccessMode::None)
    );

    Array<RenderGraph::BufferHandle> unique_scene_buffers;
    const auto import_scene_buffer =
        [&](std::string_view name, const BufferRef& buffer) {
            if (!buffer) {
                return RenderGraph::BufferHandle{};
            }
            const auto handle = ImportBuffer(graph, name, buffer);
            const bool first_import = std::find(
                                          unique_scene_buffers.begin(),
                                          unique_scene_buffers.end(),
                                          handle
                                      ) == unique_scene_buffers.end();
            if (first_import) {
                graph.SetInitialState(
                    handle,
                    RenderGraph::BufferState::ShaderResource,
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::AccessMode::Read
                );
                unique_scene_buffers.emplace_back(handle);
            }
            return handle;
        };
    graph_resources.scene.light_resource = payload->scene_resources.light_buf;
    graph_resources.scene.light = import_scene_buffer(
        "RT.FrameSetup.scene.light",
        payload->scene_resources.light_buf
    );
    graph_resources.scene.material_resource = payload->scene_resources.material_buf;
    graph_resources.scene.material = import_scene_buffer(
        "RT.FrameSetup.scene.material",
        payload->scene_resources.material_buf
    );
    graph_resources.scene.primitive_resource = payload->scene_resources.primitive_buf;
    graph_resources.scene.primitive = import_scene_buffer(
        "RT.FrameSetup.scene.primitive",
        payload->scene_resources.primitive_buf
    );
    graph_resources.scene.instance_resource = payload->scene_resources.instance_buf;
    graph_resources.scene.instance = import_scene_buffer(
        "RT.FrameSetup.scene.instance",
        payload->scene_resources.instance_buf
    );
    graph_resources.scene.position_resource = payload->scene_resources.position_buf;
    graph_resources.scene.position = import_scene_buffer(
        "RT.FrameSetup.scene.position",
        payload->scene_resources.position_buf
    );
    graph_resources.scene.packed_normal_resource = payload->scene_resources.packed_normal_buf;
    graph_resources.scene.packed_normal = import_scene_buffer(
        "RT.FrameSetup.scene.packed_normal",
        payload->scene_resources.packed_normal_buf
    );
    graph_resources.scene.packed_tangent_resource = payload->scene_resources.packed_tangent_buf;
    graph_resources.scene.packed_tangent = import_scene_buffer(
        "RT.FrameSetup.scene.packed_tangent",
        payload->scene_resources.packed_tangent_buf
    );
    graph_resources.scene.texcoord0_resource = payload->scene_resources.texcoord0_buf;
    graph_resources.scene.texcoord0 = import_scene_buffer(
        "RT.FrameSetup.scene.texcoord0",
        payload->scene_resources.texcoord0_buf
    );
    graph_resources.scene.index_resource = payload->scene_resources.index_buf;
    graph_resources.scene.index = import_scene_buffer(
        "RT.FrameSetup.scene.index",
        payload->scene_resources.index_buf
    );
    graph_resources.scene.rt_instance_resource = payload->scene_resources.rt_instance_buf;
    graph_resources.scene.rt_instance = import_scene_buffer(
        "RT.FrameSetup.scene.rt_instance",
        payload->scene_resources.rt_instance_buf
    );
    graph_resources.scene.rt_primitive_table_resource =
        payload->scene_resources.rt_primitive_table_buf;
    graph_resources.scene.rt_primitive_table = import_scene_buffer(
        "RT.FrameSetup.scene.rt_primitive_table",
        payload->scene_resources.rt_primitive_table_buf
    );

    Array<RenderGraph::TextureHandle> unique_scene_textures;
    Array<RenderGraph::TextureState>  unique_scene_texture_states;
    graph_resources.scene.material_textures.reserve(
        payload->scene_resources.material_textures.size()
    );
    graph_resources.scene.material_texture_resources =
        payload->scene_resources.material_textures;
    for (size_t index = 0;
         index < payload->scene_resources.material_textures.size();
         ++index) {
        const TextureRef& texture =
            payload->scene_resources.material_textures[index];
        if (!texture) {
            graph_resources.scene.material_textures.emplace_back();
            continue;
        }
        const auto handle = ImportTexture(
            graph,
            std::format("RT.FrameSetup.scene.material_texture.{}", index),
            texture
        );
        graph_resources.scene.material_textures.emplace_back(handle);
        const bool first_import = std::find(
                                      unique_scene_textures.begin(),
                                      unique_scene_textures.end(),
                                      handle
                                  ) == unique_scene_textures.end();
        if (first_import) {
            graph.SetInitialState(
                handle,
                PreferredReadState(texture),
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );
            unique_scene_textures.emplace_back(handle);
            unique_scene_texture_states.emplace_back(
                PreferredReadState(texture)
            );
        }
    }

    graph_resources.update_bindless = graph.AddRecordPass(
        "RT.FrameSetup.UpdateBindless",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Write(graph_resources.bindless)
                .SideEffect();
        },
        [payload](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT Update Bindless",
                GpuMarkerPalette::Pass()
            );
            cmd_list.UpdateBindlessArray(payload->bindless_array);
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    graph_resources.normalize_scene = graph.AddRecordPass(
        "RT.FrameSetup.NormalizeSceneBindings",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .DependsOn(graph_resources.update_bindless)
                .Read(graph_resources.bindless)
                .SideEffect();
            for (const auto buffer : unique_scene_buffers) {
                builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
            }
            for (size_t index = 0;
                 index < unique_scene_textures.size();
                 ++index) {
                builder.Read(
                    unique_scene_textures[index],
                    unique_scene_texture_states[index]
                );
            }
        },
        [](CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );

    graph.Export(graph_resources.bindless);
    for (const auto buffer : unique_scene_buffers) {
        graph.Export(
            buffer,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    for (size_t index = 0;
         index < unique_scene_textures.size();
         ++index) {
        graph.Export(
            unique_scene_textures[index],
            unique_scene_texture_states[index],
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    RenderGraph::BufferHandle instance_buffer{};
    RenderGraph::BufferHandle scratch_buffer{};
    Array<RenderGraph::BufferHandle> geometry_buffers;
    if (payload->build_tlas) {
        instance_buffer =
            ImportBuffer(graph, "RT.FrameSetup.tlas_instances", payload->instance_buffer);
        scratch_buffer =
            ImportBuffer(graph, "RT.FrameSetup.tlas_scratch", payload->scratch_buffer);
        const bool instance_initialized =
            accepted_instance_buffers.contains(payload->instance_buffer.Get());
        graph.SetInitialState(
            instance_buffer,
            instance_initialized ?
                RenderGraph::BufferState::AccelerationStructureBuildInput :
                RenderGraph::BufferState::Undefined,
            instance_initialized ? RenderGraph::QueueRole::Graphics :
                                   RenderGraph::QueueRole::None,
            instance_initialized ? RenderGraph::AccessMode::Read :
                                   RenderGraph::AccessMode::None
        );
        const bool scratch_initialized =
            accepted_scratch_buffers.contains(payload->scratch_buffer.Get());
        graph.SetInitialState(
            scratch_buffer,
            scratch_initialized ?
                RenderGraph::BufferState::AccelerationStructureWrite :
                RenderGraph::BufferState::Undefined,
            scratch_initialized ? RenderGraph::QueueRole::Graphics :
                                  RenderGraph::QueueRole::None,
            scratch_initialized ? RenderGraph::AccessMode::Write :
                                  RenderGraph::AccessMode::None
        );

        geometry_buffers.reserve(payload->geometry_buffers.size());
        for (size_t index = 0; index < payload->geometry_buffers.size(); ++index) {
            const auto geometry = ImportBuffer(
                graph,
                std::format("RT.FrameSetup.geometry.{}", index),
                payload->geometry_buffers[index]
            );
            graph.SetInitialState(
                geometry,
                RenderGraph::BufferState::AccelerationStructureRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );
            geometry_buffers.emplace_back(geometry);
        }

        graph_resources.build_tlas = graph.AddRecordPass(
            "RT.FrameSetup.BuildTLAS",
            [=](RenderGraph::PassBuilder& builder) {
                auto& setup_builder =
                    builder.ExecuteOn(
                               RenderGraph::QueueRole::Graphics,
                               RenderGraph::PipelineType::Compute
                           )
                    .DependsOn(graph_resources.normalize_scene)
                    .Read(graph_resources.bindless);
                if (payload->full_instance_upload) {
                    setup_builder.Write(
                        instance_buffer,
                        RenderGraph::BufferState::UnorderedAccess
                    );
                } else {
                    setup_builder.ReadWrite(
                        instance_buffer,
                        RenderGraph::BufferState::UnorderedAccess
                    );
                }
                setup_builder
                    .Write(
                        scratch_buffer,
                        RenderGraph::BufferState::AccelerationStructureWrite
                    )
                    .Write(
                        graph_resources.current_tlas,
                        RenderGraph::BufferState::AccelerationStructureWrite
                    );
                for (const auto geometry : geometry_buffers) {
                    builder.Read(
                        geometry,
                        RenderGraph::BufferState::AccelerationStructureBuildInput
                    );
                }
            },
            [payload](CommandList& cmd_list) {
                bool expected = false;
                if (!payload->build_tlas_recorded.compare_exchange_strong(expected, true)) {
                    throw std::logic_error(
                        "RT FrameSetup BuildTLAS command was recorded more than once"
                    );
                }
                ScopedGpuMarker marker(
                    cmd_list,
                    "Pass: Scene Acceleration Structure",
                    GpuMarkerPalette::Pass()
                );
                cmd_list.PushScopeWithTimeScope("RTScene RendererTLAS");
                cmd_list.UpdateRaytracingScene(
                    std::move(payload->build_tlas_command)
                );
                cmd_list.PopScopeWithTimeScope();
            },
            RenderGraph::PassExecutionClass::SerialRecord
        );
    }

    graph_resources.finalize = graph.AddRecordPass(
        "RT.FrameSetup.Finalize",
        [=](RenderGraph::PassBuilder& builder) {
            auto& finalize_builder =
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Compute
                       )
                    .DependsOn(graph_resources.normalize_scene)
                    .Read(graph_resources.bindless)
                    .Read(
                        graph_resources.current_tlas,
                        RenderGraph::BufferState::AccelerationStructureRead
                    )
                    .Write(graph_resources.ready)
                    .SideEffect();
            if (graph_resources.build_tlas.IsValid()) {
                finalize_builder.DependsOn(graph_resources.build_tlas);
            }
            if (graph_resources.previous_tlas !=
                graph_resources.current_tlas) {
                finalize_builder.Read(
                    graph_resources.previous_tlas,
                    RenderGraph::BufferState::AccelerationStructureRead
                );
            }
        },
        [payload](CommandList&) {
            // Retain the immutable setup packet through the unified graph's
            // transaction gate, including no-build frames.
            (void)payload;
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    graph.Export(
        graph_resources.current_tlas,
        RenderGraph::BufferState::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    if (graph_resources.previous_tlas !=
        graph_resources.current_tlas) {
        graph.Export(
            graph_resources.previous_tlas,
            RenderGraph::BufferState::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    if (payload->build_tlas) {
        graph.Export(
            instance_buffer,
            RenderGraph::BufferState::AccelerationStructureBuildInput,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        graph.Export(
            scratch_buffer,
            RenderGraph::BufferState::AccelerationStructureWrite,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Write
        );
        for (const auto geometry : geometry_buffers) {
            graph.Export(
                geometry,
                RenderGraph::BufferState::AccelerationStructureRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );
        }
    }
    return graph_resources.IsValid();
}

bool RaytracingFrameSetupPass::ProcessLinear(
    CommandList&           cmd_list,
    const PreparedCommand& command
) const {
    const SharedPtr<RecordPayload> payload = command.record;
    if (!payload || !payload->bindless_array || !payload->destination_tlas ||
        !payload->destination_tlas_buffer) {
        return false;
    }
    if (payload->build_tlas &&
        (!payload->build_tlas_command || !payload->instance_buffer ||
         !payload->scratch_buffer || payload->geometry_buffers.empty())) {
        return false;
    }
    cmd_list.UpdateBindlessArray(payload->bindless_array);
    if (!payload->build_tlas || !payload->build_tlas_command) {
        return true;
    }

    bool expected = false;
    if (!payload->build_tlas_recorded.compare_exchange_strong(expected, true)) {
        throw std::logic_error("RT FrameSetup BuildTLAS command was recorded more than once");
    }
    ScopedGpuMarker marker(
        cmd_list,
        "Pass: Scene Acceleration Structure",
        GpuMarkerPalette::Pass()
    );
    cmd_list.PushScopeWithTimeScope("RTScene RendererTLAS");
    cmd_list.UpdateRaytracingScene(std::move(payload->build_tlas_command));
    cmd_list.PopScopeWithTimeScope();
    return true;
}

void RaytracingFrameSetupPass::CommitAccepted(
    const PreparedCommand& command
) noexcept {
    const SharedPtr<RecordPayload> payload = command.record;
    if (!payload) {
        return;
    }

    for (const BufferRef& buffer : {
             payload->scene_resources.light_buf,
             payload->scene_resources.material_buf,
             payload->scene_resources.primitive_buf,
             payload->scene_resources.instance_buf,
             payload->scene_resources.position_buf,
             payload->scene_resources.packed_normal_buf,
             payload->scene_resources.packed_tangent_buf,
             payload->scene_resources.texcoord0_buf,
             payload->scene_resources.index_buf,
             payload->scene_resources.rt_instance_buf,
             payload->scene_resources.rt_primitive_table_buf,
         }) {
        if (buffer) {
            accepted_scene_buffers.emplace(buffer.Get());
        }
    }
    if (payload->destination_tlas_buffer) {
        accepted_tlas_buffers.emplace(payload->destination_tlas_buffer.Get());
    }
    if (payload->previous_tlas_buffer) {
        accepted_tlas_buffers.emplace(payload->previous_tlas_buffer.Get());
    }
    if (payload->instance_buffer) {
        accepted_instance_buffers.emplace(payload->instance_buffer.Get());
    }
    if (payload->scratch_buffer) {
        accepted_scratch_buffers.emplace(payload->scratch_buffer.Get());
    }
}

void RaytracingFrameSetupPass::ResetAcceptedResources() noexcept {
    accepted_tlas_buffers.clear();
    accepted_instance_buffers.clear();
    accepted_scratch_buffers.clear();
    accepted_scene_buffers.clear();
}

} // namespace Moer::Render::Raytracing
