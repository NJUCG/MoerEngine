#include "GpuScene.h"

#include "CpuScene.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "scene/LogicalComponents.h"
#include "string/StringConvert.h"

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

    // QUESTION: 这里多线程会有问题吗？RHI如何保证线程安全
    // => RHI不做线程安全，这个应用层做的
    // => 因此，该函数执行时，需要确保没有其他线程在操作RHI资源

    // Create & Upload & Bind Textures

    const Sampler default_sampler = Sampler(ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_REPEAT);

    // All CPU→GPU uploads are recorded as a CopyScope inside the gfx CommandList.
    // The executor splits the stream at scope boundaries, routes the enclosed commands
    // to the copy queue, and auto-generates acquire/release barriers.
    {
        auto copy_scope = m_pending_cmd_lists.gfx_queue_cmd_list.BeginCopyScope();

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
                String texture_name = Utf8ToPlatform(Utf8StringView(c_name.name.data(), c_name.name.size()));
                tex_with_hdl.tex = device.CreateTexture(
                    texture_name,
                    Extent2D{c_texture.width, c_texture.height},
                    c_texture.format,
                    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST,
                    c_texture.mip_level_count,
                    c_texture.array_layer_count
                );

                // 2. Copy Data (inside CopyScope — routed to copy queue by executor)
                uint64 offset = 0;
                for (uint i = 0; i < c_texture.mip_level_count * c_texture.array_layer_count; i++) {
                    uint mip_level_byte_size = tex_with_hdl.tex->GetMipByteSize(i);
                    copy_scope.CopyFrom(
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

            auto to_hdl = [&](const entt::entity entity) -> int {
                if (entity == entt::null) {
                    return -1; // 不存在，应该使用factor
                }
                assert(
                    m_map_texture_entity_to_bindless_handle.contains(entity) && "Texture entity not found in map"
                );
                return static_cast<int>(m_map_texture_entity_to_bindless_handle.at(entity));
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
            MOER_TEXT("GpuScene::LightBuffer"),
            m_cpu_scene.m_light_buf.size() * sizeof(GLight),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        m_res.material_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::MaterialBuffer"),
            m_cpu_scene.m_material_buf.size() * sizeof(GMaterial),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        // 这里不设置为byte，是为了GeometryPass中可以直接获取 命令的数量(cpu count)、DrawIndexedCmdData的stride
        m_res.draw_cmd_buf.buf = device.CreateBuffer<Render::DrawIndexedCmdData>(
            MOER_TEXT("GpuScene::DrawCmdBuffer"),
            m_cpu_scene.m_draw_cmd_buf.size(),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
        );

        m_res.primitive_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::PrimitiveBuffer"),
            m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        m_res.instance_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::InstanceBuffer"),
            m_cpu_scene.m_instance_buf.size() * sizeof(GInstance),
            EBufferUsageFlags::UNORDERED_ACCESS
        );

        m_res.position_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::PositionMegaBuffer"),
            m_cpu_scene.mega_buf().position.size() * sizeof(float3),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
        );

        m_res.packed_normal_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::NormalMegaBuffer"),
            m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
        );

        m_res.packed_tangent_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::TangentMegaBuffer"),
            m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
        );

        m_res.texcoord0_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::Texcoord0MegaBuffer"),
            m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
        );

        m_res.index_buf.buf = device.CreateBuffer<byte>(
            MOER_TEXT("GpuScene::IndexMegaBuffer"),
            m_cpu_scene.mega_buf().index.size() * sizeof(uint32),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDEX_BUFFER
        );

        /**
         * MARK: 上传所有Buffers (inside CopyScope — routed to copy queue by executor)
         *
         * 此处按照 Res 中顺序进行上传
         */

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.m_light_buf.data(), m_cpu_scene.m_light_buf.size() * sizeof(GLight)
            ),
            m_res.light_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::LightBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.m_material_buf.data(), m_cpu_scene.m_material_buf.size() * sizeof(GMaterial)
            ),
            m_res.material_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::MaterialBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.m_draw_cmd_buf.data(),
                m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData)
            ),
            m_res.draw_cmd_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::DrawCmdBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.m_primitive_buf.data(), m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive)
            ),
            m_res.primitive_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::PrimitiveBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.m_instance_buf.data(), m_cpu_scene.m_instance_buf.size() * sizeof(GInstance)
            ),
            m_res.instance_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::InstanceBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.mega_buf().position.data(),
                m_cpu_scene.mega_buf().position.size() * sizeof(float3)
            ),
            m_res.position_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::PositionMegaBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.mega_buf().packed_normal.data(),
                m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32)
            ),
            m_res.packed_normal_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::NormalMegaBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.mega_buf().packed_tangent.data(),
                m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32)
            ),
            m_res.packed_tangent_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::TangentMegaBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.mega_buf().texcoord0.data(),
                m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2)
            ),
            m_res.texcoord0_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::Texcoord0MegaBuffer")
        );

        copy_scope.CopyFrom(
            std::span<byte>(
                (byte*)m_cpu_scene.mega_buf().index.data(), m_cpu_scene.mega_buf().index.size() * sizeof(uint32)
            ),
            m_res.index_buf.buf->GetView(),
            MOER_TEXT("CopyFrom GpuScene::IndexMegaBuffer")
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

    } // ~CopyCommandScope() — executor auto-generates copy→gfx acquire barrier here

    // InitRaytracingScene runs on the gfx queue (after copy scope, ownership returned)
    // gfx_queue交给主线程执行
    InitRaytracingScene(m_pending_cmd_lists.gfx_queue_cmd_list);
}

