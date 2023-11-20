#include "misc/CountableRef.h"

namespace Moer {
    using RenderPassRef = CountableRef<class RenderPass>;

    struct RenderPassResizeInfo {
        uint32_t width;
        uint32_t height;
    };

    class RenderPass : public Countable {
    public:
        RenderPass(){};
        virtual ~RenderPass(){};

        // Resource creation before renderloop
        virtual void BeforeRenderLoop(){};

        // Renderloop logic
        virtual void Execute() = 0;

        // Resize operations
        virtual void OnViewportResize(const RenderPassResizeInfo& _info){};

        // Resource destruction after quitting loop
        virtual void AfterRenderLoop(){};

        virtual void Destroy() override {
            delete this;
        }
    };
}// namespace Moer
