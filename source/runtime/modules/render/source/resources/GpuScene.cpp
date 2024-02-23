#include "resources/GpuScene.h"

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResourceInitilizer.h"

namespace Moer {

    class GpuPrimitiveBuilder::Impl {
    public:
        void                  Vertex(const Moer::Array<float>* data);
        void                  Index(const Moer::Array<uint32_t>* data);
        void                  Attribute(VertexAttributeFlags attribute);
        static void           InitBuild();
        static void           EndBuild();
        RHIRenderPrimitiveRef Build();
        bool                  Validate() const;

    protected:
        static RHICommandQueue*    copy_queue;
        static RHICopyCommandList* copy_cmd_list;

        const Moer::Array<float>*    m_vertex_data{nullptr};
        const Moer::Array<uint32_t>* m_index_data{nullptr};
        uint8_t                      m_attribute{0};
    };

    RHICommandQueue*    GpuPrimitiveBuilder::Impl::copy_queue    = nullptr;
    RHICopyCommandList* GpuPrimitiveBuilder::Impl::copy_cmd_list = nullptr;

    void GpuPrimitiveBuilder::Impl::Vertex(const Moer::Array<float>* data) {
        this->m_vertex_data = data;
    }

    void GpuPrimitiveBuilder::Impl::Index(const Moer::Array<uint32_t>* data) {
        this->m_index_data = data;
    }

    void GpuPrimitiveBuilder::Impl::Attribute(VertexAttributeFlags attribute) {
        this->m_attribute = attribute;
    }

    RHIRenderPrimitiveRef GpuPrimitiveBuilder::Impl::Build() {
        auto* cmd_list = copy_cmd_list;
        cmd_list->BeginRecording();

        uint32_t vertex_buffer_size = m_vertex_data->size() * sizeof(float);
        uint32_t index_buffer_size  = m_index_data->size() * sizeof(uint32_t);

        RHIBufferCreateInfo vertex_buffer_create_info(vertex_buffer_size, sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
        RHIBufferRef        vertex_buffer = g_rhi->RHICreateBuffer(vertex_buffer_create_info);
        RHIBufferCreateInfo staging_vertex_buffer_create_info(vertex_buffer_size, sizeof(float), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
        RHIBufferRef        staging_vertex_buffer = g_rhi->RHICreateBuffer(staging_vertex_buffer_create_info);

        auto* staging_vertex_buffer_mapped_ptr = static_cast<float*>(g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, vertex_buffer_size));
        memcpy(staging_vertex_buffer_mapped_ptr, m_vertex_data->data(), vertex_buffer_size);
        g_rhi->RHIUnmapBuffer(staging_vertex_buffer);

        Array<RHIBufferRegion> vertex_buffer_region_array({RHIBufferRegion{.src_offset = 0, .dst_offset = 0, .size = vertex_buffer_size}});
        RHICopyBufferInfo      vertex_copy_buffer_info{};
        vertex_copy_buffer_info.regions = vertex_buffer_region_array;
        cmd_list->CopyBuffer(vertex_copy_buffer_info, staging_vertex_buffer, vertex_buffer);

        RHIBufferCreateInfo index_buffer_create_info(index_buffer_size, sizeof(uint32_t), EBufferUsageFlags::INDEX_BUFFER);
        RHIBufferRef        index_buffer = g_rhi->RHICreateBuffer(index_buffer_create_info);
        RHIBufferCreateInfo staging_index_buffer_create_info(index_buffer_size, sizeof(uint32_t), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
        RHIBufferRef        staging_index_buffer = g_rhi->RHICreateBuffer(staging_index_buffer_create_info);

        auto* staging_index_buffer_mapped_ptr = static_cast<uint32_t*>(g_rhi->RHIMapBuffer(staging_index_buffer, 0, index_buffer_size));
        memcpy(staging_index_buffer_mapped_ptr, m_index_data->data(), index_buffer_size);
        g_rhi->RHIUnmapBuffer(staging_index_buffer);

        Array<RHIBufferRegion> index_buffer_region_array({RHIBufferRegion{.src_offset = 0, .dst_offset = 0, .size = index_buffer_size}});
        RHICopyBufferInfo      index_copy_buffer_info{};
        index_copy_buffer_info.regions = index_buffer_region_array;
        cmd_list->CopyBuffer(index_copy_buffer_info, staging_index_buffer, index_buffer);

        cmd_list->EndRecording();

        RHIFenceRef   fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::BINARY});
        RHISubmitInfo submit_info;
        submit_info.Signal(fence, 1);
        copy_queue->SubmitCommands(1, cmd_list, &submit_info);
        copy_queue->WaitForQueueComplete();

        RHIRenderPrimitiveRef primitive = new RHIRenderPrimitive(vertex_buffer, index_buffer, EPrimitiveType::TRIANGLES, 0, m_index_data->size());
        return primitive;
    }

    bool GpuPrimitiveBuilder::Impl::Validate() const {
        return m_vertex_data != nullptr && m_index_data != nullptr;
    }

