#ifndef MOER_ENGINE_UPLOAD_PASS_H
#define MOER_ENGINE_UPLOAD_PASS_H
#include "renderpass/RenderPass.h"
namespace Moer {
    class UploadPass : public RenderPass {
        //override methods
    public:
        UploadPass()          = default;
        virtual ~UploadPass() = default;
        virtual void BeforeRenderLoop() override;
        virtual void Execute() override;
        virtual void AfterRenderLoop() override;

    private:
        //test
        void TestDrawTriangle();

        struct PassData* data;
    };
}// namespace Moer

#endif//MOER_ENGINE_UPLOAD_PASS_H