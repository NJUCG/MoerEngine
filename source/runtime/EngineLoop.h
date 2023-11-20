#include <functional>
#include <vector>
#include <cstdint>

#include "misc/Singleton.h"

namespace Moer {

    class EngineLoop : public Singleton<EngineLoop> {
    public:
        void Init();
        void Run();
        bool ShouldEndLoop();
        void Quit();

        void RegisterOnDrawUI(std::function<void()> _func);

    private:
        void RenderUI();

        void AquireRenderThreadResult();

        void ProcessInputEvents();

        std::vector<std::function<void()>> on_draw_ui_funcs;

        struct EngineLoopData* data;
    };
}// namespace Moer
