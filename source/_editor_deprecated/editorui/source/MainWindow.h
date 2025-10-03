#ifndef MOER_ENGINE_MAIN_WINDOW_H
#define MOER_ENGINE_MAIN_WINDOW_H
class MainWindow {

public:
    // virtual void Init(const UICreateInfo& info);
    void Show();

    virtual ~MainWindow() {};

    bool* ShowWindow() {
        return &b_show;
    };

private:
    bool b_show{true};
};
#endif