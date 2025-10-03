#ifndef MOER_UI_BASE_H
#define MOER_UI_BASE_H

namespace Moer {
    class WindowContext;
    struct UICreateInfo {

        void* window_handle;
    };
    class UIBase {
    public:
        virtual void Init(const UICreateInfo& info) = 0;
        virtual void Tick()                         = 0;

        virtual ~UIBase(){};
    };
}// namespace Moer

#endif