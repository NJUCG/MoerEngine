
#include "rhi/RHIResource.h"
class RHIGraphicsCommandList;
class RHICommandQueue;
class RHIFence;
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

        void ProcessInputEvents();

        RHIGraphicsCommandList* ui_command_list = nullptr;
        RHICommandQueue*        command_queue   = nullptr;
        RHIFence*               present_fence   = nullptr;

        uint64_t frame_index = 0;

        std::vector<std::function<void()>> on_draw_ui_funcs;
    };
}// namespace Moer
