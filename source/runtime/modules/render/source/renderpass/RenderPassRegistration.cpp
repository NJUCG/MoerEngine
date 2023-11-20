#include "renderpass/RenderPassRegistration.h"
namespace Moer {
    RenderPassRegistration& RenderPassRegistration::GetInstance() {
        static RenderPassRegistration instance;
        return instance;
    }
    void RenderPassRegistration::RegisterRenderPass(RenderPassRef _renderPass) {
        passes.push_back(_renderPass);
    }

}// namespace Moer
