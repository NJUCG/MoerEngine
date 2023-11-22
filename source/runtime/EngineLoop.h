#include <functional>
#include <vector>
#include <cstdint>
#include "RenderThread.h"

#include "misc/Singleton.h"

namespace Moer {
    class FrameEndSync {
        /** Pair of fences. */
        RenderThreadFence fence[2];
        /** Current index into events array. */
        int32_t event_index;
        /** cleanup delegate for engine pre-exit */
        // FDelegateHandle CleanupDelegate;

    public:
        FrameEndSync();
        ~FrameEndSync();

        /**
	 * Syncs the game thread with the render thread. Depending on passed in bool this will be a total
	 * sync or a one frame lag.
	 */
        RENDER_API void Sync(bool b_allow_one_frame_lag);

    private:
        void Cleanup();
    };
    class EngineLoop : public Singleton<EngineLoop> {
    public:
        void Init();
        void Run();
        bool ShouldEndLoop();
        void Quit();

        void RegisterOnDrawUI(std::function<void()> _func);

    private:
        void ProcessInputEvents();

        std::vector<std::function<void()>> on_draw_ui_funcs;

        struct EngineLoopData* data;

        FrameEndSync frame_end_sync;
    };
}// namespace Moer
