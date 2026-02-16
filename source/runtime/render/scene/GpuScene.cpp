#include "GpuScene.h"

#include "CpuScene.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "scene/LogicalComponents.h"

namespace Moer::Render {

/**
 * 这里存在着一点耦合问题：GpuScene需要修改CpuScene创建好的数据
 * - 原因就是，CpuScene需要bindless handle，但是这些handle只有在GpuScene中才能创建
 * - 目前的解决方案，就是GpuScene里再去填充数据，平凡地做
 * 
 * 另外一个解决方案，是将Texture/Mesh等持久化资源，专门独立到一个对象中管理
 * - 引入RenderResourceManager，专门负责管理这些持久化资源
 * - CpuScene和GpuScene都通过RenderResourceManager来获取资源handle
 * 
 * 现在也不好说哪种方案更好，所以现在这里记录一下这个问题
 * 
 * 因此，下面的Initialize和Update函数 的 CpuScene&，不应该添加const修饰符
 */

GpuScene::GpuScene(CpuScene& cpu_scene, BindlessArrayRef bindless_array) :
    m_logical_scene(cpu_scene.m_logical_scene),
    m_cpu_scene(cpu_scene),
    m_bindless_array(bindless_array) {

    m_res = Res{};

    auto& device = RenderDevice::Get();
    auto& bdls   = m_bindless_array;

    CommandList cmd_list{};

    // QUESTION: 这里多线程会有问题吗？RHI如何保证线程安全
    // => RHI不做线程安全，这个应用层做的
    // => 因此，该函数执行时，需要确保没有其他线程在操作RHI资源

    // Create & Upload & Bind Textures

    const Sampler default_sampler = Sampler(ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_REPEAT);

    {
        auto view = m_logical_scene.r().view<const ecs::CTexture, const ecs::CName>();

        m_res.texture_array.clear();
        m_res.texture_array.reserve(view.size_hint());

        m_map_texture_entity_to_bindless_handle.clear();

        // Device创建TextureRef & 录制Copy命令
        view.each([&](const auto entity, const ecs::CTexture& c_texture, const ecs::CName& c_name) {
            TextureWithHandle tex_with_hdl{};

            // 下文处的实现参考了重构前的 GpuScene.cpp: BuildTexturesInBatch()
            // TODO: 原函数中的Batch，意为分批上传数据，避免一个Command拷贝过多内容
            //       原函数没有实现这个功能，所以此处也暂不实现

            // 1. Create Texture
            tex_with_hdl.tex = device.CreateTexture(
                c_name.name,
                Extent2D{c_texture.width, c_texture.height},
                c_texture.format,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST,
                c_texture.mip_level_count,
                c_texture.array_layer_count
            );

            // 2. Copy Data
            uint64 offset = 0;
            for (uint i = 0; i < c_texture.mip_level_count * c_texture.array_layer_count; i++) {
                uint mip_level_byte_size = tex_with_hdl.tex->GetMipByteSize(i);
                cmd_list.CopyFrom(
                    std::span<byte>((byte*)c_texture.data.data() + offset, mip_level_byte_size),
                    tex_with_hdl.tex->GetView(i, 1)
                );
                offset += mip_level_byte_size;
            }

            // 3. Get Bindless Handle
            tex_with_hdl.hdl = bdls->AllocateTexture(
                tex_with_hdl.tex->GetView(0, tex_with_hdl.tex->GetNumMips()), default_sampler
            );
            m_map_texture_entity_to_bindless_handle[entity] = tex_with_hdl.hdl;

            // 4. Store
            m_res.texture_array.emplace_back(tex_with_hdl);
        });
    }

    // 填充GMaterial中空余的hdl字段

    {

        auto view = m_logical_scene.r().view<const ecs::CMaterial>();

        auto to_hdl = [&](const entt::entity entity) -> int64 {
            if (entity == entt::null) {
                return -1; // 不存在，应该使用factor
            }
            assert(
                m_map_texture_entity_to_bindless_handle.contains(entity) && "Texture entity not found in map"
            );
            return m_map_texture_entity_to_bindless_handle.at(entity);
        };

        view.each([&](const auto entity, const ecs::CMaterial& c_material) {
            uint mat_id = m_cpu_scene.m_map_material_entity_to_id.at(entity);

            GMaterial& g_material = m_cpu_scene.m_material_buf[mat_id];

            g_material.normal_map_hdl             = to_hdl(c_material.normal_map_entt);
            g_material.ao_map_hdl                 = to_hdl(c_material.ao_map_entt);
            g_material.albedo_map_hdl             = to_hdl(c_material.albedo_map_entt);
            g_material.emissive_map_hdl           = to_hdl(c_material.emissive_map_entt);
            g_material.metallic_roughness_map_hdl = to_hdl(c_material.metallic_roughness_map_entt);
        });
    }

    /**
     * MARK: 创建所有Buffers
     * 
     * 此处按照 Res 中顺序进行创建
     */

    m_res.light_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::LightBuffer",
        m_cpu_scene.m_light_buf.size() * sizeof(GLight),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    m_res.material_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::MaterialBuffer",
        m_cpu_scene.m_material_buf.size() * sizeof(GMaterial),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    // 这里不设置为byte，是为了GeometryPass中可以直接获取 命令的数量(cpu count)、DrawIndexedCmdData的stride
    m_res.draw_cmd_buf.buf = device.CreateBuffer<Render::DrawIndexedCmdData>(
        "GpuScene::DrawCmdBuffer",
        m_cpu_scene.m_draw_cmd_buf.size(),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
    );

    m_res.primitive_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::PrimitiveBuffer",
        m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    m_res.instance_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::InstanceBuffer",
        m_cpu_scene.m_instance_buf.size() * sizeof(GInstance),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    m_res.position_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::PositionMegaBuffer",
        m_cpu_scene.mega_buf().position.size() * sizeof(float3),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );

