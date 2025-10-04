#pragma once

#include "misc/STL.h"

namespace Moer {

class EditorUI;
class Engine;

class Editor {
public:
    Editor();
    virtual ~Editor();

    void Init(int argc, const char** argv);
    void Run();
    void ShutDown();

private:
    UniquePtr<Engine>   m_engine;
    UniquePtr<EditorUI> m_editor_ui;
};

} // namespace Moer