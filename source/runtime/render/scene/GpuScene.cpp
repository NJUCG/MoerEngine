#pragma once

#include "GpuScene.h"

#include "CpuScene.h"
#include "rhi/RHI.h"

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
        auto& view = m_logical_scene.r().view<const ecs::CTexture, const ecs::CName>();

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
                    tex_with_hdl.tex->GetView(i, 1),
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

        auto& view = m_logical_scene.r().view<const ecs::CMaterial>();

        auto to_hdl = [&](const entt::entity entity) -> int64 {
            if (entity == entt::null) {
                return -1; // 不存在，应该使用factor
            } else {
                assert(
                    m_map_texture_entity_to_bindless_handle.contains(entity) &&
                    "Texture entity not found in map"
                );
                return m_map_texture_entity_to_bindless_handle.at(entity);
            }
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

    m_res.draw_cmd_buf.buf = device.CreateBuffer<byte>(
        "GpuScene::DrawCmdBuffer",
        m_cpu_scene.m_draw_cmd_buf.size() * sizeof(Render::DrawIndexedCmdData),
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

    /**
     * MARK: Upload & Execute
     * 
     * 这里这段代码我不理解，我对GraphicsAPI底层的各种queue和barrier不够熟悉
     * 这部分代码是从旧的SceneCache.cpp中抄过来的
     */

    Array<ExportTexture> export_tex(m_res.texture_array.size());
    Array<ExportBuffer>  export_buf(buffers.size());

    Array<ImportTexture> import_tex(m_res.texture_array.size());
    Array<ImportBuffer>  import_buf(buffers.size());

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

        auto& fence = copy_queue.GetFenceHandle();
        auto& evt   = copy_queue.Execute(cmd_list.Submit().Wait(fence, fence->GetValue()));
        copy_queue.Sync(evt.timeline);
    }

    {

        cmd_list.ImportResourcesFromQueue(EQueueType::Copy, std::move(import_tex), std::move(import_buf));

        gfx_queue.Execute(cmd_list.Submit());
        gfx_queue.Sync();
    }
}

void GpuScene::Update(const ecs::LogicalScene& m_logical_scene, CpuScene& m_cpu_scene) {
    // TODO
}

} // namespace Moer::Render