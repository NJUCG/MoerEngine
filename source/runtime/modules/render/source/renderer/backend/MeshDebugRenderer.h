#ifndef MOER_MESH_DEBUG_RENDERER_H
#define MOER_MESH_DEBUG_RENDERER_H

#include "renderer/BackendRenderer.h"
namespace Moer{

    class MeshDebugRenderer : public BackendRenderer{
    public:
        MeshDebugRenderer() = default;
        virtual ~MeshDebugRenderer() = default;
        virtual void Init(const BackendRendererInitInfo& _init_info) override;
        virtual void ShutDown() override;
        virtual void DrawFrame() override;
        virtual void Present() override;
        virtual void SetOriginResolution(uint32_t _width, uint32_t _height) override;
        virtual void SetPresentResolution(uint32_t _width, uint32_t _height) override;

        virtual void* GetRendererOutput() override;

    private:
        class Impl;
        Impl* impl;
    };
};
#endif