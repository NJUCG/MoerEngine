#pragma once

#include "rhi/RHIResource.h"

namespace Moer {
    enum E_VERTEX_ATTRIBUTE  {
        E_POSITION = 1 << 1,
        E_NORMAL   = 1 << 2,
        E_TANGENT  = 1 << 3,
        E_BITANGENT= 1 << 4,
        E_UV0      = 1 << 5,
    };
    using VertexAttributeFlags = uint8_t;


    class  GpuPrimitiveBuilder {
    public:

        RENDER_API static void InitBuild();
        RENDER_API static void EndBuild();

        RENDER_API GpuPrimitiveBuilder  & Vertex(const std::vector<float> * data );
        RENDER_API GpuPrimitiveBuilder  & Index(const std::vector<uint32_t> * data);
        RENDER_API GpuPrimitiveBuilder  & Attribute(VertexAttributeFlags attribute);

        RENDER_API RHIRenderPrimitiveRef Build();

        RENDER_API GpuPrimitiveBuilder();
        RENDER_API ~GpuPrimitiveBuilder();
        bool Validate() const;
    protected:
        class Impl;
        Impl * m_impl;
    };


    struct GpuCamera {
        Matrix4x4f view,perspective;
        //todo add  other required attribute 
    };
}