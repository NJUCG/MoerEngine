#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include "scene/SceneLoadInfoAsync.h"
#include <filesystem>

namespace Moer::ecs {
class LogicalScene;
}

namespace Moer {

/**
 * Loader Interface
 * 
 * 一个纯static类，负责从文件加载场景数据到LogicalScene中
 * 提供了两类结构
 * - 同步加载接口 LoadSceneFromFile
 *   - 阻塞当前线程，直到场景加载完成
 *   - 返回bool，表示加载是否成功
 * - 异步加载接口 LoadSceneFromFileAsync
 *   - 不阻塞当前线程
 *   - 返回SceneLoadInfoAsync指针，外部可以通过该指针查询加载状态
 */
class RENDER_API LoaderInterface {

public:
    /**
     * 从文件加载场景数据到out_logical_scene中
     * 
     * @return bool 加载是否成功
     */
    static bool
    LoadSceneFromFile(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path);

    /**
     * 从文件异步加载场景数据到out_logical_scene中
     * 
     * 在目前的架构中，请不要调用这个异步函数。因为LoaderInterface只负责到LogicalScene
     * 如果你想加载整个场景（包括后续CpuScene和GpuScene的构建），请调用 Scene::LoadSceneFromFileAsync 接口
     * 
     * @return SharedPtr<SceneLoadInfoAsync> 场景加载状态信息
     */
    static SharedPtr<SceneLoadInfoAsync>
    LoadSceneFromFileAsync(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path);

private:
    static bool
    LoadSceneFromFileCommon(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path);
};

} // namespace Moer
