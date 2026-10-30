#include "Engine.h"
#include "misc/Types.h"
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace Moer;
using namespace Moer::Render;
namespace Moer::Render::Testing {
RENDER_API void RunCudaInteropReadbacks();
}

int main(int argc, const char** argv) {
    Engine engine;
    try {
        engine.Init(argc, argv, false);
        Moer::Render::Testing::RunCudaInteropReadbacks();
        bool raster = false;
        for (int i = 1; i < argc; ++i)
            raster |= std::string_view(argv[i]) == "--raster";
        if (raster) {
            auto config                              = engine.GetEditorConfig();
            config->selected_render_method           = ERenderMethod::Raster;
            config->raster_config.ai_is_cuda_enabled = true;
            config->raster_config.ao_mode            = EAoMode::RTAO;
            config->raster_config.ao_half_resolution = false;
            config->raster_config.probe_gi_enabled   = false;
            uint32 ready_frames                      = 0;
            engine.Run(EngineHooks{.on_tick_test = [&](Scene& scene) {
                if (!scene.IsReady())
                    return;
                ++ready_frames;
                if (ready_frames == 13)
                    config->raster_config.ao_half_resolution = true;
                if (ready_frames == 25)
                    config->raster_config.ai_is_cuda_enabled = false;
                if (ready_frames == 29)
                    config->raster_config.ai_is_cuda_enabled = true;
                if (ready_frames == 41)
                    engine.RequestExit();
            }});
            if (ready_frames != 41)
                throw std::runtime_error("CUDA Raster validation exited before completing all phases");
            std::cout << "[TESTCASE][PASS] CudaRasterLifecycle full=12 half=12 disabled=4 reenabled=12\n";
        }
        engine.ShutDown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[TESTCASE][FAIL] " << error.what() << '\n';
        engine.ShutDown();
        return 1;
    }
}
