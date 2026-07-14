#include "GpuScene.h"

#include "RenderThread.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"

#include <algorithm>
#include <cassert>
#include <type_traits>
#include <utility>

namespace Moer::Render {
namespace {

bool RecreateByteBufferIfNeeded(
    BindlessArrayRef  bindless_array,
    BufferWithHandle& target,
    std::string_view  name,
    uint64            required_byte_size,
    EBufferUsageFlags usage
) {
    if (required_byte_size == 0) {
        return false;
    }
    if (target.buf != nullptr && target.buf->GetByteSize() >= required_byte_size) {
        return false;
    }

    auto& device = RenderDevice::Get();
    if (target.hdl != 0) {
        bindless_array->UnbindBuffer(target.hdl);
        target.hdl = 0;
    }

    target.buf = device.CreateBuffer<byte>(name, static_cast<uint>(required_byte_size), usage);
    target.hdl = bindless_array->AllocateBuffer(target.buf->GetView());
    return true;
}

void UploadByteBuffer(
    CommandList&      cmd_list,
    BindlessArrayRef  bindless_array,
    BufferWithHandle& target,
    std::string_view  name,
    const void*       data,
    uint64            required_byte_size,
    EBufferUsageFlags usage
) {
    if (required_byte_size == 0) {
        return;
    }

    const bool need_bindless_update =
        RecreateByteBufferIfNeeded(bindless_array, target, name, required_byte_size, usage);
    cmd_list.CopyFrom(
        std::span<byte>(static_cast<byte*>(const_cast<void*>(data)), required_byte_size),
        target.buf->GetView(),
        name
    );

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(bindless_array);
    }
}

static constexpr std::string_view s_rt_scene_build_blas_scope_name  = "RTScene BuildBLAS";
static constexpr std::string_view s_rt_scene_build_tlas_scope_name  = "RTScene BuildTLAS";
static constexpr std::string_view s_rt_scene_update_tlas_scope_name = "RTScene UpdateTLAS";

} // namespace

GpuScene::GpuScene(BindlessArrayRef bindless_array) : m_bindless_array(std::move(bindless_array)) {}

void GpuScene::ApplyUpdate(GpuSceneUpdate&& update) {
    assert(IsCurrentlyGameThread() || IsCurrentlyRenderThread());
    assert(update.HasWork());

    m_pending_cmd_lists = PendingCommandList{};
    auto update_data = MakeShared<GpuSceneUpdate>(std::move(update));
    auto& pending_update = *update_data;

    if (pending_update.full_rebuild) {
        InitializeResources(pending_update);
    } else {
        auto& gfx_cmd_list = m_pending_cmd_lists.gfx_queue_cmd_list;
        if (pending_update.update_lights) {
            UpdateLightBuffer(gfx_cmd_list, pending_update.lights);
        }
        if (pending_update.update_materials) {
            UpdateMaterialBuffer(
                gfx_cmd_list, pending_update.materials, pending_update.material_texture_refs
            );
        }
        if (pending_update.update_meshes) {
            UpdateDrawCommandBuffer(gfx_cmd_list, pending_update.draw_commands);
            UpdatePrimitiveBuffer(gfx_cmd_list, pending_update.primitives);
            UpdateInstanceBuffer(gfx_cmd_list, pending_update.instances);
            UpdateClusterGroupBuffer(gfx_cmd_list, pending_update.cluster_groups);
            UpdatePositionMegaBuffer(gfx_cmd_list, pending_update.positions);
            UpdatePackedNormalMegaBuffer(gfx_cmd_list, pending_update.packed_normals);
            UpdatePackedTangentMegaBuffer(gfx_cmd_list, pending_update.packed_tangents);
            UpdateTexcoord0MegaBuffer(gfx_cmd_list, pending_update.texcoords0);
            UpdateIndexMegaBuffer(gfx_cmd_list, pending_update.indices);
        } else if (pending_update.raytracing_update != EGpuSceneRaytracingUpdate::None) {
            UpdateInstanceBuffer(gfx_cmd_list, pending_update.instances);
        }

        switch (pending_update.raytracing_update) {
            case EGpuSceneRaytracingUpdate::None:
                break;
            case EGpuSceneRaytracingUpdate::UpdateInstances:
                UpdateRaytracingScene(gfx_cmd_list, pending_update.rt_instances);
                break;
            case EGpuSceneRaytracingUpdate::RebuildTlas:
                RebuildRaytracingSceneTlas(gfx_cmd_list, pending_update.rt_instances);
                break;
            case EGpuSceneRaytracingUpdate::RebuildBlas:
                InitRaytracingScene(
                    gfx_cmd_list, pending_update.rt_meshes, pending_update.rt_instances
                );
                break;
        }
    }

    // CopyFrom(span) stores a non-owning pointer. Keep the snapshot alive through each relevant submit.
    if (!m_pending_cmd_lists.copy_queue_cmd_list.IsEmpty()) {
        m_pending_cmd_lists.copy_queue_cmd_list.AddCallback([update_data]() {});
    }
    if (!m_pending_cmd_lists.gfx_queue_cmd_list.IsEmpty()) {
        m_pending_cmd_lists.gfx_queue_cmd_list.AddCallback([update_data]() {});
    }
}

