#include "resources/GpuScene.h"

#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResourceInitilizer.h"

namespace Moer {

class GpuPrimitiveBuilder::Impl {
public:
    void        Vertex(const Moer::Array<float>* data);
    void        Index(const Moer::Array<uint32_t>* data);
    void        Attribute(VertexAttributeFlags attribute);
    static void InitBuild();
    static void EndBuild();
    bool        Validate() const;

protected:
    const Moer::Array<float>*    m_vertex_data{nullptr};
    const Moer::Array<uint32_t>* m_index_data{nullptr};
    uint8_t                      m_attribute{0};
};

void GpuPrimitiveBuilder::Impl::Vertex(const Moer::Array<float>* data) {
    this->m_vertex_data = data;
}

void GpuPrimitiveBuilder::Impl::Index(const Moer::Array<uint32_t>* data) {
    this->m_index_data = data;
}

void GpuPrimitiveBuilder::Impl::Attribute(VertexAttributeFlags attribute) {
    this->m_attribute = attribute;
}

bool GpuPrimitiveBuilder::Impl::Validate() const {
    return m_vertex_data != nullptr && m_index_data != nullptr;
}

void GpuPrimitiveBuilder::Impl::InitBuild() {
    // copy_cmd_list = g_rhi->RHICreateCopyCommandList();
    // copy_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
}

void GpuPrimitiveBuilder::Impl::EndBuild() {
    // copy_cmd_list = nullptr;
    // copy_queue    = nullptr;
}

void GpuPrimitiveBuilder::InitBuild() {
    Impl::InitBuild();
}

void GpuPrimitiveBuilder::EndBuild() {
    Impl::EndBuild();
}

GpuPrimitiveBuilder& GpuPrimitiveBuilder::Vertex(const Moer::Array<float>* data) {
    m_impl->Vertex(data);
    return *this;
}
GpuPrimitiveBuilder& GpuPrimitiveBuilder::Index(const Moer::Array<uint32_t>* data) {
    m_impl->Index(data);
    return *this;
}
GpuPrimitiveBuilder& GpuPrimitiveBuilder::Attribute(VertexAttributeFlags attribute) {
    m_impl->Attribute(attribute);
    return *this;
}

GpuPrimitiveBuilder::GpuPrimitiveBuilder() {
    m_impl = new Impl();
}

GpuPrimitiveBuilder::~GpuPrimitiveBuilder() {
    delete m_impl;
}

bool GpuPrimitiveBuilder::Validate() const {
    return m_impl->Validate();
}
TextureBuilder& TextureBuilder::Width(uint32_t width) noexcept {
    m_width = width;
    return *this;
}
TextureBuilder& TextureBuilder::Height(uint32_t height) noexcept {
    m_height = height;
    return *this;
}
TextureBuilder& TextureBuilder::Depth(uint32_t depth) noexcept {
    m_depth = depth;
    return *this;
}
TextureBuilder& TextureBuilder::Format(EPixelFormat format) noexcept {
    m_format = format;
    return *this;
}
TextureBuilder& TextureBuilder::MipAndLayers(
    uint32_t        mip_levels,
    uint32_t        layer_levels,
    const uint32_t* offsets,
    const Extent3D* extents
) noexcept {
    m_mip_levels   = mip_levels;
    m_layer_levels = layer_levels;
    m_offsets      = new uint32_t[mip_levels * layer_levels];
    m_mip_extents  = new Extent3D[mip_levels * layer_levels];
    memcpy(m_offsets, offsets, mip_levels * layer_levels * sizeof(uint32_t));
    memcpy(m_mip_extents, extents, mip_levels * layer_levels * sizeof(Extent3D));
    return *this;
}
TextureBuilder& TextureBuilder::CallBack(Callback callback) noexcept {
    m_callback = callback;
    return *this;
}
TextureBuilder& TextureBuilder::Data(void* data, uint32_t data_size) noexcept {
    m_data_size = data_size;
    m_data      = data;
    return *this;
}
TextureBuilder& TextureBuilder::Name(const std::string& name) noexcept {
    m_name = name;
    return *this;
}
TextureBuilder::~TextureBuilder() noexcept {
    if (m_data && m_callback)
        m_callback(m_data);
    if (m_offsets)
        delete[] m_offsets;
    if (m_mip_extents)
        delete[] m_mip_extents;
}
Moer::UnorderedMap<std::string, Render::TextureRef>
TextureBuilder::BuildTexturesInBatch(Moer::Array<TextureBuilder>& builders) noexcept {
    const int                                           batch_size = 256'000'000;
    Moer::UnorderedMap<std::string, Render::TextureRef> textures(builders.size());
    Moer::Array<Moer::Array<uint32_t>>                  batch_indices;

    {
        auto* indices  = &batch_indices.emplace_back();
        int   cur_size = 0;
        for (int i = 0; i < builders.size(); i++) {
            if (cur_size + builders[i].m_data_size > batch_size) {
                indices  = &batch_indices.emplace_back();
                cur_size = 0;
            }
            cur_size += builders[i].m_data_size;
            indices->emplace_back(i);
        }
    }

    auto&               device     = Render::RenderDevice::Get();
    auto&               copy_queue = device.GetCopyQueue();
    Render::CommandList cmd_list{};
    for (const auto& indices : batch_indices) {
        int count = 0;
        for (auto& indice : indices) {
            auto& builder            = builders[indice];
            textures[builder.m_name] = device.CreateTexture(
                builder.m_name,
                Extent2D{builder.m_width, builder.m_height},
                builder.m_format,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::SRGB | ETextureUsageFlags::TRANSFER_DST,
                builder.m_mip_levels,
                builder.m_layer_levels
            );

            // 原来这里是手动生成MIPMAP的！没有使用硬件生成MIPMAP！
            // 问了下AI，发现这么做的是工业界标准。手动生成MIPMAP的优势还挺多的
            auto   target_texture = textures[builder.m_name];
            uint64 offset         = 0;
            for (uint i = 0; i < builder.m_mip_levels * builder.m_layer_levels; i++) {
                uint mip_size = target_texture->GetMipByteSize(i);
                cmd_list.CopyFrom(
                    std::span<byte>((byte*)builder.m_data + offset, mip_size),
                    textures[builder.m_name]->GetView(i, 1)
                );
                offset += mip_size;
            }
            count++;
        }
    }
    auto evt = copy_queue.Execute(cmd_list.Submit());
    copy_queue.Sync(evt.timeline);
    return textures;
}

class GpuSceneBufferBuilder::Impl {
    friend GpuSceneBufferBuilder;
    const Moer::Array<float>*    m_vertex_data{nullptr};
    const Moer::Array<uint32_t>* m_index_data{nullptr};
};

GpuSceneBufferBuilder& GpuSceneBufferBuilder::Vertex(const Moer::Array<float>* data) {
    m_impl->m_vertex_data = data;
    return *this;
}
GpuSceneBufferBuilder& GpuSceneBufferBuilder::Index(const Moer::Array<uint32_t>* data) {
    m_impl->m_index_data = data;
    return *this;
}
GpuSceneBufferBuilder::GpuSceneBufferBuilder() {
    m_impl = new Impl();
}
GpuSceneBufferBuilder::~GpuSceneBufferBuilder() {
    delete m_impl;
}

} // namespace Moer