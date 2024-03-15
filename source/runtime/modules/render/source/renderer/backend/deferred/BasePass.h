#ifndef MOER_DEFERRED_BASE_PASS_H
#define MOER_DEFERRED_BASE_PASS_H
#include "math/Base.h"
#include "misc/STL.h"
namespace Moer {
    class BasePass {
    public:
        BasePass();
        ~BasePass();
        struct Impl;
        void InitResources(class RenderResourceDeferred& resources);
        void Draw(const struct PassInput& input);
        void OnResizeViewport(Moer::Vector2i extent);

    private:
        UniquePtr<Impl> impl;
    };
}// namespace Moer

#endif