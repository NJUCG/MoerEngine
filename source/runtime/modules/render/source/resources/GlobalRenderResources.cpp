#include "resources/GlobalRenderResources.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHICommandList.h"
#include "rhi/RHICommandQueue.h"

namespace Moer {
    GlobalRenderResources& GetInstance() {
        static GlobalRenderResources instance;
        return instance;
    }
    GlobalRenderData& GlobalRenderResources::GetGlobalRenderData() {
        // Implementation of GetGlobalRenderData method
        // ...
        return GetInstance().global_render_data;
    }
    void GlobalRenderResources::Init() {
        // Implementation of Init method
        // ...
        auto& instance = GetInstance();

        uint32_t frame_count = ConfigManager::GetInstance().GetInitConfig().max_frame_in_flight;

        instance.global_render_data.frame_datas.resize(frame_count);

        auto* main_viewport = g_rhi->RHIGetMainViewport();

        auto viewport_extent = main_viewport->GetViewportExtent();

        RHITextureCreateInfo info = RHITextureCreateInfo::Create2D("upload texture")
                                        .SetArraySize(1)
                                        .SetNumMips(1)
                                        .SetDepth(1)
                                        .SetExtent({(int32_t)viewport_extent.width, (int32_t)viewport_extent.height})
                                        .SetFormat(PF_R8G8B8A8_SRGB)
                                        .SetUsageFlags(ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC)
                                        .SetInitialLayout(TEXTURE_LAYOUT_UNDEFINED);

        for (uint32_t i = 0; i < frame_count; ++i) {

            auto&         frame_data   = instance.global_render_data.frame_datas[i];
            RHITextureRef temp_texture = g_rhi->RHICreateTexture(info);
            temp_texture->AddRef();

            frame_data.upload_texture = temp_texture;
            frame_data.command_list   = g_rhi->CreateGraphicsCommandList();
        }
        instance.global_render_data.graphics_command_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
        instance.global_render_data.compute_command_queue  = g_rhi->CreateCommandQueue(ECommandQueueType::COMPUTE);
        instance.global_render_data.transfer_command_queue = g_rhi->CreateCommandQueue(ECommandQueueType::COPY);
    }

    void GlobalRenderResources::ShutDown() {
        // Implementation of ShutDown method
        // ...
        auto& instance    = GetInstance();
        auto  frame_count = instance.global_render_data.frame_datas.size();

        //delete upload buffers
        for (uint32_t i = 0; i < frame_count; i++) {
            instance.global_render_data.frame_datas[i]
                .upload_texture->DeRef();
        }
        delete instance.global_render_data.graphics_command_queue;
        delete instance.global_render_data.compute_command_queue;
        delete instance.global_render_data.transfer_command_queue;
    }
}// namespace Moer