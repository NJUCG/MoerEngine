#pragma once

#include "RenderAPI.h"

namespace Moer {

// 前向声明
class Scene;

/**
 * SceneGlobalEntry
 *
 * 单例模式，用于全局绑定和访问 Scene 对象
 * 便于在全局范围内访问当前场景
 *
 * 使用例：EditorUI中，需要scene对象检测场景是否加载完毕
 */
class RENDER_API SceneGlobalEntry {
public:
    /**
     * 获取单例实例
     */
    static SceneGlobalEntry& Get();

    /**
     * 绑定 Scene 对象
     * @param scene 要绑定的 Scene 指针，可以为 nullptr 用于解绑
     */
    void BindScene(Scene* scene);

    /**
     * 获取当前绑定的 Scene 对象
     * @return Scene 指针，如果未绑定则返回 nullptr
     */
    Scene* GetScene();

    /**
     * 获取当前绑定的 Scene 对象（const 版本）
     * @return const Scene 指针，如果未绑定则返回 nullptr
     */
    const Scene* GetScene() const;

private:
    SceneGlobalEntry()  = default;
    ~SceneGlobalEntry() = default;

    SceneGlobalEntry(const SceneGlobalEntry&)            = delete;
    SceneGlobalEntry& operator=(const SceneGlobalEntry&) = delete;

    Scene* m_scene = nullptr;
};

} // namespace Moer