void GpuScene::ResolveMaterialTextureHandles(
    Array<GMaterial>&                         materials,
    const Array<GpuSceneMaterialTextureRefs>& texture_refs
) const {
    assert(materials.size() == texture_refs.size());

    auto resolve = [&](GpuSceneResourceKey key) -> int64 {
        if (key == k_invalid_gpu_scene_resource_key) {
            return -1;
        }
        const auto it = m_map_texture_key_to_bindless_handle.find(key);
        if (it == m_map_texture_key_to_bindless_handle.end()) {
            LOG_WARNING("GpuScene material references unknown texture key {}.", key);
            return -1;
        }
        return static_cast<int64>(it->second);
    };

    for (size_t index = 0; index < materials.size(); ++index) {
        auto&       material = materials[index];
        const auto& refs     = texture_refs[index];
        material.normal_map_hdl             = resolve(refs.normal);
        material.ao_map_hdl                 = resolve(refs.ao);
        material.albedo_map_hdl             = resolve(refs.albedo);
        material.emissive_map_hdl           = resolve(refs.emissive);
        material.metallic_roughness_map_hdl = resolve(refs.metallic_roughness);
    }
}

void GpuScene::InitializeResources(GpuSceneUpdate& update) {
    assert(update.full_rebuild);
    auto& device = RenderDevice::Get();
    auto& bdls   = m_bindless_array;

    const Sampler default_sampler = Sampler(SF_LINEAR, SAM_REPEAT);
    m_res                         = Res{};
    m_map_texture_key_to_bindless_handle.clear();
    m_res.texture_array.reserve(update.textures.size());

    for (const GpuSceneTextureData& texture : update.textures) {
        TextureWithHandle gpu_texture{};
        gpu_texture.tex = device.CreateTexture(
            texture.name,
            Extent2D{texture.width, texture.height},
            texture.format,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST,
            texture.mip_level_count,
            texture.array_layer_count
        );

        uint64 offset = 0;
        for (uint subresource = 0;
             subresource < texture.mip_level_count * texture.array_layer_count;
             ++subresource) {
            const uint mip_byte_size = gpu_texture.tex->GetMipByteSize(subresource);
            assert(offset + mip_byte_size <= texture.data.size());
            m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
                std::span<byte>(
                    reinterpret_cast<byte*>(const_cast<uint8*>(texture.data.data())) + offset,
                    mip_byte_size
                ),
                gpu_texture.tex->GetView(subresource, 1)
            );
            offset += mip_byte_size;
        }

        gpu_texture.hdl = bdls->AllocateTexture(
            gpu_texture.tex->GetView(0, gpu_texture.tex->GetNumMips()), default_sampler
        );
        m_map_texture_key_to_bindless_handle[texture.key] = gpu_texture.hdl;
        m_res.texture_array.push_back(std::move(gpu_texture));
    }

    ResolveMaterialTextureHandles(update.materials, update.material_texture_refs);
    m_draw_commands = update.draw_commands;

    auto create_byte_buffer = [&](BufferWithHandle& target,
                                  std::string_view  name,
                                  uint64            byte_size,
                                  EBufferUsageFlags usage) {
        if (byte_size == 0) {
            return;
        }
        target.buf = device.CreateBuffer<byte>(name, static_cast<uint>(byte_size), usage);
    };

    create_byte_buffer(
        m_res.light_buf,
        "GpuScene::LightBuffer",
        update.lights.size() * sizeof(GLight),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    create_byte_buffer(
        m_res.material_buf,
        "GpuScene::MaterialBuffer",
        update.materials.size() * sizeof(GMaterial),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    if (!update.draw_commands.empty()) {
        m_res.draw_cmd_buf.buf = device.CreateBuffer<DrawIndexedCmdData>(
            "GpuScene::DrawCmdBuffer",
            static_cast<uint>(update.draw_commands.size()),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
        );
    }
    create_byte_buffer(
        m_res.primitive_buf,
        "GpuScene::PrimitiveBuffer",
        update.primitives.size() * sizeof(GPrimitive),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    create_byte_buffer(
        m_res.instance_buf,
        "GpuScene::InstanceBuffer",
        update.instances.size() * sizeof(GInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    create_byte_buffer(
        m_res.cluster_group_buf,
        "GpuScene::ClusterGroupBuffer",
        update.cluster_groups.size() * sizeof(GClusterGroup),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    create_byte_buffer(
        m_res.position_buf,
        "GpuScene::PositionMegaBuffer",
        update.positions.size() * sizeof(float3),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
    create_byte_buffer(
        m_res.packed_normal_buf,
        "GpuScene::NormalMegaBuffer",
        update.packed_normals.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
    create_byte_buffer(
        m_res.packed_tangent_buf,
        "GpuScene::TangentMegaBuffer",
        update.packed_tangents.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
    create_byte_buffer(
        m_res.texcoord0_buf,
        "GpuScene::Texcoord0MegaBuffer",
        update.texcoords0.size() * sizeof(float2),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
    create_byte_buffer(
        m_res.index_buf,
        "GpuScene::IndexMegaBuffer",
        update.indices.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDEX_BUFFER
    );

    auto record_copy = [&](const auto& source, BufferWithHandle& target, std::string_view name) {
        using Element = typename std::decay_t<decltype(source)>::value_type;
        if (source.empty()) {
            return;
        }
        m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(const_cast<Element*>(source.data())),
                source.size() * sizeof(Element)
            ),
            target.buf->GetView(),
            name
        );
    };

    record_copy(update.lights, m_res.light_buf, "CopyFrom GpuScene::LightBuffer");
    record_copy(update.materials, m_res.material_buf, "CopyFrom GpuScene::MaterialBuffer");
    record_copy(update.draw_commands, m_res.draw_cmd_buf, "CopyFrom GpuScene::DrawCmdBuffer");
    record_copy(update.primitives, m_res.primitive_buf, "CopyFrom GpuScene::PrimitiveBuffer");
    record_copy(update.instances, m_res.instance_buf, "CopyFrom GpuScene::InstanceBuffer");
    record_copy(
        update.cluster_groups, m_res.cluster_group_buf, "CopyFrom GpuScene::ClusterGroupBuffer"
    );
    record_copy(update.positions, m_res.position_buf, "CopyFrom GpuScene::PositionMegaBuffer");
    record_copy(
        update.packed_normals, m_res.packed_normal_buf, "CopyFrom GpuScene::NormalMegaBuffer"
    );
    record_copy(
        update.packed_tangents, m_res.packed_tangent_buf, "CopyFrom GpuScene::TangentMegaBuffer"
    );
    record_copy(update.texcoords0, m_res.texcoord0_buf, "CopyFrom GpuScene::Texcoord0MegaBuffer");
    record_copy(update.indices, m_res.index_buf, "CopyFrom GpuScene::IndexMegaBuffer");

    Array<BufferWithHandle*> buffers = {
        &m_res.light_buf,
        &m_res.material_buf,
        &m_res.draw_cmd_buf,
        &m_res.primitive_buf,
        &m_res.instance_buf,
        &m_res.cluster_group_buf,
        &m_res.position_buf,
        &m_res.packed_normal_buf,
        &m_res.packed_tangent_buf,
        &m_res.texcoord0_buf,
        &m_res.index_buf,
    };
    buffers.erase(
        std::remove_if(buffers.begin(), buffers.end(), [](const BufferWithHandle* buffer) {
            return buffer->buf == nullptr;
        }),
        buffers.end()
    );

    for (BufferWithHandle* buffer : buffers) {
        buffer->hdl = bdls->AllocateBuffer(buffer->buf->GetView());
    }
    m_pending_cmd_lists.gfx_queue_cmd_list.UpdateBindlessArray(bdls);

    Array<ExportTexture> export_textures;
    Array<ImportTexture> import_textures;
    export_textures.reserve(m_res.texture_array.size());
    import_textures.reserve(m_res.texture_array.size());
    for (const TextureWithHandle& texture : m_res.texture_array) {
        export_textures.push_back({texture.tex->GetView(), ETextureState::SAMPLE});
        import_textures.push_back(
            {texture.tex->GetView(0, texture.tex->GetNumMips()), ETextureState::SAMPLE}
        );
    }

    Array<ExportBuffer> export_buffers;
    Array<ImportBuffer> import_buffers;
    export_buffers.reserve(buffers.size());
    import_buffers.reserve(buffers.size());
    for (const BufferWithHandle* buffer : buffers) {
        export_buffers.push_back({buffer->buf->GetView(), EBufferState::UNORDERED_ACCESS});
        import_buffers.push_back({buffer->buf->GetView(), EBufferState::UNORDERED_ACCESS});
    }

    m_pending_cmd_lists.copy_queue_cmd_list.ExportResourcesToQueue(
        EQueueType::Graphics, std::move(export_textures), std::move(export_buffers)
    );
    m_pending_cmd_lists.gfx_queue_cmd_list.ImportResourcesFromQueue(
        EQueueType::Copy, std::move(import_textures), std::move(import_buffers)
    );
    InitRaytracingScene(
        m_pending_cmd_lists.gfx_queue_cmd_list, update.rt_meshes, update.rt_instances
    );
}

void GpuScene::UpdateLightBuffer(CommandList& cmd_list, const Array<GLight>& lights) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.light_buf,
        "GpuScene::LightBuffer",
        lights.data(),
        lights.size() * sizeof(GLight),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdateMaterialBuffer(
    CommandList&                              cmd_list,
    Array<GMaterial>&                         materials,
    const Array<GpuSceneMaterialTextureRefs>& texture_refs
) {
    ResolveMaterialTextureHandles(materials, texture_refs);
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.material_buf,
        "GpuScene::MaterialBuffer",
        materials.data(),
        materials.size() * sizeof(GMaterial),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdateDrawCommandBuffer(
    CommandList& cmd_list, const Array<DrawIndexedCmdData>& draw_commands
) {
    m_draw_commands = draw_commands;
    if (draw_commands.empty()) {
        return;
    }

    bool need_bindless_update = false;
    if (m_res.draw_cmd_buf.buf == nullptr ||
        m_res.draw_cmd_buf.buf->GetNumElement() < draw_commands.size()) {
        if (m_res.draw_cmd_buf.hdl != 0) {
            m_bindless_array->UnbindBuffer(m_res.draw_cmd_buf.hdl);
        }
        m_res.draw_cmd_buf.buf = RenderDevice::Get().CreateBuffer<DrawIndexedCmdData>(
            "GpuScene::DrawCmdBuffer",
            static_cast<uint>(draw_commands.size()),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
        );
        m_res.draw_cmd_buf.hdl =
            m_bindless_array->AllocateBuffer(m_res.draw_cmd_buf.buf->GetView());
        need_bindless_update = true;
    }

    cmd_list.CopyFrom(
        std::span<byte>(
            reinterpret_cast<byte*>(const_cast<DrawIndexedCmdData*>(draw_commands.data())),
            draw_commands.size() * sizeof(DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        "CopyFrom GpuScene::DrawCmdBuffer"
    );
    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(m_bindless_array);
    }
}

void GpuScene::UpdateInstanceBuffer(CommandList& cmd_list, const Array<GInstance>& instances) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.instance_buf,
        "GpuScene::InstanceBuffer",
        instances.data(),
        instances.size() * sizeof(GInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdatePrimitiveBuffer(CommandList& cmd_list, const Array<GPrimitive>& primitives) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.primitive_buf,
        "GpuScene::PrimitiveBuffer",
        primitives.data(),
        primitives.size() * sizeof(GPrimitive),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdateClusterGroupBuffer(
    CommandList& cmd_list, const Array<GClusterGroup>& cluster_groups
) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.cluster_group_buf,
        "GpuScene::ClusterGroupBuffer",
        cluster_groups.data(),
        cluster_groups.size() * sizeof(GClusterGroup),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdatePositionMegaBuffer(CommandList& cmd_list, const Array<float3>& positions) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.position_buf,
        "GpuScene::PositionMegaBuffer",
        positions.data(),
        positions.size() * sizeof(float3),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdatePackedNormalMegaBuffer(
    CommandList& cmd_list, const Array<uint32>& packed_normals
) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.packed_normal_buf,
        "GpuScene::NormalMegaBuffer",
        packed_normals.data(),
        packed_normals.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdatePackedTangentMegaBuffer(
    CommandList& cmd_list, const Array<uint32>& packed_tangents
) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.packed_tangent_buf,
        "GpuScene::TangentMegaBuffer",
        packed_tangents.data(),
        packed_tangents.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdateTexcoord0MegaBuffer(CommandList& cmd_list, const Array<float2>& texcoords0) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.texcoord0_buf,
        "GpuScene::Texcoord0MegaBuffer",
        texcoords0.data(),
        texcoords0.size() * sizeof(float2),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdateIndexMegaBuffer(CommandList& cmd_list, const Array<uint32>& indices) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.index_buf,
        "GpuScene::IndexMegaBuffer",
        indices.data(),
        indices.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDEX_BUFFER
    );
}

GpuScene::~GpuScene() noexcept {
    assert(IsCurrentlyGameThread() || IsCurrentlyRenderThread());
    if (!m_bindless_array) {
        return;
    }

    for (const TextureWithHandle& texture : m_res.texture_array) {
        if (texture.hdl != 0) {
            m_bindless_array->UnbindTexture(texture.hdl);
        }
    }

    BufferWithHandle* buffers[] = {
        &m_res.light_buf,
        &m_res.material_buf,
        &m_res.draw_cmd_buf,
        &m_res.primitive_buf,
        &m_res.instance_buf,
        &m_res.cluster_group_buf,
        &m_res.position_buf,
        &m_res.packed_normal_buf,
        &m_res.packed_tangent_buf,
        &m_res.texcoord0_buf,
        &m_res.index_buf,
        &m_res.rt_instance_buf,
        &m_res.rt_primitive_table_buf,
    };
    for (const BufferWithHandle* buffer : buffers) {
        if (buffer->hdl != 0) {
            m_bindless_array->UnbindBuffer(buffer->hdl);
        }
    }
}

void GpuScene::InitRaytracingScene(
    CommandList&                         cmd_list,
    const Array<GpuSceneRtMeshData>&     meshes,
    const Array<GpuSceneRtInstanceData>& instances
) {
    auto& device = RenderDevice::Get();
    m_res.rt_scene = device.CreateRaytracingScene();
    m_mesh_key_to_blas.clear();
    m_rt_primitive_table_cache.clear();
    m_mesh_key_to_primitive_table_offset.clear();

    Array<AccelerationStructureBuildParam> build_params;
    for (const GpuSceneRtMeshData& mesh : meshes) {
        RaytracingGeometryInfo geometry_info{};
        geometry_info.build_flags =
            ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
        geometry_info.vertex_format = PF_R32G32B32_SFLOAT;
        geometry_info.index_type    = IET_UINT32;

        m_mesh_key_to_primitive_table_offset[mesh.key] =
            static_cast<uint32>(m_rt_primitive_table_cache.size());
        for (const GpuSceneRtGeometryData& geometry : mesh.geometries) {
            geometry_info.segments.emplace_back(
                0,
                0,
                geometry.vertex_offset,
                geometry.vertex_count,
                sizeof(float3),
                geometry.index_offset / 3,
                geometry.index_count / 3,
                m_res.position_buf.buf,
                m_res.index_buf.buf,
                RTGT_TRIANGLES,
                ERayTracingGeometryFlags::GEOMETRY_OPAQUE,
                false,
                false,
                false
            );
            m_rt_primitive_table_cache.push_back(geometry.primitive_id);
        }

        if (!geometry_info.segments.empty()) {
            RaytracingGeometryRef blas = device.CreateRaytracingGeometry(geometry_info);
            m_mesh_key_to_blas[mesh.key] = blas;
            build_params.push_back({blas, ERaytracingBuildMode::BUILD});
        }
    }

    m_rt_instance_cache.clear();
    for (const GpuSceneRtInstanceData& source : instances) {
        const auto blas_it = m_mesh_key_to_blas.find(source.mesh_key);
        const auto offset_it = m_mesh_key_to_primitive_table_offset.find(source.mesh_key);
        if (blas_it == m_mesh_key_to_blas.end() ||
            offset_it == m_mesh_key_to_primitive_table_offset.end()) {
            continue;
        }

        const uint instance_index = static_cast<uint>(m_rt_instance_cache.size());
        auto&      instance       = m_res.rt_scene->AddInstance();
        instance.geom             = blas_it->second;
        instance.transform        = source.world_transform.ToTransposedMatrix3x4f();
        instance.flag.need_create = true;
        instance.custom_index     = instance_index;
        instance.visible_mask     = RTVM_ALL;
        m_res.rt_scene->MarkModified(instance.instance_id);

        GRtInstance gpu_instance{};
        gpu_instance.world_transform        = source.world_transform;
        gpu_instance.primitive_table_offset = offset_it->second;
        gpu_instance.primitive_count        = source.primitive_count;
        gpu_instance.first_primitive_id     = source.first_primitive_id;
        m_rt_instance_cache.push_back(gpu_instance);
    }

    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.rt_instance_buf,
        "GpuScene::RtInstanceBuffer",
        m_rt_instance_cache.data(),
        m_rt_instance_cache.size() * sizeof(GRtInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.rt_primitive_table_buf,
        "GpuScene::RtPrimitiveTableBuffer",
        m_rt_primitive_table_cache.data(),
        m_rt_primitive_table_cache.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    if (!build_params.empty()) {
        cmd_list.PushScopeWithTimeScope(s_rt_scene_build_blas_scope_name);
        cmd_list.BuildAccelerationStructures(std::move(build_params));
        cmd_list.PopScopeWithTimeScope();
    }
    cmd_list.PushScopeWithTimeScope(s_rt_scene_build_tlas_scope_name);
    cmd_list.UpdateRaytracingScene(m_res.rt_scene);
    cmd_list.PopScopeWithTimeScope();

    LOG_DEBUG(
        "[RTSceneProfile] Build summary: mesh_blas_count={} tlas_instance_count={} primitive_table_size={}",
        m_mesh_key_to_blas.size(),
        m_res.rt_scene->GetInstanceCount(),
        m_rt_primitive_table_cache.size()
    );
}

void GpuScene::RebuildRaytracingSceneTlas(
    CommandList&                             cmd_list,
    const Array<GpuSceneRtInstanceData>& instances
) {
    if (m_mesh_key_to_blas.empty()) {
        LOG_WARNING("Cannot rebuild TLAS because RenderScene has no BLAS cache.");
        return;
    }

    m_res.rt_scene = RenderDevice::Get().CreateRaytracingScene();
    m_rt_instance_cache.clear();
    for (const GpuSceneRtInstanceData& source : instances) {
        const auto blas_it = m_mesh_key_to_blas.find(source.mesh_key);
        const auto offset_it = m_mesh_key_to_primitive_table_offset.find(source.mesh_key);
        if (blas_it == m_mesh_key_to_blas.end() ||
            offset_it == m_mesh_key_to_primitive_table_offset.end()) {
            continue;
        }

        const uint instance_index = static_cast<uint>(m_rt_instance_cache.size());
        auto&      instance       = m_res.rt_scene->AddInstance();
        instance.geom             = blas_it->second;
        instance.transform        = source.world_transform.ToTransposedMatrix3x4f();
        instance.flag.need_create = true;
        instance.custom_index     = instance_index;
        instance.visible_mask     = RTVM_ALL;
        m_res.rt_scene->MarkModified(instance.instance_id);

        GRtInstance gpu_instance{};
        gpu_instance.world_transform        = source.world_transform;
        gpu_instance.primitive_table_offset = offset_it->second;
        gpu_instance.primitive_count        = source.primitive_count;
        gpu_instance.first_primitive_id     = source.first_primitive_id;
        m_rt_instance_cache.push_back(gpu_instance);
    }

    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.rt_instance_buf,
        "GpuScene::RtInstanceBuffer",
        m_rt_instance_cache.data(),
        m_rt_instance_cache.size() * sizeof(GRtInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    cmd_list.PushScopeWithTimeScope(s_rt_scene_build_tlas_scope_name);
    cmd_list.UpdateRaytracingScene(m_res.rt_scene);
    cmd_list.PopScopeWithTimeScope();
}

void GpuScene::UpdateRaytracingScene(
    CommandList&                             cmd_list,
    const Array<GpuSceneRtInstanceData>& instances
) {
    if (!m_res.rt_scene) {
        LOG_WARNING("Cannot update TLAS because RenderScene has no ray tracing scene.");
        return;
    }

    uint valid_instance_count = 0;
    for (const GpuSceneRtInstanceData& source : instances) {
        if (m_mesh_key_to_blas.contains(source.mesh_key)) {
            ++valid_instance_count;
        }
    }
    if (valid_instance_count != m_res.rt_scene->GetInstanceCount()) {
        RebuildRaytracingSceneTlas(cmd_list, instances);
        return;
    }

    uint instance_index = 0;
    for (const GpuSceneRtInstanceData& source : instances) {
        if (!m_mesh_key_to_blas.contains(source.mesh_key)) {
            continue;
        }
        auto& instance     = m_res.rt_scene->GetInstance(instance_index);
        instance.transform = source.world_transform.ToTransposedMatrix3x4f();
        m_res.rt_scene->MarkModified(instance.instance_id);
        m_rt_instance_cache[instance_index].world_transform = source.world_transform;
        ++instance_index;
    }

    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.rt_instance_buf,
        "GpuScene::RtInstanceBuffer",
        m_rt_instance_cache.data(),
        m_rt_instance_cache.size() * sizeof(GRtInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    cmd_list.PushScopeWithTimeScope(s_rt_scene_update_tlas_scope_name);
    cmd_list.UpdateRaytracingScene(m_res.rt_scene);
    cmd_list.PopScopeWithTimeScope();
}

void GpuScene::RestoreDrawCommands(CommandList& cmd_list) {
    if (m_draw_commands.empty() || m_res.draw_cmd_buf.buf == nullptr) {
        return;
    }
    cmd_list.CopyFrom(
        std::span<byte>(
            reinterpret_cast<byte*>(m_draw_commands.data()),
            m_draw_commands.size() * sizeof(DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        "RestoreDrawCommands"
    );
}

} // namespace Moer::Render
