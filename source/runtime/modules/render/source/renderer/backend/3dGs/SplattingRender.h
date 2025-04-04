// #pragma once
// #include "renderer/BackendRenderer.h"

// namespace Moer {

//     /*3d gaussian splatting renderer
//     Pipeline:
//     1. Preprocess: preprocess the vertex buffer and generate the vertex attribute buffer.
//     2. Prefix-sum: calculate instances num
//     3. Preprocess-sort: sort all instances
//     */
//     class SplattingRender : public BackendRenderer {
//     public:
//         void      Init(const BackendRendererInitInfo& _init_info) override;
//         void      DrawFrame() override;
//         void      ShutDown() override;
//         void      Present() override;
//         void      SetOriginResolution(uint32_t _width, uint32_t _height) override;
//         void      SetPresentResolution(uint32_t _width, uint32_t _height) override;
//         RHISRVRef GetRendererOutput() override;
//         ~SplattingRender();

//     protected:
//         class Impl;
//         Impl* impl{nullptr};
//     };
// }// namespace Moer