void GpuScene::Update(const ecs::LogicalScene& m_logical_scene, CpuScene& m_cpu_scene) {
    auto& device = RenderDevice::Get();

    // TODO: others

    UpdateRaytracingScene(m_pending_cmd_lists.gfx_queue_cmd_list); // gfx_queue交给主线程执行
}

GpuScene::~GpuScene() noexcept {
    // TODO: 释放所有资源

    // 1. Buffers, Textures

    // 2. Bindless Resources
}

void GpuScene::InitRaytracingScene(CommandList& cmd_list) {
    auto& device    = RenderDevice::Get();
    auto& r         = m_logical_scene.r();
    auto& cpu_scene = m_cpu_scene;

    /**
     * RT Scene 数据结构说明（与 Raster 保持一致）
     * 
     * 1. 思路：RT scene 和光栅化保持一致，都是 primitive 紧凑排列，instance 紧凑排列
     *    - Primitive 按 primitive_id 顺序排列（0, 1, 2, ...）
     *    - Instance 按 primitive_id 分组，每组内按 instance_idx 顺序排列
     *    - 与 CpuScene::m_instance_buf 的顺序完全一致（按 primitive_id 扁平化）
     * 
     * 2. 对应关系：
     *    - RaytracingScene：整个场景只有一个 RaytracingScene 对象（m_res.rt_scene）
     *    - TLAS：每个 RaytracingScene 包含一个 TLAS（Top-Level Acceleration Structure）
     *            - TLAS 是顶层加速结构，包含所有 TLAS Instance
     *            - 场景中只有一个 TLAS（可能还有 prev_tlas 用于双缓冲）
     *    - TLAS Instance：每个 (CPrimitive, CTransform) 对对应一个 TLAS Instance
     *            - TLAS Instance 存储在 RaytracingScene::instances 数组中
     *            - TLAS Instance 顺序 = m_instance_buf 顺序 = GInstance[] 顺序
     *            - 每个 TLAS Instance 的 custom_index = 该 Instance 在 m_instance_buf 中的索引
     *            - 每个 TLAS Instance 引用一个 BLAS（通过 instance.geom）
     *    - BLAS：每个 CPrimitive 对应一个 BLAS，存储在 m_primitive_id_to_blas[primitive_id]
     * 
     * 3. Shader 调用 ray trace inline 时，如何访问（对应 GBufferRT 中 ray_query 的取值）：
     *    - CandidateInstanceID()：    instance_buf的索引。来自 instance.custom_index
     *    - CandidateGeometryIndex()： === 0
     *    - CandidatePrimitiveIndex()：Primitive的三角形索引（0-based）
     */

    // 获取共享的 vertex 和 index buffers
    BufferRef position_buf_ref = m_res.position_buf.buf;
    BufferRef index_buf_ref    = m_res.index_buf.buf;

    Array<AccelerationStructureBuildParam> build_params;
    Array<RaytracingGeometryRef>           built_geometries;

    m_res.rt_scene = device.CreateRaytracingScene();

    // 与 Raster 一致：按 primitive_id 建 BLAS，TLAS Instance 顺序 = m_instance_buf 顺序
    const uint primitive_count = cpu_scene.GetPrimitiveCount();
    Array<bool> primitive_has_rt_instance(primitive_count, false);
    for (uint primitive_id = 0; primitive_id < primitive_count; ++primitive_id) {
        primitive_has_rt_instance[primitive_id] = cpu_scene.GetInstanceCountForPrimitive(primitive_id) != 0;
    }

    m_primitive_id_to_blas.clear();
    m_primitive_id_to_blas.resize(primitive_count);
    build_params.reserve(primitive_count);
    built_geometries.reserve(primitive_count);

    r.view<const ecs::CPrimitive>().each([&](const auto primitive_entt, const ecs::CPrimitive& c_primitive) {
        const uint primitive_id = cpu_scene.GetPrimitiveId(primitive_entt);
        if (primitive_id == UINT_MAX || primitive_id >= primitive_count) {
            return;
        }

        if (!primitive_has_rt_instance[primitive_id]) {
            return;
        }

        if (!c_primitive.position.is_valid || !c_primitive.index.is_valid) {
            return;
        }

        uint vtx_offset = c_primitive.position.start_idx;
        uint vtx_count  = c_primitive.vertex_count;
        uint idx_offset = c_primitive.index.start_idx;
        uint idx_count  = c_primitive.index_count;

        RaytracingGeometryInfo rt_geo_info{};
        rt_geo_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
        rt_geo_info.vertex_format = PF_R32G32B32_SFLOAT;
        rt_geo_info.index_type    = IET_UINT32;

        rt_geo_info.segments.emplace_back(
            0,
            0,
            vtx_offset,
            vtx_count,
            sizeof(float3),
            idx_offset / 3,
            idx_count / 3,
            position_buf_ref,
            index_buf_ref,
            RTGT_TRIANGLES,
            ERayTracingGeometryFlags::GEOMETRY_OPAQUE,
            false,
            false,
            false
        );

        RaytracingGeometryRef blas           = device.CreateRaytracingGeometry(rt_geo_info);
        m_primitive_id_to_blas[primitive_id] = blas;
        build_params.push_back({blas, ERaytracingBuildMode::BUILD});
        built_geometries.push_back(blas);
    });

    // TLAS：按 primitive_id 升序、再按 instance_idx，与 m_instance_buf 顺序一致
    for (uint primitive_id = 0; primitive_id < primitive_count; ++primitive_id) {
        RaytracingGeometryRef& blas_ref = m_primitive_id_to_blas[primitive_id];
        if (!blas_ref.IsValid()) {
            continue;
        }

        const uint instance_count = cpu_scene.GetInstanceCountForPrimitive(primitive_id);
        for (uint instance_idx = 0; instance_idx < instance_count; ++instance_idx) {
            const GInstance& ginst = cpu_scene.GetInstanceForPrimitive(primitive_id, instance_idx);

            auto& instance = m_res.rt_scene->AddInstance();
            instance.geom  = blas_ref;
            // Vulkan TLAS 需要 3x4 行主序（M 的前三行）；ginst.world_transform 存的是列主序 M（即 r0..r3 为 M 的列）
            instance.transform        = ginst.world_transform.ToTransposedMatrix3x4f();
            instance.flag.need_create = true;
            instance.custom_index     = static_cast<uint>(m_res.rt_scene->GetInstanceCount() - 1);
            instance.visible_mask     = RTVM_ALL;
            m_res.rt_scene->MarkModified(instance.instance_id);
        }
    }

    if (!build_params.empty()) {
        Array<BarrierCreateInfo> build_input_barriers{};
        build_input_barriers.reserve(2);
        build_input_barriers.push_back(BarrierCreateInfo::Transition(
            position_buf_ref->GetView(),
            EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT,
            EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT,
            EPassType::Raytracing
        ));
        build_input_barriers.push_back(BarrierCreateInfo::Transition(
            index_buf_ref->GetView(),
            EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT,
            EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT,
            EPassType::Raytracing
        ));
        cmd_list.Barriers(
            std::span<const BarrierCreateInfo>(build_input_barriers.data(), build_input_barriers.size()),
            EQueueType::Graphics,
            EQueueType::Graphics
        );

        for (const RaytracingGeometryRef& geometry : built_geometries) {
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Raytracing,
                WriteRaytracingGeometry{geometry, EBufferState::ACCELERATION_STRUCTURE_WRITE}
            );
        }

        cmd_list.BuildAccelerationStructures(std::move(build_params));

        for (const RaytracingGeometryRef& geometry : built_geometries) {
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Raytracing,
                ReadRaytracingGeometry{geometry, EBufferState::ACCELERATION_STRUCTURE_READ}
            );
        }
    }
    cmd_list.UpdateRaytracingScene(m_res.rt_scene);

    // 这里不应该提交命令！
    // 这个函数会在 非主线程 被执行，在这里提交命令，会导致gfx_queue死锁
    // RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Execute(cmd_list.Submit());
    // RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
}

