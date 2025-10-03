#ifndef MOER_EDITOR_H
#define MOER_EDITOR_H

namespace Moer {
class Engine;
class UIBase;
class Editor {
public:
    Editor();
    virtual ~Editor();
    void Init(Engine* _engine_runtime);
    void Run();
    void ShutDown();

private:
    Engine* engine_runtime{nullptr};
    UIBase* editor_ui{nullptr};
};
} // namespace Moer

#endif