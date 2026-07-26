#include "RaytracingFrameSetupPass.h"

#include "rhi/RHIImpl.h"

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
    bool               scene_inputs_updated,
    bool               external_tlas_built
) {
    auto payload = MakeShared<RecordPayload>();
    payload->bindless_array       = std::move(bindless_array);
    payload->scene                = std::move(scene);
    payload->target_revision      = target_revision;
    payload->build_tlas           = build_tlas;
    payload->scene_inputs_updated = scene_inputs_updated;
    payload->external_tlas_built  = external_tlas_built;

    if (!payload->bindless_array || !payload->scene) {
        return PreparedCommand{.record = std::move(payload)};
    }

    const RaytracingBindlessResources& scene_resources = rt_ctx.GetBindlessResources();
    for (const BufferRef& buffer : {
             scene_resources.light_buf,
             scene_resources.material_buf,
             scene_resources.primitive_buf,
             scene_resources.instance_buf,
             scene_resources.position_buf,
             scene_resources.packed_normal_buf,
             scene_resources.packed_tangent_buf,
             scene_resources.texcoord0_buf,
             scene_resources.index_buf,
             scene_resources.rt_instance_buf,
             scene_resources.rt_primitive_table_buf,
         }) {
        AppendUnique(payload->scene_buffers, buffer);
    }
    for (const TextureRef& texture : scene_resources.material_textures) {
        AppendUnique(payload->scene_textures, texture);
    }

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
    RenderGraph&           graph,
    const PreparedCommand& command
) const {
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
    if (!payload->scene_inputs_updated) {
        for (const BufferRef& buffer : payload->scene_buffers) {
            if (!accepted_scene_buffers.contains(buffer.Get())) {
                return false;
            }
        }
    }
    if (!payload->build_tlas && !payload->external_tlas_built &&
        !accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get())) {
        return false;
    }

    const auto bindless =
        graph.ImportToken("RT.FrameSetup.bindless", payload->bindless_array.Get());
    const auto current_tlas =
        ImportBuffer(graph, "RT.FrameSetup.current_tlas", payload->destination_tlas_buffer);

    Array<RenderGraph::BufferHandle> scene_buffers;
    scene_buffers.reserve(payload->scene_buffers.size());
    for (size_t index = 0; index < payload->scene_buffers.size(); ++index) {
        const BufferRef& buffer = payload->scene_buffers[index];
        const auto handle = ImportBuffer(
            graph,
            std::format("RT.FrameSetup.scene_buffer.{}", index),
            buffer
        );
        graph.SetInitialState(
            handle,
            payload->scene_inputs_updated ?
                RenderGraph::BufferState::UnorderedAccess :
                RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            payload->scene_inputs_updated ?
                RenderGraph::AccessMode::ReadWrite :
                RenderGraph::AccessMode::Read
        );
        scene_buffers.emplace_back(handle);
    }

    Array<RenderGraph::TextureHandle> scene_textures;
    scene_textures.reserve(payload->scene_textures.size());
    for (size_t index = 0; index < payload->scene_textures.size(); ++index) {
        const TextureRef& texture = payload->scene_textures[index];
        const auto handle = ImportTexture(
            graph,
            std::format("RT.FrameSetup.scene_texture.{}", index),
            texture
        );
        graph.SetInitialState(
            handle,
            PreferredReadState(texture),
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        scene_textures.emplace_back(handle);
    }

    const auto update_bindless = graph.AddRecordPass(
        "RT.FrameSetup.UpdateBindless",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Write(bindless)
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

    const auto normalize_scene = graph.AddRecordPass(
        "RT.FrameSetup.NormalizeSceneBindings",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .DependsOn(update_bindless)
                .Read(bindless)
                .SideEffect();
            for (const auto buffer : scene_buffers) {
                builder.Read(buffer, RenderGraph::BufferState::ShaderResource);
            }
            for (size_t index = 0; index < scene_textures.size(); ++index) {
                builder.Read(
                    scene_textures[index],
                    PreferredReadState(payload->scene_textures[index])
                );
            }
        },
        [](CommandList&) {},
        RenderGraph::PassExecutionClass::SerialRecord
    );

    graph.Export(bindless);
    for (const auto buffer : scene_buffers) {
        graph.Export(
            buffer,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    for (size_t index = 0; index < scene_textures.size(); ++index) {
        graph.Export(
            scene_textures[index],
            PreferredReadState(payload->scene_textures[index]),
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    if (!payload->build_tlas) {
        graph.SetInitialState(
            current_tlas,
            payload->external_tlas_built ?
                RenderGraph::BufferState::AccelerationStructureWrite :
                RenderGraph::BufferState::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            payload->external_tlas_built ?
                RenderGraph::AccessMode::Write :
                RenderGraph::AccessMode::Read
        );
        graph.AddRecordPass(
            "RT.FrameSetup.NormalizeTLAS",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Compute
                       )
                    .DependsOn(normalize_scene)
                    .Read(
                        current_tlas,
                        RenderGraph::BufferState::AccelerationStructureRead
                    )
                    .SideEffect();
            },
            [](CommandList&) {},
            RenderGraph::PassExecutionClass::SerialRecord
        );
        graph.Export(
            current_tlas,
            RenderGraph::BufferState::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        return true;
    }

    graph.SetInitialState(
        current_tlas,
        payload->external_tlas_built ?
            RenderGraph::BufferState::AccelerationStructureWrite :
            (accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get()) ?
                 RenderGraph::BufferState::AccelerationStructureRead :
                 RenderGraph::BufferState::Undefined),
        payload->external_tlas_built ||
                accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get()) ?
            RenderGraph::QueueRole::Graphics :
            RenderGraph::QueueRole::None,
        payload->external_tlas_built ?
            RenderGraph::AccessMode::Write :
            (accepted_tlas_buffers.contains(payload->destination_tlas_buffer.Get()) ?
                 RenderGraph::AccessMode::Read :
                 RenderGraph::AccessMode::None)
    );
    const auto instance_buffer =
        ImportBuffer(graph, "RT.FrameSetup.tlas_instances", payload->instance_buffer);
    const auto scratch_buffer =
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

    Array<RenderGraph::BufferHandle> geometry_buffers;
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

    graph.AddRecordPass(
        "RT.FrameSetup.BuildTLAS",
        [=](RenderGraph::PassBuilder& builder) {
            auto& setup_builder =
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::PipelineType::Compute
                       )
                .DependsOn(normalize_scene)
                .Read(bindless);
            if (payload->full_instance_upload) {
                // Prepare marks every live instance dirty before materializing
                // the command, so this transaction fully overwrites the
                // instance payload before the native TLAS build consumes it.
                // Model the pass boundary as Write; the command's internal
                // compute->AS barrier and the graph export establish the
                // build-input read state seen by later transactions.
                setup_builder.Write(
                    instance_buffer,
                    RenderGraph::BufferState::UnorderedAccess
                );
            } else {
                // Future backends may legally materialize a partial update.
                // That path is graph-safe only after the same physical buffer
                // has an accepted predecessor whose untouched instances remain
                // valid.
                setup_builder.ReadWrite(
                    instance_buffer,
                    RenderGraph::BufferState::UnorderedAccess
                );
            }
            setup_builder
                .Write(scratch_buffer, RenderGraph::BufferState::AccelerationStructureWrite)
                .Write(current_tlas, RenderGraph::BufferState::AccelerationStructureWrite);
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
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    graph.Export(
        current_tlas,
        RenderGraph::BufferState::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
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
    return true;
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

RaytracingFrameSetupPass::AcceptedSnapshot RaytracingFrameSetupPass::CommitAccepted(
    const PreparedCommand& command
) noexcept {
    const SharedPtr<RecordPayload> payload = command.record;
    if (!payload) {
        return {};
    }

    for (const BufferRef& buffer : payload->scene_buffers) {
        accepted_scene_buffers.emplace(buffer.Get());
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

    return AcceptedSnapshot{
        .bindless_array       = payload->bindless_array,
        .scene                = payload->scene,
        .current_tlas         = payload->destination_tlas,
        .previous_tlas        = payload->previous_tlas,
        .current_tlas_buffer  = payload->destination_tlas_buffer,
        .previous_tlas_buffer = payload->previous_tlas_buffer,
        .tlas_revision        = payload->target_revision,
        .tlas_built           = payload->build_tlas
    };
}

void RaytracingFrameSetupPass::ResetAcceptedResources() noexcept {
    accepted_tlas_buffers.clear();
    accepted_instance_buffers.clear();
    accepted_scratch_buffers.clear();
    accepted_scene_buffers.clear();
}

} // namespace Moer::Render::Raytracing
