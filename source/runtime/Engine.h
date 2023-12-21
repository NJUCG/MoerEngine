#ifndef MOREENGINE_ENGINE_H
#define MOREENGINE_ENGINE_H

#include <filesystem>
#include <functional>

namespace Moer {
    struct EngineInitInfo {

        std::filesystem::path workspace_path;
    };

    class Engine final {
        friend class EngineLoop;

    public:
        void Init(const EngineInitInfo& _init_info);

        void PostInit();

        void Run();

        void Quit();

        bool IsRequestQuiting() const { return b_request_quiting; }

        void RegisterOnDrawUI(std::function<void()> _func);

    private:
        void InitCore(const std::filesystem::path&);
        void ShutDownCore();

        void InitRenderSystem();
        void PostInitRenderSystem();
        void ShutDownRenderSystem();

        void InitWindow();
        void ShutDownWindow();

    private:
        bool b_request_quiting = false;
    };
}// namespace Moer

#endif