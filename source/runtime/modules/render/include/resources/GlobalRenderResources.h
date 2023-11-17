#ifndef MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H
#define MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H
#include <vector>
class RHITexture;
class RHIGraphicsCommandList;
class RHICommandQueue;
namespace Moer {
    struct GlobalRenderFrameData {
        RHITexture* upload_texture;

        RHIGraphicsCommandList* command_list;

        //MARK... todo compute command list and queue
    };
    struct GlobalRenderData {
        std::vector<GlobalRenderFrameData> frame_datas;

        RHICommandQueue* graphics_command_queue;

        RHICommandQueue* compute_command_queue;

        RHICommandQueue* transfer_command_queue;
    };

    class GlobalRenderResources {
    public:
        friend class RenderSystem;
        static GlobalRenderData& GetGlobalRenderData();

    private:
        static void Init();

        static void ShutDown();

        GlobalRenderData global_render_data;
    };
}// namespace Moer

#endif//MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H