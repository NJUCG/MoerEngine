#include "resources/GlobalRenderResources.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHICommand.h"

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

        // instance.global_render_data.frame_datas.resize(frame_count);

        // auto* main_viewport = g_rhi->RHIGetMainViewport();

        // auto viewport_extent = main_viewport->GetViewportExtent();

        // RHITextureCreateInfo info = RHITextureCreateInfo::Create2D("upload texture")
        //                                 .SetArraySize(1)
        //                                 .SetNumMips(1)
        //                                 .SetDepth(1)
        //                                 .SetExtent({(int32_t)viewport_extent.width, (int32_t)viewport_extent.height})
        //                                 .SetFormat(PF_R8G8B8A8_SRGB)
        //                                 .SetUsageFlags(ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC)
        //                                 .SetInitialLayout(TEXTURE_LAYOUT_UNDEFINED);

        // for (uint32_t i = 0; i < frame_count; ++i) {

        //     auto&         frame_data   = instance.global_render_data.frame_datas[i];
        //     RHITextureRef temp_texture = g_rhi->RHICreateTexture(info);
        //     temp_texture->AddRef();

        //     frame_data.upload_texture = temp_texture;
        //     frame_data.command_list   = g_rhi->CreateGraphicsCommandList();
        // }
        // instance.global_render_data.graphics_command_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
        // instance.global_render_data.compute_command_queue  = g_rhi->CreateCommandQueue(ECommandQueueType::COMPUTE);
        // instance.global_render_data.transfer_command_queue = g_rhi->CreateCommandQueue(ECommandQueueType::COPY);
    }

    void GlobalRenderResources::ShutDown() {
        // Implementation of ShutDown method
        // ...
        // auto& instance    = GetInstance();
        // auto  frame_count = instance.global_render_data.frame_datas.size();

        // //delete upload buffers
        // for (uint32_t i = 0; i < frame_count; i++) {
        //     instance.global_render_data.frame_datas[i]
        //         .upload_texture->DeRef();
        // }
        // delete instance.global_render_data.graphics_command_queue;
        // delete instance.global_render_data.compute_command_queue;
        // delete instance.global_render_data.transfer_command_queue;
    }

    size_t RHISamplerHash(const RHISamplerInitializer& params) {
        size_t hash = 0;
        //todo
        // HashCombine(hash, params.filter);
        // HashCombine(hash, params.texture_layout);
        // HashCombine(hash, params.address_mode_u);
        // HashCombine(hash, params.address_mode_v);
        // HashCombine(hash, params.address_mode_w);
        // HashCombine(hash, params.mip_lod_bias);
        // HashCombine(hash, params.min_mip_level);
        // HashCombine(hash, params.max_mip_level);
        // HashCombine(hash, params.max_anisotropy);
        // HashCombine(hash, params.border_color);
        // HashCombine(hash, params.compare_op);
        return hash;
    }

    size_t RHITextureViewHash(RHITexture const* texture, const RHIViewInfo::TextureSRV::Initializer& params) {
        //todo
        return reinterpret_cast<size_t>(texture);
        size_t hash = 0;
        // HashCombine(hash, params.texture);
        return hash;
    }

    class SamplerCache::Impl {
    public:
        Impl()  = default;
        ~Impl() = default;
        RHISampler* GetSampler(const RHISamplerInitializer& params) {
            size_t hash = RHISamplerHash(params);
            if (!m_sampler_cache.contains(hash)) {
                RHISamplerRef sampler = g_rhi->RHICreateSampler(params);
                m_sampler_cache[hash] = sampler;
            }
            return m_sampler_cache[hash];
        }
        RHIShaderResourceView* GetTextureView(RHITexture* texture) {
            auto srv_info = RHIViewInfo::CreateTextureSRVInfo()
                                .SetFormat(PF_R8G8B8A8_UNORM)
                                .SetDimension(ETextureDimension::TEX_2D)
                                .SetMipRange(0, 1)
                                .SetArrayRange(0, 1);

            size_t hash = RHITextureViewHash(texture, srv_info);
            if (!m_texture_view_cache.contains(hash)) {
                RHIShaderResourceViewRef texture_view = g_rhi->RHICreateSRV(texture, srv_info);
                m_texture_view_cache[hash]            = texture_view;
            }
            return m_texture_view_cache[hash];
        }

    protected:
        UnorderedMap<size_t, RHISamplerRef>            m_sampler_cache;
        UnorderedMap<size_t, RHIShaderResourceViewRef> m_texture_view_cache;
    };

    SamplerCache& SamplerCache::Get() {
        if (m_instance == nullptr) {
            m_instance = new SamplerCache();
        }
        return *m_instance;
    }
    RHISampler* SamplerCache::GetSampler(const RHISamplerInitializer& params) {
        return m_impl->GetSampler(params);
    }
    RHIShaderResourceView* SamplerCache::GetTextureView(RHITexture* texture) {
        return m_impl->GetTextureView(texture);
    }

    SamplerCache::SamplerCache() {
        m_impl = new Impl();
    }

    SamplerCache::~SamplerCache() {
        delete m_impl;
    }
}// namespace Moer