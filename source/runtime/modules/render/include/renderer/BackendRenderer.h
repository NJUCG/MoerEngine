#ifndef MOER_BACKEND_RENDERER_H
#define MOER_BACKEND_RENDERER_H
#include "PixelFormat.h"
#include "rhi/RHIResource.h"
#include <cstdint>
namespace Moer {
    struct BackendRendererInitInfo {
        uint32_t     width;
        uint32_t     height;
        EPixelFormat format = PF_R8G8B8A8_SRGB;
    };
#include "RenderAPI.h"
    /**
 * @brief BackendRenderer is responsible for rendering on render thread.
 * there may be multiple BackendRenderer instances, each contains a
 * VirtualViewport, which is a virtual swap chain for presenting on UI.
 * 
 */
    class BackendRenderer {
    public:
        BackendRenderer() = default;

        virtual ~BackendRenderer() = default;

        RENDER_API virtual void Init(const BackendRendererInitInfo& _init_info) = 0;

        RENDER_API virtual void ShutDown() = 0;

        RENDER_API virtual void DrawFrame() = 0;

        RENDER_API virtual void Present() = 0;

        RENDER_API virtual void SetOriginResolution(uint32_t _width, uint32_t _height) = 0;

        RENDER_API virtual void SetPresentResolution(uint32_t _width, uint32_t _height) = 0;

        RENDER_API virtual RHISRVRef GetRendererOutput() = 0;

        RENDER_API virtual void UpdateGUI() {}
    };
}// namespace Moer
#endif