    void GpuPrimitiveBuilder::Impl::InitBuild() {
        copy_cmd_list = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        copy_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
    }

    void GpuPrimitiveBuilder::Impl::EndBuild() {
        copy_cmd_list = nullptr;
        copy_queue    = nullptr;
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
    RHIRenderPrimitiveRef GpuPrimitiveBuilder::Build() {
        assert(Validate() && "GpuPrimitiveBuilder::Build() called without valid data");
        return m_impl->Build();
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
    TextureBuilder& TextureBuilder::MipLevels(uint32_t mipLevels) noexcept {
        m_mipLevels = mipLevels;
        return *this;
    }
    TextureBuilder& TextureBuilder::CallBack(Callback callback) noexcept {
        m_callback = callback;
        return *this;
    }
    TextureBuilder& TextureBuilder::Data(void* data) noexcept {
        m_data = data;
        return *this;
    }
    TextureBuilder::~TextureBuilder() noexcept {
        if (m_data)
            m_callback(m_data);
    }

    RHITextureRef TextureBuilder::Build() noexcept {
        auto           texture      = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create("GuiFontTexture2D", ETextureDimension::TEX_2D)
                                                   .SetNumSamples(1)
                                                   .SetExtent({static_cast<int>(m_width), static_cast<int>(m_height)})
                                                   .SetNumMips(1)
                                                   .SetArraySize(1)
                                                   .SetFormat(m_format)
                                                   .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::SRGB | ETextureUsageFlags::TRANSFER_DST)
                                                   .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));
        const uint32_t alignment    = 256;
        uint32_t       upload_pitch = (m_width * 4 + alignment - 1u) & ~(alignment - 1u);
        uint32_t       upload_size  = m_height * upload_pitch;

