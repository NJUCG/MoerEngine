#pragma once

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/ECS.h"
#include <cstddef>

namespace Moer {
    enum E_VERTEX_ATTRIBUTE {
        E_POSITION  = 1 << 1,
        E_NORMAL    = 1 << 2,
        E_TANGENT   = 1 << 3,
        E_BITANGENT = 1 << 4,
        E_UV0       = 1 << 5,
    };
    using VertexAttributeFlags = uint8_t;

    class GpuPrimitiveBuilder {
    public:
        RENDER_API static void InitBuild();
        RENDER_API static void EndBuild();

        RENDER_API GpuPrimitiveBuilder& Vertex(const Moer::Array<float>* data);
        RENDER_API GpuPrimitiveBuilder& Index(const Moer::Array<uint32_t>* data);
        RENDER_API GpuPrimitiveBuilder& Attribute(VertexAttributeFlags attribute);

        RENDER_API RHIRenderPrimitiveRef Build();

        RENDER_API GpuPrimitiveBuilder();
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
        RHITextureRef   Build() noexcept;
        ~TextureBuilder() noexcept;
        // static void InitBuild() noexcept;
        // static void EndBuild() noexcept;
    protected:
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
        std::pair<RHIBufferRef, RHIBufferRef> Build();
        static RHIBufferRef                   CopyFrom(EBufferUsageFlags usages, const void* data, uint32_t size, uint32_t _stride = sizeof(std::byte));
        template<typename T>
            requires std::is_trivially_copyable_v<T>
        static RHIBufferRef CopyFrom(EBufferUsageFlags _usages, const std::span<T> _data) {
            return CopyFrom(_usages, _data->data(), _data->size() * sizeof(T), sizeof(T));
        }
        RHIBufferRef CreateBufferWithData(EBufferUsageFlags usages, const void* data, uint32_t size);

    protected:
        class Impl;
        Impl* m_impl{nullptr};
    };
}// namespace Moer