    m_res.packed_normal_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::NormalMegaBuffer",
        m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );

    m_res.packed_tangent_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::TangentMegaBuffer",
        m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );

    m_res.texcoord0_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::Texcoord0MegaBuffer",
        m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );

    m_res.index_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::IndexMegaBuffer",
        m_cpu_scene.mega_buf().index.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDEX_BUFFER
    );

    /**
     * MARK: 上传所有Buffers
     * 
     * 此处按照 Res 中顺序进行上传
     */

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_light_buf.data(), m_cpu_scene.m_light_buf.size() * sizeof(GLight)
        ),
        m_res.light_buf.buf->GetView(),
        "CopyFrom GpuScene::LightBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_material_buf.data(), m_cpu_scene.m_material_buf.size() * sizeof(GMaterial)
        ),
        m_res.material_buf.buf->GetView(),
        "CopyFrom GpuScene::MaterialBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_draw_cmd_buf.data(),
            m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        "CopyFrom GpuScene::DrawCmdBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_primitive_buf.data(), m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive)
        ),
        m_res.primitive_buf.buf->GetView(),
        "CopyFrom GpuScene::PrimitiveBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_instance_buf.data(), m_cpu_scene.m_instance_buf.size() * sizeof(GInstance)
        ),
        m_res.instance_buf.buf->GetView(),
        "CopyFrom GpuScene::InstanceBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().position.data(),
            m_cpu_scene.mega_buf().position.size() * sizeof(float3)
        ),
        m_res.position_buf.buf->GetView(),
        "CopyFrom GpuScene::PositionMegaBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().packed_normal.data(),
            m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32)
        ),
        m_res.packed_normal_buf.buf->GetView(),
        "CopyFrom GpuScene::NormalMegaBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().packed_tangent.data(),
            m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32)
        ),
        m_res.packed_tangent_buf.buf->GetView(),
        "CopyFrom GpuScene::TangentMegaBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().texcoord0.data(),
            m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2)
        ),
        m_res.texcoord0_buf.buf->GetView(),
        "CopyFrom GpuScene::Texcoord0MegaBuffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().index.data(), m_cpu_scene.mega_buf().index.size() * sizeof(uint32)
        ),
        m_res.index_buf.buf->GetView(),
        "CopyFrom GpuScene::IndexMegaBuffer"
    );

    /**
     * MARK: Bindless所有buffer
     * 
     * 绑定所有buffer到Bindless Array
     * 
     * 此处，我们默认所有buffer都被绑定到Bindless中。如果需要手动绑定，也不影响。
     * 理论上，一个资源同时被bindless绑定和正常绑定，是不会有 任何性能和资源浪费 或者 错误。
     */

    Array<BufferWithHandle*> buffers = {
        &m_res.light_buf,
        &m_res.material_buf,
        &m_res.draw_cmd_buf,
        &m_res.primitive_buf,
        &m_res.instance_buf,
        &m_res.position_buf,
        &m_res.packed_normal_buf,
        &m_res.packed_tangent_buf,
        &m_res.texcoord0_buf,
        &m_res.index_buf,
    };

    for (auto& buf_with_hdl_ptr : buffers) {
        BufferWithHandle& buf_with_hdl = *buf_with_hdl_ptr;

        buf_with_hdl.hdl = bdls->AllocateBuffer(buf_with_hdl.buf->GetView());
    }

    // NOTE: UpdateBindlessArray 需要在 Graphics/Compute Queue 中执行，不能在 Copy Queue 中执行
    // 这里只分配了 handle，实际的 bindless array 更新应该在后续的 Graphics Queue 命令中完成
    // cmd_list.UpdateBindlessArray(bdls);

    /**
     * MARK: Upload & Execute
     * 
     * 这里这段代码我不理解，我对GraphicsAPI底层的各种queue和barrier不够熟悉
     * 这部分代码是从旧的SceneCache.cpp中抄过来的
     */

    // 注意，此处不能在这两个数组中添加空资源，否则会触发RHI崩溃
    Array<ExportTexture> export_tex;
    Array<ExportBuffer>  export_buf;
    Array<ImportTexture> import_tex;
    Array<ImportBuffer>  import_buf;

    export_tex.reserve(m_res.texture_array.size());
    export_buf.reserve(buffers.size());
    import_tex.reserve(m_res.texture_array.size());
    import_buf.reserve(buffers.size());

    for (const auto& tex_with_hdl : m_res.texture_array) {

        auto mip_level_count = tex_with_hdl.tex->GetNumMips();

        // FIXME: 这里的GetView是否要指定mip范围？
        export_tex.emplace_back(ExportTexture{tex_with_hdl.tex->GetView(), ETextureState::SAMPLE});
        import_tex.emplace_back(
            ImportTexture{tex_with_hdl.tex->GetView(0, mip_level_count), ETextureState::SAMPLE}
        );
    }

    for (const auto& buf_with_hdl_ptr : buffers) {
        const BufferWithHandle& buf_with_hdl = *buf_with_hdl_ptr;

        export_buf.emplace_back(ExportBuffer{buf_with_hdl.buf->GetView(), EBufferState::UNORDERED_ACCESS});
        import_buf.emplace_back(ImportBuffer{buf_with_hdl.buf->GetView(), EBufferState::UNORDERED_ACCESS});
        // FIXME：源代码import_buf这里的state是UNDEFINED。我觉得可能是bug，所以这里改为了UNORDERED_ACCESS
    }

    // 从此处开始正式执行命令

    auto& copy_queue = device.GetCopyQueue();
    auto& gfx_queue  = device.GetCommandQueue(EQueueType::Graphics);

    {
        // 1. 拷贝Buffer等内容

        auto evt = copy_queue.Execute(cmd_list.Submit());
        copy_queue.Sync(evt.timeline);
    }

    {
        // 2. 导出资源到Graphics Queue

        cmd_list.ExportResourcesToQueue(EQueueType::Graphics, std::move(export_tex), std::move(export_buf));

        auto fence = copy_queue.GetFenceHandle();
        auto evt   = copy_queue.Execute(cmd_list.Submit().Wait(fence, fence->GetValue()));
        copy_queue.Sync(evt.timeline);
    }

    {

        cmd_list.ImportResourcesFromQueue(EQueueType::Copy, std::move(import_tex), std::move(import_buf));

        gfx_queue.Execute(cmd_list.Submit());
        gfx_queue.Sync();
    }

    InitRaytracingScene(cmd_list);

    gfx_queue.Execute(cmd_list.Submit()); // 提交RaytracingScene的新操作
    gfx_queue.Sync();
}