        RHIBufferRef staging_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(upload_size, 0, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));

        assert(texture.Get() && staging_buffer.Get());

        void* mapped = g_rhi->RHIMapBuffer(staging_buffer, 0, upload_size);
        // for (int32_t y = 0; y < height; y++) {
        //     memcpy((void*)((uint8_t*)mapped + y * upload_pitch), pixels + y * width * 4, width * 4);
        // }
        memcpy(mapped, m_data, upload_size);

        g_rhi->RHIUnmapBuffer(staging_buffer);

        RHISubresourceRange range{ETextureAspectFlags::COLOR,
                                  0,
                                  1,
                                  0,
                                  1,
                                  0,
                                  1};

        RHITextureBarrierInfo tex_barriers[2];

        tex_barriers[0].src_layout = TEXTURE_LAYOUT_UNDEFINED;
        tex_barriers[0].dst_layout = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[0].src_access = ERHIAccessFlags::UNDEFINED;
        tex_barriers[0].dst_access = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[0].dst_stage  = PS_TRANSFER;

        tex_barriers[0].p_texture          = texture;
        tex_barriers[0].sub_resource_range = range;

        tex_barriers[1].src_layout         = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[1].dst_layout         = TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex_barriers[1].src_access         = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[1].dst_access         = ERHIAccessFlags::SHADER_READ;
        tex_barriers[1].src_stage          = PS_TRANSFER;
        tex_barriers[1].dst_stage          = PS_FRAGMENT_SHADER;
        tex_barriers[1].p_texture          = texture;
        tex_barriers[1].sub_resource_range = range;

        RHIGraphicsCommandList* command_list = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());

        RHIBarrierDependencyInfo font_create_barriers{};
        font_create_barriers.texture_barriers.resize(1);
        font_create_barriers.texture_barriers[0] = tex_barriers[0];

        command_list->BeginRecording();
        command_list->SetPipelineBarrier(font_create_barriers);

        RHISubresourceSlice        resource_slice(ETextureAspectFlags::COLOR, 0, 0, 1, 0, 1);
        RHICopyBufferToTextureInfo copy_info(
            ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST,
            {0, 0, 0},
            {(uint32_t)m_width, m_height, 1},
            resource_slice,
            0);

        // 3. MARK: pRegion[0] is trying to copy 518144 bytes plus 0 offset to/from the VkBuffer (VkBuffer 0xcb1c7c000000001b[]) which exceeds the VkBuffer total size of 131072 bytes.
        command_list->CopyBufferToTexture(copy_info, staging_buffer, texture);

        RHIBarrierDependencyInfo font_copy_barriers{};
        font_copy_barriers.texture_barriers.resize(1);
        font_copy_barriers.texture_barriers[0] = tex_barriers[1];

        command_list->SetPipelineBarrier(font_copy_barriers);

        command_list->EndRecording();

        RHICommandQueue* queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

        RHIFenceCreateInfo fence_info{EFenceUsageFlags::TIMELINE};
        RHIFenceRef        fence = g_rhi->RHICreateFence(fence_info);

        RHISubmitInfo submit_info;

        uint64_t wait_value = 1;
        submit_info.Signal(fence, wait_value);
        queue->SubmitCommands(1, command_list, &submit_info);

        fence->Wait(wait_value);

        return texture;
    }

    class GpuSceneBufferBuilder::Impl {
        friend GpuSceneBufferBuilder;
        const Moer::Array<float>*             m_vertex_data{nullptr};
        const Moer::Array<uint32_t>*          m_index_data{nullptr};
        std::pair<RHIBufferRef, RHIBufferRef> Build();
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
    std::pair<RHIBufferRef, RHIBufferRef> GpuSceneBufferBuilder::Build() {
        return m_impl->Build();
    }
    RHIBufferRef GpuSceneBufferBuilder::CopyFrom(const void* data, uint32_t size) {
        RHIBufferCreateInfo staging_buffer_create_info(size, 0, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
        RHIBufferRef        staging_buffer            = g_rhi->RHICreateBuffer(staging_buffer_create_info);
        auto*               staging_buffer_mapped_ptr = static_cast<uint8_t*>(g_rhi->RHIMapBuffer(staging_buffer, 0, size));
        memcpy(staging_buffer_mapped_ptr, data, size);
        g_rhi->RHIUnmapBuffer(staging_buffer);

        RHIBufferRef buffer = g_rhi->RHICreateBuffer(RHIBufferCreateInfo(size, 0, EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::STORAGE_BUFFER));

        auto* cmd_list   = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        auto* copy_queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
        cmd_list->BeginRecording();

        RHICopyBufferInfo copy_buffer_info({RHIBufferRegion{0, 0, size}});
        cmd_list->CopyBuffer(copy_buffer_info, staging_buffer, buffer);

        cmd_list->EndRecording();

        RHIFenceRef fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::BINARY});

        RHISubmitInfo submit_info;

        submit_info.Signal(fence, 1);

        copy_queue->SubmitCommands(1, cmd_list, &submit_info);

        copy_queue->WaitForQueueComplete();
        return buffer;
    }

    std::pair<RHIBufferRef, RHIBufferRef> GpuSceneBufferBuilder::Impl::Build() {
        auto* cmd_list   = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        auto* copy_queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
        cmd_list->BeginRecording();

        uint32_t vertex_buffer_size = m_vertex_data->size() * sizeof(float);
        uint32_t index_buffer_size  = m_index_data->size() * sizeof(uint32_t);

        RHIBufferCreateInfo vertex_buffer_create_info(vertex_buffer_size, sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
        RHIBufferRef        vertex_buffer = g_rhi->RHICreateBuffer(vertex_buffer_create_info);
        RHIBufferCreateInfo staging_vertex_buffer_create_info(vertex_buffer_size, sizeof(float), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
        RHIBufferRef        staging_vertex_buffer = g_rhi->RHICreateBuffer(staging_vertex_buffer_create_info);

        auto* staging_vertex_buffer_mapped_ptr = static_cast<float*>(g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, vertex_buffer_size));
        memcpy(staging_vertex_buffer_mapped_ptr, m_vertex_data->data(), vertex_buffer_size);
        g_rhi->RHIUnmapBuffer(staging_vertex_buffer);

        Array<RHIBufferRegion> vertex_buffer_region_array({RHIBufferRegion{.src_offset = 0, .dst_offset = 0, .size = vertex_buffer_size}});
        RHICopyBufferInfo      vertex_copy_buffer_info{};
        vertex_copy_buffer_info.regions = vertex_buffer_region_array;
        cmd_list->CopyBuffer(vertex_copy_buffer_info, staging_vertex_buffer, vertex_buffer);

        RHIBufferCreateInfo index_buffer_create_info(index_buffer_size, sizeof(uint32_t), EBufferUsageFlags::INDEX_BUFFER);
        RHIBufferRef        index_buffer = g_rhi->RHICreateBuffer(index_buffer_create_info);
        RHIBufferCreateInfo staging_index_buffer_create_info(index_buffer_size, sizeof(uint32_t), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);
        RHIBufferRef        staging_index_buffer = g_rhi->RHICreateBuffer(staging_index_buffer_create_info);

        auto* staging_index_buffer_mapped_ptr = static_cast<uint32_t*>(g_rhi->RHIMapBuffer(staging_index_buffer, 0, index_buffer_size));
        memcpy(staging_index_buffer_mapped_ptr, m_index_data->data(), index_buffer_size);
        g_rhi->RHIUnmapBuffer(staging_index_buffer);

        Array<RHIBufferRegion> index_buffer_region_array({RHIBufferRegion{.src_offset = 0, .dst_offset = 0, .size = index_buffer_size}});
        RHICopyBufferInfo      index_copy_buffer_info{};
        index_copy_buffer_info.regions = index_buffer_region_array;
        cmd_list->CopyBuffer(index_copy_buffer_info, staging_index_buffer, index_buffer);

        cmd_list->EndRecording();

        RHIFenceRef   fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::BINARY});
        RHISubmitInfo submit_info;
        submit_info.Signal(fence, 1);
        copy_queue->SubmitCommands(1, cmd_list, &submit_info);
        copy_queue->WaitForQueueComplete();

        return {vertex_buffer, index_buffer};
    }

}// namespace Moer