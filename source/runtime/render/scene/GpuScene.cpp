#include "GpuScene.h"

#include "CpuScene.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "scene/LogicalComponents.h"

namespace Moer::Render {

static bool RecreateByteBufferIfNeeded(
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

static void UploadByteBuffer(
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

    cmd_list.CopyFrom(std::span<byte>((byte*)data, required_byte_size), target.buf->GetView(), name);

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(bindless_array);
    }
}

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
                m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
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

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_light_buf.data(), m_cpu_scene.m_light_buf.size() * sizeof(GLight)
        ),
        m_res.light_buf.buf->GetView(),
        "CopyFrom GpuScene::LightBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_material_buf.data(), m_cpu_scene.m_material_buf.size() * sizeof(GMaterial)
        ),
        m_res.material_buf.buf->GetView(),
        "CopyFrom GpuScene::MaterialBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_draw_cmd_buf.data(),
            m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        "CopyFrom GpuScene::DrawCmdBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_primitive_buf.data(), m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive)
        ),
        m_res.primitive_buf.buf->GetView(),
        "CopyFrom GpuScene::PrimitiveBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_instance_buf.data(), m_cpu_scene.m_instance_buf.size() * sizeof(GInstance)
        ),
        m_res.instance_buf.buf->GetView(),
        "CopyFrom GpuScene::InstanceBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().position.data(),
            m_cpu_scene.mega_buf().position.size() * sizeof(float3)
        ),
        m_res.position_buf.buf->GetView(),
        "CopyFrom GpuScene::PositionMegaBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().packed_normal.data(),
            m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32)
        ),
        m_res.packed_normal_buf.buf->GetView(),
        "CopyFrom GpuScene::NormalMegaBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().packed_tangent.data(),
            m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32)
        ),
        m_res.packed_tangent_buf.buf->GetView(),
        "CopyFrom GpuScene::TangentMegaBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.mega_buf().texcoord0.data(),
            m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2)
        ),
        m_res.texcoord0_buf.buf->GetView(),
        "CopyFrom GpuScene::Texcoord0MegaBuffer"
    );

    m_pending_cmd_lists.copy_queue_cmd_list.CopyFrom(
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

    // 1. copy queue

    m_pending_cmd_lists.copy_queue_cmd_list.ExportResourcesToQueue(
        EQueueType::Graphics, std::move(export_tex), std::move(export_buf)
    );

    // 2. graphics queue
    // - 为了避免多线程抢占gfx_queue资源，导致卡死。我们规定只在主线程执行gfx_queue命令

    m_pending_cmd_lists.gfx_queue_cmd_list.ImportResourcesFromQueue( // gfx_queue交给主线程执行
        EQueueType::Copy, std::move(import_tex), std::move(import_buf)
    );

    InitRaytracingScene(m_pending_cmd_lists.gfx_queue_cmd_list); // gfx_queue交给主线程执行
}

void GpuScene::Update(const ecs::LogicalScene& m_logical_scene, CpuScene& m_cpu_scene, bool rebuilt_mesh) {
    UpdateLightBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
    UpdateMaterialBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);

    if (rebuilt_mesh) {
        UpdateDrawCommandBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdatePrimitiveBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdateInstanceBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdatePositionMegaBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdatePackedNormalMegaBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdatePackedTangentMegaBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdateTexcoord0MegaBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);
        UpdateIndexMegaBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);

        // Renderable 结构变化会改变 TLAS instance 数量，当前直接整批重建 RT scene
        InitRaytracingScene(m_pending_cmd_lists.gfx_queue_cmd_list);
        return;
    }

    UpdateInstanceBuffer(m_pending_cmd_lists.gfx_queue_cmd_list);

    UpdateRaytracingScene(m_pending_cmd_lists.gfx_queue_cmd_list); // gfx_queue交给主线程执行
}

