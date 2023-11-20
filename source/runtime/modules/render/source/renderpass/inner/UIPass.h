#ifndef MOER_ENGINE_UI_PASS_H
#define MOER_ENGINE_UI_PASS_H
#include "renderpass/RenderPass.h"
namespace Moer {
    class UIPass : public RenderPass {
        //inherit from RenderPass
    public:
        UIPass()          = default;
        virtual ~UIPass() = default;
        virtual void BeforeRenderLoop() override;
        virtual void Execute() override;
        virtual void AfterRenderLoop() override;
    };
}// namespace Moer
#endif//MOER_ENGINE_UI_PASS_H
