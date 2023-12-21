#include "Launcher.h"
#include <filesystem>
#include "Editor.h"
#include "Engine.h"
#include "Core.h"
//compile set

namespace Moer {
    Launcher& Launcher::GetInstance() {
        static Launcher launcher;
        return launcher;
    }

    Launcher::Launcher() {}

    void Launcher::Init(const std::filesystem::path& _work_space_path) {

        EngineInitInfo info{_work_space_path};

        Engine* engine = MoerNew(Engine)();
        engine->Init(info);
        editor = MoerNew(Editor);

        editor->Init(engine);

        engine->PostInit();
    }

    void Launcher::Run() {

        editor->Run();

        editor->ShutDown();
    }

    void Launcher::Quit() {
        MoerDelete(editor);
        editor = nullptr;
    }
}// namespace Moer