void GpuScene::UpdateRaytracingScene(CommandList& cmd_list) {
    auto& cpu_scene = m_cpu_scene;

    // 与 InitRaytracingScene 相同顺序：按 primitive_id、再 instance_idx，用 CpuScene getter 更新 transform
    const uint primitive_count   = cpu_scene.GetPrimitiveCount();
    uint       tlas_instance_idx = 0;

    for (uint primitive_id = 0; primitive_id < primitive_count; ++primitive_id) {
        if (!m_primitive_id_to_blas[primitive_id].IsValid()) {
            continue;
        }

        const uint instance_count = cpu_scene.GetInstanceCountForPrimitive(primitive_id);
        for (uint instance_idx = 0; instance_idx < instance_count; ++instance_idx) {
            if (tlas_instance_idx < m_res.rt_scene->GetInstanceCount()) {
                const GInstance& ginst    = cpu_scene.GetInstanceForPrimitive(primitive_id, instance_idx);
                auto&            instance = m_res.rt_scene->GetInstance(tlas_instance_idx);
                instance.transform        = ginst.world_transform.ToTransposedMatrix3x4f();
                m_res.rt_scene->MarkModified(instance.instance_id);
            }
            tlas_instance_idx++;
        }
    }

    cmd_list.UpdateRaytracingScene(m_res.rt_scene);
}

void GpuScene::RestoreDrawCommands(CommandList& cmd_list) {
    // 从 CPU 数据重新上传 draw_cmd_buf，恢复原始 instance_cnt
    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_draw_cmd_buf.data(),
            m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        MOER_TEXT("RestoreDrawCommands")
    );
}

} // namespace Moer::Render