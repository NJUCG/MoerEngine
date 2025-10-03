#pragma once

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/ECS.h"
#include <cstddef>

namespace Moer {
    enum EVertexAttributeFlags {
        E_POSITION  = 1 << 1,
        E_NORMAL    = 1 << 2,
        E_TANGENT   = 1 << 3,
        E_BITANGENT = 1 << 4,
        E_UV0       = 1 << 5,
        E_UV1       = 1 << 6,
        E_COLOR     = 1 << 7,
        E_JOINTS    = 1 << 8,
        E_WEIGHTS   = 1 << 9
    };

    using VertexAttributeFlags           = uint8_t;
    static constexpr uint vtx_attrib_cnt = 6;

    class GpuPrimitiveBuilder {
    public:
        RENDER_API static void InitBuild();
        RENDER_API static void EndBuild();

        RENDER_API GpuPrimitiveBuilder& Vertex(const Moer::Array<float>* data);
        RENDER_API GpuPrimitiveBuilder& Index(const Moer::Array<uint32_t>* data);
        RENDER_API GpuPrimitiveBuilder& Attribute(VertexAttributeFlags attribute);
        RENDER_API                      GpuPrimitiveBuilder();
        RENDER_API ~GpuPrimitiveBuilder();
        bool Validate() const;

    protected:
        class Impl;
        Impl* m_impl;
    };

    struct GpuCamera {
        Matrix4x4f view, perspective;
        //todo add  other required attribute
    };

    class RENDER_API TextureBuilder {
    public:
        using Callback = std::function<void(void*)>;

        TextureBuilder& Width(uint32_t width) noexcept;
        TextureBuilder& Height(uint32_t height) noexcept;
        TextureBuilder& Depth(uint32_t depth) noexcept;
        TextureBuilder& Format(EPixelFormat format) noexcept;
        TextureBuilder& MipAndLayers(uint32_t mip_levels, uint32_t layer_levels, const uint32_t* offsets, const Extent3D* extents) noexcept;
        TextureBuilder& CallBack(Callback callback) noexcept;
        TextureBuilder& Data(void* data, uint32_t data_size) noexcept;
        TextureBuilder& Name(const std::string& name) noexcept;
        ~TextureBuilder() noexcept;
        static Moer::UnorderedMap<std::string, Render::TextureRef> BuildTexturesInBatch(Moer::Array<TextureBuilder>& builders) noexcept;
        // static void InitBuild() noexcept;
        // static void EndBuild() noexcept;
    protected:
        std::string  m_name;
        EPixelFormat m_format{EPixelFormat::PF_R8G8B8_UNORM};
        uint32_t     m_width{0}, m_height{0}, m_depth{0}, m_mip_levels{1}, m_layer_levels{1}, m_data_size{0};
        Callback     m_callback{nullptr};
        // uint32_t*    m_mip_offsets{nullptr};
        uint32_t* m_offsets{nullptr};
        Extent3D* m_mip_extents{nullptr};
        void*     m_data{nullptr};
    };

    class RENDER_API GpuSceneBufferBuilder {
    public:
        GpuSceneBufferBuilder& Vertex(const Moer::Array<float>* data);
        GpuSceneBufferBuilder& Index(const Moer::Array<uint32_t>* data);
        GpuSceneBufferBuilder();
        ~GpuSceneBufferBuilder();

    protected:
        class Impl;
        Impl* m_impl{nullptr};
    };
}// namespace Moer