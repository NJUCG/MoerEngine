#pragma once

#include "GpuScene.h"
#include "RenderAPI.h"
#include "gaussian/GaussianGpuScene.h"

namespace Moer::Render {

class RENDER_API RenderScene {
public:
    explicit RenderScene(BindlessArrayRef bindless_array);
    ~RenderScene();

    RenderScene(const RenderScene&)            = delete;
    RenderScene& operator=(const RenderScene&) = delete;

    GpuScene::PendingCommandList ApplyUpdate(
        GpuSceneUpdate&&                                  update,
        const GpuScene::PendingCommandListSetupCallback& setup_command_lists = {}
    );

    bool IsReady() const {
        return m_gpu_scene != nullptr;
    }

    const GpuScene::Res& gpu_scene_res() const;
    const GpuScene::Res& GetGpuSceneRes() const {
        return gpu_scene_res();
    }

    void RestoreDrawCommands(CommandList& cmd_list);
    const GaussianGpuScene* GetGaussianGpuScene() const { return m_gaussian_scene.get(); }

private:
    BindlessArrayRef    m_bindless_array;
    UniquePtr<GpuScene> m_gpu_scene;
    UniquePtr<GaussianGpuScene> m_gaussian_scene;
};

} // namespace Moer::Render
