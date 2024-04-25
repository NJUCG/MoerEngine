#ifndef MOER_DEFERRED_BASE_PASS_H
#define MOER_DEFERRED_BASE_PASS_H
#include "math/Base.h"
#include "misc/STL.h"
#include "RenderResourceDeferred.h"
namespace Moer {
    class BasePass {
    public:
        BasePass();
        ~BasePass();
        struct Impl;
        void InitResources(RenderContext& _resources);
        void UpdateSceneData(RenderContext& _resources);
        void Draw(RenderContext& _input);
        void OnResizeViewport(Moer::Vector2i _extent);

    private:
        UniquePtr<Impl> impl;
    };
}// namespace Moer

#endif