#ifndef MOER_EDITOR_UI_H
#define MOER_EDITOR_UI_H
#include "ui/UIBase.h"
namespace Moer {
class WindowContext;
class EditorUI : public UIBase {
public:
    virtual void Init(const UICreateInfo& info) override;
    virtual void Tick() override;
    virtual ~EditorUI();

private:
    void ShowEditorMenu(bool* _b_show);
    void ShowMainWindow(bool* b_show);
    void ShowInspectorWindow(bool* b_show);

private:
    bool m_b_show_editor_menu{true};
    bool m_b_show_main_window{true};
    bool m_b_show_inspector_window{true};
};
} // namespace Moer
#endif