void GpuScene::Update(const ecs::LogicalScene& m_logical_scene, CpuScene& m_cpu_scene) {
    CommandList cmd_list{};
    auto&       device    = RenderDevice::Get();
    auto&       gfx_queue = device.GetCommandQueue(EQueueType::Graphics);

    // TODO: others

    UpdateRaytracingScene(cmd_list);

    gfx_queue.Execute(cmd_list.Submit()); // 提交RaytracingScene的新操作
    gfx_queue.Sync();
}

GpuScene::~GpuScene() noexcept {
    // TODO: 释放所有资源

    // 1. Buffers, Textures

    // 2. Bindless Resources
}

void GpuScene::InitRaytracingScene(CommandList& cmd_list) {
    auto& device = RenderDevice::Get();
    auto& r      = m_logical_scene.r();

    // 获取共享的 vertex 和 index buffers
    BufferRef position_buf_ref = m_res.position_buf.buf;
    BufferRef index_buf_ref    = m_res.index_buf.buf;

    Array<AccelerationStructureBuildParam> build_params;

    // init rt scene and geometries
    m_res.rt_scene  = device.CreateRaytracingScene();
    m_rt_geometries = Array<RaytracingGeometryRef>();

    // 遍历所有有 CRenderable 的 entity
    auto renderable_view = r.view<const ecs::CRenderable, const ecs::CTransform>();
    // 使用 size_hint() 估算大小（EnTT view 没有 size() 方法）
    m_rt_geometries.reserve(renderable_view.size_hint());
    build_params.reserve(renderable_view.size_hint());

    renderable_view.each(
        [&](const auto entity, const ecs::CRenderable& c_renderable, const ecs::CTransform& c_transform) {
            // 获取 CMesh
            if (!r.valid(c_renderable.mesh_entt) || !r.all_of<ecs::CMesh>(c_renderable.mesh_entt)) {
                return; // Skip invalid mesh
            }

            const ecs::CMesh& c_mesh = r.get<ecs::CMesh>(c_renderable.mesh_entt);

            // 为每个 CRenderable 创建一个 BLAS（包含对应CRenderable的所有 primitive，并且会重复创建）
            // TODO: 去重
            RaytracingGeometryInfo rt_geo_info{};
            rt_geo_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
            rt_geo_info.vertex_format = PF_R32G32B32_SFLOAT;
            rt_geo_info.index_type    = IET_UINT32;

            // 遍历该 Mesh 的所有 Primitive
            for (const entt::entity primitive_entt : c_mesh.primitive_entts) {
                if (!r.valid(primitive_entt) || !r.all_of<ecs::CPrimitive>(primitive_entt)) {
                    continue; // Skip invalid primitive
                }

                const ecs::CPrimitive& c_primitive = r.get<ecs::CPrimitive>(primitive_entt);

                // 检查必要的 buffer view 是否有效
                if (!c_primitive.position.is_valid || !c_primitive.index.is_valid) {
                    continue; // Skip primitive without valid position/index
                }

                // 从 CPrimitive 获取顶点和索引信息
                uint vtx_offset = c_primitive.position.start_idx; // element offset
                uint vtx_count  = c_primitive.vertex_count;
                uint idx_offset = c_primitive.index.start_idx; // element offset (in indices)
                uint idx_count  = c_primitive.index_count;

                // 使用 GpuScene 的共享 buffers（RaytracingSegment 需要 BufferRef，不是 BufferView）
                rt_geo_info.segments.emplace_back(
                    0,                // vertex_offset
                    0,                // index_offset
                    vtx_offset,       // first_vertex
                    vtx_count,        // vertex_count
                    sizeof(float3),   // vertex_stride
                    idx_offset / 3,   // first_primitive (indices are uint32, 3 per triangle)
                    idx_count / 3,    // primitive_count
                    position_buf_ref, // vertex_buffer (BufferRef)
                    index_buf_ref,    // index_buffer (BufferRef)
                    RTGT_TRIANGLES,   // type
                    ERayTracingGeometryFlags::GEOMETRY_OPAQUE, // flags
                    false,                                     // b_force_opaque
                    false,                                     // b_cull_back_face
                    false                                      // b_flip_face
                );
            }

            // 如果没有有效的 segments，跳过这个 entity
            if (rt_geo_info.segments.empty()) {
                return;
            }

            // 创建 BLAS
            RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
            m_rt_geometries.push_back(blas);

            // 添加 instance
            auto& instance = m_res.rt_scene->AddInstance();
            instance.geom  = blas;
            // 将 float4x4 转换为 Matrix3x4f
            instance.transform = Matrix3x4f(
                c_transform.d_world_transform.r0,
                c_transform.d_world_transform.r1,
                c_transform.d_world_transform.r2
            );

            instance.flag.need_create = true;
            instance.custom_index     = instance.instance_id;
            instance.visible_mask     = RTVM_ALL;
            m_res.rt_scene->MarkModified(instance.instance_id);

            // 建立 entity -> instance_idx 映射
            m_map_entity_to_instance_idx[entity] = instance.array_idx;

            build_params.push_back({blas, ERaytracingBuildMode::BUILD});
        }
    );

    cmd_list.BuildAccelerationStructures(std::move(build_params));
    cmd_list.UpdateRaytracingScene(m_res.rt_scene);

    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Execute(cmd_list.Submit());
    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
}

