#include "Editor.h"
#include "EditorUI.h"
#include "Engine.h"
#include "ui/UIBase.h"
#include "window/WindowContext.h"
#include <assert.h>

namespace Moer {
    Editor::Editor() {
    }
    Editor::~Editor() {
    }
    void Editor::Init(Engine* engine) {
        editor_ui = new EditorUI;
        UICreateInfo info{};

        editor_ui->Init(info);
        assert(engine != nullptr && "Fail to create Engine Runtime.");
        engine_runtime = engine;
        engine_runtime->RegisterOnDrawUI(std::bind(&EditorUI::Tick, (EditorUI*)editor_ui));
    }

    void Editor::ShutDown() {
        engine_runtime->Quit();
        delete editor_ui;
        delete engine_runtime;
    }

    void Editor::Run() {
        engine_runtime->Run();
    }
}// namespace Moer