
#include <vector>
#include "RenderPass.h"

namespace Moer {

    class RenderPassRegistration {
    public:
        static RenderPassRegistration& GetInstance();

        void RegisterRenderPass(RenderPassRef _renderPass);

    private:
        friend class RenderLoop;
        RenderPassRegistration() = default;

        std::vector<RenderPassRef> passes;
    };
}// namespace Moer
