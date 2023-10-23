#ifndef MOER_UI_BASE_H
#define MOER_UI_BASE_H

namespace Moer {
    class WindowContext;
    struct UICreateInfo {

        WindowContext* window_context;
    };
    class UIBase {
    public:
        virtual void Init(const UICreateInfo& info) = 0;
        virtual ~UIBase(){};
    };
}// namespace Moer

#endif