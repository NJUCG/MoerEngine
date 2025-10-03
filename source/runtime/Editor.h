#pragma once

#include "misc/STL.h"

namespace Moer {

class EditorUI;
class EditorAssets;

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
    void Init3rdParty();
    void ShutDown3rdParty();

private:
    SharedPtr<EditorUI>     m_editor_ui;
    UniquePtr<EditorAssets> m_editor_assets;
};

} // namespace Moer