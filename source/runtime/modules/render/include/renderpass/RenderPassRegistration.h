#include "misc/STL.h"

#include "RenderPass.h"

namespace Moer {

    class RenderPassRegistration {
    public:
        static RenderPassRegistration& GetInstance();

        void RegisterRenderPass(RenderPassRef _renderPass);

    private:
        friend class RenderLoop;
        RenderPassRegistration() = default;

        Moer::Array<RenderPassRef> passes;
    };
}// namespace Moer
