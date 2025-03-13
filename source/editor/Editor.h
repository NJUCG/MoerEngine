#pragma once

#include "misc/STL.h"

namespace Moer {

class EditorUI;

/**
 * TODO: 将Runtime和Editor分离
 */
class Editor {
public:
    Editor();
    virtual ~Editor();

    void Init(int argc, const char** argv);
    void Run();
    void ShutDown();

private:
    SharedPtr<EditorUI> m_editor_ui;
};

} // namespace Moer