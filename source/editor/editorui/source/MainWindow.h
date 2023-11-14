#ifndef MOER_ENGINE_MAIN_WINDOW_H
#define MOER_ENGINE_MAIN_WINDOW_H
class MainWindow {

public:
    // virtual void Init(const UICreateInfo& info);
    virtual void Tick();

    virtual ~MainWindow(){};
};
#endif