// #pragma once
//
// #include "renderer/BackendRenderer.h"
// namespace Moer {
//
//     class RenderGraph;
//     class DeferedRenderingRenderGraphRender;
//
//     class RenderGraphRender : public BackendRenderer {
//     public:
//         RenderGraphRender()          = default;
//         virtual ~RenderGraphRender() = default;
//         virtual void Init(const BackendRendererInitInfo& _init_info) override;
//         virtual void ShutDown() override;
//         virtual void DrawFrame() override;
//         virtual void Present() override;
//         virtual void SetOriginResolution(uint32_t _width, uint32_t _height) override;
//         virtual void SetPresentResolution(uint32_t _width, uint32_t _height) override;
//
//         virtual void* GetRendererOutput() override;
//
//     protected:
//         class Impl;
//         Impl* impl;
//     };
//
//     class DeferedRenderingRenderGraphRender : public RenderGraphRender {
//     public:
//         DeferedRenderingRenderGraphRender();
//         ~DeferedRenderingRenderGraphRender();
//         void Init(const BackendRendererInitInfo& _init_info) override;
//
//     private:
//         class Impl;
//         //  Impl* m_impl;
//     };
// }// namespace Moer
