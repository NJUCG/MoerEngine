#ifndef MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H
#define MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H
#include "rhi/RHIResource.h"

class RHITexture;
class RHIGraphicsCommandList;
class RHICommandQueue;
class RHISRV;
class RHISampler;
class RHISRV;
class RHITexture;

struct RHISamplerCreateInfo;

namespace Moer {
    struct GlobalRenderFrameData {
        // RHITexture* upload_texture;

        // RHIShaderResourceView* upload_texture_srv;

        // RHIGraphicsCommandList* command_list;

        //MARK... todo compute command list and queue
    };
    struct GlobalRenderData {
        // Moer::Array<GlobalRenderFrameData> frame_datas;

        // RHICommandQueue* graphics_command_queue;

        // RHICommandQueue* compute_command_queue;

        // RHICommandQueue* transfer_command_queue;
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

    class SamplerCache {
    public:
        static SamplerCache& Get();
        RHISampler*          GetSampler(const RHISamplerCreateInfo& params);
        RHISRV*              GetTextureView(RHITexture* texture);
        ~SamplerCache();

    protected:
        SamplerCache();
        inline static SamplerCache* m_instance{nullptr};
        class Impl;
        Impl* m_impl;
    };

    class RenderGraphResourceCache {
    public:
        static RenderGraphResourceCache& Get();
        RHITextureRef                    GetTexture(const std::string& name, Extent2D size, EPixelFormat format, ETextureUsageFlags usage, uint32_t mipLevels = 1, uint32_t arrayLayers = 1);
        //   RHITextureRef GetBuffer()
        // RHIBufferRef GetBuffer(const std::string & name,RenderGraphBuffer::Descriptor);
        RHIUAVRef   GetUAV(RHITextureRef texture, uint32_t mip_num = 0, uint32_t array_min = 0, uint32_t array_num = 1);
        RHISRVRef   GetSRV(RHITextureRef texture, uint32_t mip_min = 0, uint32_t mip_num = 0, uint32_t array_min = 0, uint32_t array_num = 1);
        RHISampler* GetSampler(const RHISamplerCreateInfo& params);
        ~RenderGraphResourceCache();

    protected:
        RenderGraphResourceCache();
        class Impl;
        Impl* m_impl{nullptr};
    };

}// namespace Moer

#endif//MOER_ENGINE_GLOBAL_RENDER_RESOURCES_H