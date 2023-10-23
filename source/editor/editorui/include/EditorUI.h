#ifndef MOER_EDITOR_UI_H
#define MOER_EDITOR_UI_H
#include "ui/UIBase.h"
namespace Moer {
    class WindowContext;
    class EditorUI : public UIBase {
    public:
        virtual void Init(const UICreateInfo& info) override;
        virtual ~EditorUI();
    };
}// namespace Moer
#endif