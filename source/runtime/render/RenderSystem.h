#ifndef MOERENGINE_RENDER_SYSTEM_H
#define MOERENGINE_RENDER_SYSTEM_H
#include "RenderAPI.h"
namespace Moer {
class RENDER_API RenderSystem {
public:
    static void Init();
    static void PostInit();
    static void Tick();
    static void ShutDown();

    ~RenderSystem();

private:
    static void InitShaderResources();
    static void FreeShaderResources();
    static void InitRHI();
    static void PostInitRHI();
    static void ShutDownRHI();
    RenderSystem() = default;
};
}; // namespace Moer
#endif