void GpuScene::UpdateRaytracingScene(CommandList& cmd_list) {
    auto& r = m_logical_scene.r();

    // 遍历所有有 CRenderable 的 entity，更新对应的 instance transform
    auto renderable_view = r.view<const ecs::CRenderable, const ecs::CTransform>();

    renderable_view.each(
        [&](const auto entity, const ecs::CRenderable& c_renderable, const ecs::CTransform& c_transform) {
            // 通过映射获取 instance_idx
            auto it = m_map_entity_to_instance_idx.find(entity);
            if (it == m_map_entity_to_instance_idx.end()) {
                LOG_WARNING(
                    "New entity has been added to the scene, but no instance mapping found. TODO: Add "
                    "instance mapping for this entity!"
                );
                return; // Skip entity without valid instance mapping
            }
            uint instance_idx = it->second;

            // 更新 transform
            auto& instance     = m_res.rt_scene->GetInstance(instance_idx);
            instance.transform = Matrix3x4f(
                c_transform.d_world_transform.r0,
                c_transform.d_world_transform.r1,
                c_transform.d_world_transform.r2
            );
            m_res.rt_scene->MarkModified(instance.instance_id);
        }
    );

    cmd_list.UpdateRaytracingScene(m_res.rt_scene);
}

} // namespace Moer::Render