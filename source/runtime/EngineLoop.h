#include <functional>
#include <vector>
#include <cstdint>

namespace Moer {
    class EngineLoop {
    public:
        static EngineLoop& GetInstance() {
            static EngineLoop instance;
            return instance;
        }
        void Init();

        void BeforeLoop();
        void Run();
        bool ShouldEndLoop();
        void AfterLoop();

        void RegisterOnDrawUI(std::function<void()> _func);

    private:
        void RenderUI();

        void AquireRenderThreadResult();

        void ProcessInputEvents();

        std::vector<std::function<void()>> on_draw_ui_funcs;

        struct EngineLoopData* data;
    };
}// namespace Moer
