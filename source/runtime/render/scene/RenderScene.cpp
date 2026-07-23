#include "RenderScene.h"

#include "RenderThread.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutor.h"

namespace Moer::Render {

RenderScene::RenderScene(BindlessArrayRef bindless_array) : m_bindless_array(std::move(bindless_array)) {}

RenderScene::~RenderScene() = default;

GpuScene::PendingCommandList RenderScene::ApplyUpdate(GpuSceneUpdate&& update) {
    assert(IsCurrentlyGameThread() || IsCurrentlyRenderThread());
    assert(update.HasWork());

    LOG_INFO(
        "[Threading] RenderScene update executes on {} Thread; full_rebuild={}, lights={}, "
        "materials={}, meshes={}, raytracing_update={}.",
        IsCurrentlyRenderThread() ? "Render" : "Game",
        update.full_rebuild ? 1 : 0,
        update.update_lights ? 1 : 0,
        update.update_materials ? 1 : 0,
        update.update_meshes ? 1 : 0,
        static_cast<uint>(update.raytracing_update)
    );

    if (update.full_rebuild) {
        // Device idle only covers work already submitted to Vulkan. Drain the
        // upper submission owner first so no queued translation can retain the
        // scene resources that are about to be replaced.
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        RenderDevice::Get().WaitIdle();
        m_gpu_scene = MakeUnique<GpuScene>(m_bindless_array);
    }

    assert(m_gpu_scene && "The first RenderScene update must be a full rebuild.");
    m_gpu_scene->ApplyUpdate(std::move(update));
    return m_gpu_scene->PopPendingCommandList();
}

const GpuScene::Res& RenderScene::gpu_scene_res() const {
    assert(m_gpu_scene && "RenderScene is not ready.");
    return m_gpu_scene->res();
}

void RenderScene::RestoreDrawCommands(CommandList& cmd_list) {
    assert(m_gpu_scene && "RenderScene is not ready.");
    m_gpu_scene->RestoreDrawCommands(cmd_list);
}

} // namespace Moer::Render