// 同步 CPU light cache 到 GPU light buffer，必要时重建 bindless buffer。
void GpuScene::UpdateLightBuffer(CommandList& cmd_list) {
    const uint64 required_byte_size = m_cpu_scene.m_light_buf.size() * sizeof(GLight);
    if (required_byte_size == 0) {
        return;
    }

    bool need_bindless_update = false;
    if (m_res.light_buf.buf == nullptr || m_res.light_buf.buf->GetByteSize() < required_byte_size) {
        auto& device = RenderDevice::Get();

        if (m_res.light_buf.hdl != 0) {
            m_bindless_array->UnbindBuffer(m_res.light_buf.hdl);
            m_res.light_buf.hdl = 0;
        }

        // TODO: debug 阶段 light 数量很小，先按当前需求大小重建；后续改为 capacity/chunk 策略和局部更新。
        m_res.light_buf.buf = device.CreateBuffer<byte>(
            "GpuScene::LightBuffer", required_byte_size, EBufferUsageFlags::UNORDERED_ACCESS
        );
        m_res.light_buf.hdl  = m_bindless_array->AllocateBuffer(m_res.light_buf.buf->GetView());
        need_bindless_update = true;
    }

    cmd_list.CopyFrom(
        std::span<byte>((byte*)m_cpu_scene.m_light_buf.data(), required_byte_size),
        m_res.light_buf.buf->GetView(),
        "CopyFrom GpuScene::LightBuffer"
    );

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(m_bindless_array);
    }
}

void GpuScene::UpdateMaterialBuffer(CommandList& cmd_list) {
    const uint64 required_byte_size = m_cpu_scene.m_material_buf.size() * sizeof(GMaterial);
    if (required_byte_size == 0) {
        return;
    }

    bool need_bindless_update = false;
    if (m_res.material_buf.buf == nullptr || m_res.material_buf.buf->GetByteSize() < required_byte_size) {
        auto& device = RenderDevice::Get();

        if (m_res.material_buf.hdl != 0) {
            m_bindless_array->UnbindBuffer(m_res.material_buf.hdl);
            m_res.material_buf.hdl = 0;
        }

        m_res.material_buf.buf = device.CreateBuffer<byte>(
            "GpuScene::MaterialBuffer", required_byte_size, EBufferUsageFlags::UNORDERED_ACCESS
        );
        m_res.material_buf.hdl = m_bindless_array->AllocateBuffer(m_res.material_buf.buf->GetView());
        need_bindless_update   = true;
    }

    cmd_list.CopyFrom(
        std::span<byte>((byte*)m_cpu_scene.m_material_buf.data(), required_byte_size),
        m_res.material_buf.buf->GetView(),
        "CopyFrom GpuScene::MaterialBuffer"
    );

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(m_bindless_array);
    }
}

void GpuScene::UpdateDrawCommandBuffer(CommandList& cmd_list) {
    const uint64 required_byte_size = m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData);
    if (required_byte_size == 0) {
        return;
    }

    bool need_bindless_update = false;
    if (m_res.draw_cmd_buf.buf == nullptr || m_res.draw_cmd_buf.buf->GetByteSize() < required_byte_size) {
        auto& device = RenderDevice::Get();

        if (m_res.draw_cmd_buf.hdl != 0) {
            m_bindless_array->UnbindBuffer(m_res.draw_cmd_buf.hdl);
            m_res.draw_cmd_buf.hdl = 0;
        }

        m_res.draw_cmd_buf.buf = device.CreateBuffer<Render::DrawIndexedCmdData>(
            "GpuScene::DrawCmdBuffer",
            m_cpu_scene.m_draw_cmd_buf.size(),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
        );
        m_res.draw_cmd_buf.hdl = m_bindless_array->AllocateBuffer(m_res.draw_cmd_buf.buf->GetView());
        need_bindless_update   = true;
    }

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)m_cpu_scene.m_draw_cmd_buf.data(),
            m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData)
        ),
        m_res.draw_cmd_buf.buf->GetView(),
        "CopyFrom GpuScene::DrawCmdBuffer"
    );

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(m_bindless_array);
    }
}

