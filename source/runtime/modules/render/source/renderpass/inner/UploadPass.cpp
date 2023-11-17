
#include "UploadPass.h"
#include "resources/GlobalRenderResources.h"
#include "rhi/RHIResource.h"
namespace Moer {
    struct PassData {
        RHITextureRef upload_textures;
    };
    void UploadPass::BeforeRenderLoop() {
        // Implementation of execute method
        // ...
    }

    void UploadPass::Execute() {
        // Implementation of execute method
        // ...
        GlobalRenderData& global_render_data = GlobalRenderResources::GetGlobalRenderData();

        // auto& frame_data = global_render_data.frame_datas[0];
    }

    void UploadPass::AfterRenderLoop() {
        // Implementation of execute method
        // ...
    }
}// namespace Moer
