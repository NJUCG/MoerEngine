#ifndef MOREENGINE_ENGINE_H
#define MOREENGINE_ENGINE_H

#include <filesystem>

namespace Moer {
    struct EngineInitInfo {

        std::filesystem::path config_path;
    };

    class Engine final {
    public:
        void Init(const EngineInitInfo& _init_info);

        void PostInit();

        void Run();

        void Quit();

    private:
        void InitCore();
        void ShutDownCore();
        void InitRenderSystem();
        void Tick();
    };
}// namespace Moer

#endif