#ifndef MOERENGINE_RENDER_SYSTEM_H
#define MOERENGINE_RENDER_SYSTEM_H
namespace Moer {
    class RenderSystem {
    public:
        static void Init();
        static void Tick();
        static void ShutDown();

        ~RenderSystem();

    private:
        static void InitShaderLibrary();
        RenderSystem() = default;
    };
};// namespace Moer
#endif