#pragma once

// 管理 Engine 外层的编辑器生命周期，并将编辑器 UI 回调接入运行时循环。

#include "misc/STL.h"

#include <functional>

namespace Moer {

class EditorUI;
class Engine;
class Scene;

class Editor {
public:
    struct ExtraHooks {
        std::function<void(Scene&)> on_tick_test;
    };

    Editor();
    virtual ~Editor();

    void Init(int argc, const char** argv);
    void Run();
    void Run(const ExtraHooks& extra_hooks);
    void ShutDown();

    Engine&       GetEngine();
    const Engine& GetEngine() const;

private:
    UniquePtr<Engine>   m_engine;
    UniquePtr<EditorUI> m_editor_ui;
};

} // namespace Moer
