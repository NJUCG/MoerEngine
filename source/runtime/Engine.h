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

        bool IsRequestQuiting() const { return b_request_quiting; }

    private:
        void InitCore();
        void ShutDownCore();
        void InitRenderSystem();
        void Tick();

    private:
        bool b_request_quiting;
    };
}// namespace Moer

#endif