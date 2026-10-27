#include "RaytracingFrameSetupPass.h"

#include "rhi/RHIImpl.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <stdexcept>

namespace Moer::Render::Raytracing {

namespace {

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
    bool               build_tlas
) {
    auto payload = MakeShared<RecordPayload>();
    payload->bindless_array       = std::move(bindless_array);
    payload->scene                = std::move(scene);
    payload->build_tlas           = build_tlas;

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
    // There is no temporal TLAS on the first accepted frame. Match the shader
    // binding's current-TLAS fallback; temporal resampling stays disabled.
    if (!payload->previous_tlas) { payload->previous_tlas = payload->destination_tlas; }
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
            RenderGraph::BufferAccessType::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    // Every accepted scene/frame build exports ASRead. A new buffer has no
    // image layout and no prior accesses, so it uses the same boundary without
    // a separate allocation-address registry.
    graph.SetInitialState(
        graph_resources.current_tlas,
        RenderGraph::BufferAccessType::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
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
                    RenderGraph::BufferAccessType::ShaderRead,
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
    Array<RenderGraph::TextureAccessType> unique_scene_texture_states;
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
                RenderGraph::TextureAccessType::Sampled,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );
            unique_scene_textures.emplace_back(handle);
            unique_scene_texture_states.emplace_back(RenderGraph::TextureAccessType::Sampled);
        }
    }

    // Capturing pending bindless updates consumes shared CPU state in order.
    graph_resources.update_bindless = graph.AddRecordPass(
        "RT.FrameSetup.UpdateBindless",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
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
        RenderGraph::PassRecordPolicy::Serial
    );

    graph_resources.normalize_scene = graph.AddRecordPass(
        "RT.FrameSetup.NormalizeSceneBindings",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
                .DependsOn(graph_resources.update_bindless)
                .Read(graph_resources.bindless)
                .SideEffect();
            for (const auto buffer : unique_scene_buffers) {
                builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
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
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    graph.Export(graph_resources.bindless);
    for (const auto buffer : unique_scene_buffers) {
        graph.Export(
            buffer,
            RenderGraph::BufferAccessType::ShaderRead,
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

    Array<RenderGraph::BufferHandle> geometry_buffers;
    if (payload->build_tlas) {
        // Instance upload and scratch storage are private to the RHI TLAS
        // operation. RDG owns only its public BLAS inputs and TLAS output.
        geometry_buffers.reserve(payload->geometry_buffers.size());
        for (size_t index = 0; index < payload->geometry_buffers.size(); ++index) {
            const auto geometry = ImportBuffer(
                graph,
                std::format("RT.FrameSetup.geometry.{}", index),
                payload->geometry_buffers[index]
            );
            graph.SetInitialState(
                geometry,
                RenderGraph::BufferAccessType::AccelerationStructureRead,
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
                               RenderGraph::GpuWorkType::Compute
                           )
                    .DependsOn(graph_resources.normalize_scene)
                    .Read(graph_resources.bindless);
                setup_builder
                    .Write(
                        graph_resources.current_tlas,
                        RenderGraph::BufferAccessType::AccelerationStructureWrite
                    );
                for (const auto geometry : geometry_buffers) {
                    builder.Read(
                        geometry,
                        RenderGraph::BufferAccessType::AccelerationStructureBuildInput
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
            RenderGraph::PassRecordPolicy::ParallelAllowed
        );
    }

    graph_resources.finalize = graph.AddRecordPass(
        "RT.FrameSetup.Finalize",
        [=](RenderGraph::PassBuilder& builder) {
            auto& finalize_builder =
                builder.ExecuteOn(
                           RenderGraph::QueueRole::Graphics,
                           RenderGraph::GpuWorkType::Compute
                )
                    .DependsOn(graph_resources.normalize_scene)
                    .Read(graph_resources.bindless)
                    .Read(
                        graph_resources.current_tlas,
                        RenderGraph::BufferAccessType::AccelerationStructureRead
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
                    RenderGraph::BufferAccessType::AccelerationStructureRead
                );
            }
        },
        [payload](CommandList&) {
            // Retain the immutable setup packet through the unified graph's
            // transaction gate, including no-build frames.
            (void)payload;
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );

    graph.Export(
        graph_resources.current_tlas,
        RenderGraph::BufferAccessType::AccelerationStructureRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    if (graph_resources.previous_tlas !=
        graph_resources.current_tlas) {
        graph.Export(
            graph_resources.previous_tlas,
            RenderGraph::BufferAccessType::AccelerationStructureRead,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    if (payload->build_tlas) {
        for (const auto geometry : geometry_buffers) {
            graph.Export(
                geometry,
                RenderGraph::BufferAccessType::AccelerationStructureRead,
                RenderGraph::QueueRole::Graphics,
                RenderGraph::AccessMode::Read
            );
        }
    }
    return graph_resources.IsValid();
}

} // namespace Moer::Render::Raytracing