void GpuScene::UpdateInstanceBuffer(CommandList& cmd_list) {
    const uint64 required_byte_size = m_cpu_scene.m_instance_buf.size() * sizeof(GInstance);
    if (required_byte_size == 0) {
        return;
    }

    bool need_bindless_update = false;
    if (m_res.instance_buf.buf == nullptr || m_res.instance_buf.buf->GetByteSize() < required_byte_size) {
        auto& device = RenderDevice::Get();

        if (m_res.instance_buf.hdl != 0) {
            m_bindless_array->UnbindBuffer(m_res.instance_buf.hdl);
            m_res.instance_buf.hdl = 0;
        }

        m_res.instance_buf.buf = device.CreateBuffer<byte>(
            "GpuScene::InstanceBuffer", required_byte_size, EBufferUsageFlags::UNORDERED_ACCESS
        );
        m_res.instance_buf.hdl = m_bindless_array->AllocateBuffer(m_res.instance_buf.buf->GetView());
        need_bindless_update   = true;
    }

    cmd_list.CopyFrom(
        std::span<byte>((byte*)m_cpu_scene.m_instance_buf.data(), required_byte_size),
        m_res.instance_buf.buf->GetView(),
        "CopyFrom GpuScene::InstanceBuffer"
    );

    if (need_bindless_update) {
        cmd_list.UpdateBindlessArray(m_bindless_array);
    }
}

void GpuScene::UpdatePrimitiveBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.primitive_buf,
        "GpuScene::PrimitiveBuffer",
        m_cpu_scene.m_primitive_buf.data(),
        m_cpu_scene.m_primitive_buf.size() * sizeof(GPrimitive),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
}

void GpuScene::UpdatePositionMegaBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.position_buf,
        "GpuScene::PositionMegaBuffer",
        m_cpu_scene.mega_buf().position.data(),
        m_cpu_scene.mega_buf().position.size() * sizeof(float3),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdatePackedNormalMegaBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.packed_normal_buf,
        "GpuScene::NormalMegaBuffer",
        m_cpu_scene.mega_buf().packed_normal.data(),
        m_cpu_scene.mega_buf().packed_normal.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdatePackedTangentMegaBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.packed_tangent_buf,
        "GpuScene::TangentMegaBuffer",
        m_cpu_scene.mega_buf().packed_tangent.data(),
        m_cpu_scene.mega_buf().packed_tangent.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdateTexcoord0MegaBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.texcoord0_buf,
        "GpuScene::Texcoord0MegaBuffer",
        m_cpu_scene.mega_buf().texcoord0.data(),
        m_cpu_scene.mega_buf().texcoord0.size() * sizeof(float2),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::VERTEX_BUFFER
    );
}

void GpuScene::UpdateIndexMegaBuffer(CommandList& cmd_list) {
    UploadByteBuffer(
        cmd_list,
        m_bindless_array,
        m_res.index_buf,
        "GpuScene::IndexMegaBuffer",
        m_cpu_scene.mega_buf().index.data(),
        m_cpu_scene.mega_buf().index.size() * sizeof(uint32),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDEX_BUFFER
    );
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
     *    - TLAS Instance：每个 (CPrimitive, CNode) 对对应一个 TLAS Instance
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

    m_res.rt_scene = device.CreateRaytracingScene();

    // 与 Raster 一致：按 primitive_id 建 BLAS，TLAS Instance 顺序 = m_instance_buf 顺序
    const uint primitive_count = cpu_scene.GetPrimitiveCount();
    m_primitive_id_to_blas.clear();
    m_primitive_id_to_blas.resize(primitive_count);

    r.view<const ecs::CPrimitive>().each([&](const auto primitive_entt, const ecs::CPrimitive& c_primitive) {
        const uint primitive_id = cpu_scene.GetPrimitiveId(primitive_entt);
        if (primitive_id == UINT_MAX || primitive_id >= primitive_count) {
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

    cmd_list.BuildAccelerationStructures(std::move(build_params));
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
        "RestoreDrawCommands"
    );
}

} // namespace Moer::Render