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

    size_t RHISamplerHash(const RHISamplerCreateInfo& params) {
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
        RHISampler* GetSampler(const RHISamplerCreateInfo& params) {
            size_t hash = RHISamplerHash(params);
            if (!m_sampler_cache.contains(hash)) {
                RHISamplerRef sampler = g_rhi->RHICreateSampler(params);
                m_sampler_cache[hash] = sampler;
            }
            return m_sampler_cache[hash];
        }
        RHISRV* GetTextureView(RHITexture* texture) {
            auto default_format = PF_R8G8B8A8_UNORM;
            auto srv_info       = RHIViewInfo::CreateTextureSRVInfo()
                                .SetFormat(default_format)
                                .SetDimension(ETextureDimension::TEX_2D)
                                .SetMipRange(0, texture->GetNumMips())
                                .SetArrayRange(0, 1);

            size_t hash = RHITextureViewHash(texture, srv_info);
            if (!m_texture_view_cache.contains(hash)) {
                RHISRVRef texture_view     = g_rhi->RHICreateTextureSRV(texture, default_format);
                m_texture_view_cache[hash] = texture_view;
            }
            return m_texture_view_cache[hash];
        }

    protected:
        UnorderedMap<size_t, RHISamplerRef> m_sampler_cache;
        UnorderedMap<size_t, RHISRVRef>     m_texture_view_cache;
    };

    SamplerCache& SamplerCache::Get() {
        if (m_instance == nullptr) {
            m_instance = new SamplerCache();
        }
        return *m_instance;
    }
    RHISampler* SamplerCache::GetSampler(const RHISamplerCreateInfo& params) {
        return m_impl->GetSampler(params);
    }
    RHISRV* SamplerCache::GetTextureView(RHITexture* texture) {
        return m_impl->GetTextureView(texture);
    }

    SamplerCache::SamplerCache() {
        m_impl = new Impl();
    }

    SamplerCache::~SamplerCache() {
        delete m_impl;
    }

    RenderGraphResourceCache& RenderGraphResourceCache::Get() {
        static UniquePtr<RenderGraphResourceCache> m_instance = nullptr;
        if (!m_instance)
            m_instance = std::move(UniquePtr<RenderGraphResourceCache>(MoerNew(RenderGraphResourceCache)()));
        return *m_instance;
    }

    class RenderGraphResourceCache::Impl {
    public:
        // RHITextureRef GetTexture(const std::string& name, RenderGraphTexture::Descriptor desc) {
        //     size_t hash;
        //     HashCombine(hash, name);
        //     HashCombine(hash, desc.extent2D.x);
        //     HashCombine(hash, desc.extent2D.y);
        //     HashCombine(hash, desc.format);
        //     HashCombine(hash, desc.usage);
        //     auto it = m_textures.find(hash);
        //     if (it != m_textures.end()) {
        //         return it->second;
        //     }
        //     RHITextureRef texture = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create2D(name.c_str(), desc.extent2D, desc.format).SetArraySize(1).SetNumMips(1).SetClearAttachment({}).SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));
        //     m_textures.insert({hash, texture});
        //     return texture;
        // }

        RHITextureRef GetTexture(const std::string& name, Extent2D size, EPixelFormat format, ETextureUsageFlags usage, uint32_t mipLevels, uint32_t arrayLayers) {
            size_t hash;
            HashCombine(hash, name);
            HashCombine(hash, size.x);
            HashCombine(hash, size.y);
            HashCombine(hash, format);
            HashCombine(hash, usage);
            HashCombine(hash, mipLevels);
            HashCombine(hash, arrayLayers);
            auto it = m_textures.find(hash);
            if (it != m_textures.end()) {
                return it->second;
            }
            RHITextureRef texture = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create2D(name.c_str(), size, format)
                                                                .SetArraySize(arrayLayers)
                                                                .SetNumMips(mipLevels)
                                                                .SetClearAttachment({})
                                                                .SetUsageFlags(usage));
            m_textures.insert({hash, texture});
            return texture;
        }

        RHISampler* GetSampler(const RHISamplerCreateInfo& params) {
            size_t hash;

            HashCombine(hash, params.filter.GetValue());
            HashCombine(hash, params.texture_layout);
            HashCombine(hash, params.address_mode_u.GetValue());
            HashCombine(hash, params.address_mode_v.GetValue());
            HashCombine(hash, params.address_mode_w.GetValue());
            HashCombine(hash, params.mip_lod_bias);
            HashCombine(hash, params.min_mip_level);
            HashCombine(hash, params.max_mip_level);
            HashCombine(hash, params.max_anisotropy);
            HashCombine(hash, params.border_color);
            HashCombine(hash, params.compare_op.GetValue());
            auto it = mSamplers.find(hash);
            if (it != mSamplers.end()) {
                return it->second;
            }
            RHISamplerRef sampler = g_rhi->RHICreateSampler(params);
            mSamplers.insert({hash, sampler});
            return sampler;
        }

        // RHIBufferRef GetBuffer(const std::string& name, RenderGraphBuffer::Descriptor desc) {
        //     size_t hash;
        //     HashCombine(hash, name);
        //     HashCombine(hash, desc.size);
        //     HashCombine(hash, desc.usage);
        //     auto it = m_buffers.find(hash);
        //     if (it != m_buffers.end()) {
        //         return it->second;
        //     }
        //     //todo float?
        //     RHIBufferRef buffer = g_rhi->RHICreateBuffer<float>(desc.size, static_cast<EBufferUsageFlags>(desc.usage));
        //     m_buffers.insert({hash, buffer});
        //     return buffer;
        // }
        RHIUAVRef GetUAV(RHITextureRef texture, EPixelFormat format, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
            size_t hash;
            //    HashCombine(hash,texture->GetName());
            HashCombine(hash, texture.Get());
            HashCombine(hash, mip_num);
            HashCombine(hash, array_min);
            HashCombine(hash, array_num);
            HashCombine(hash, format);
            auto it = mUavs.find(hash);
            if (it != mUavs.end()) {
                return it->second;
            }
            RHIUAVRef uav = g_rhi->RHICreateTextureUAV(texture, format, 0, array_min, array_num);
            mUavs.insert({hash, uav});
            return uav;
        }
        RHISRVRef GetSRV(RHITextureRef texture, EPixelFormat format, uint32_t mip_num, uint32_t mip_min, uint32_t array_min, uint32_t array_num) {
            size_t hash;
            //    HashCombine(hash,texture->GetName());
            HashCombine(hash, texture.Get());
            HashCombine(hash, mip_num);
            HashCombine(hash, mip_min);
            HashCombine(hash, array_min);
            HashCombine(hash, array_num);
            HashCombine(hash, format);
            auto it = mSrvs.find(hash);
            if (it != mSrvs.end()) {
                return it->second;
            }
            RHISRVRef srv = g_rhi->RHICreateTextureSRV(texture, format, mip_min, mip_num, array_min, array_num);
            mSrvs.insert({hash, srv});
            return srv;
        }

    private:
        mutable Moer::UnorderedMap<size_t, RHITextureRef> m_textures;
        mutable Moer::UnorderedMap<size_t, RHIBufferRef>  m_buffers;
        mutable Moer::UnorderedMap<size_t, RHISRVRef>     mSrvs;
        mutable Moer::UnorderedMap<size_t, RHIUAVRef>     mUavs;
        mutable Moer::UnorderedMap<size_t, RHISamplerRef> mSamplers;
    };

    RHITextureRef RenderGraphResourceCache::GetTexture(const std::string& name, Extent2D size, EPixelFormat format, ETextureUsageFlags usage, uint32_t mipLevels, uint32_t arrayLayers) {
        return m_impl->GetTexture(name, size, format, usage, mipLevels, arrayLayers);
    }

    RHIUAVRef RenderGraphResourceCache::GetUAV(RHITextureRef texture, EPixelFormat format, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
        if (format == PF_UNDEFINED)
            return m_impl->GetUAV(texture, texture->GetFormat(), mip_num, array_min, array_num);
        return m_impl->GetUAV(texture, format, mip_num, array_min, array_num);
    }
    RHISRVRef RenderGraphResourceCache::GetSRV(RHITextureRef texture, EPixelFormat format, uint32_t mip_min, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
        if (format == PF_UNDEFINED)
            return m_impl->GetSRV(texture, texture->GetFormat(), mip_num, mip_min, array_min, array_num);
        return m_impl->GetSRV(texture, format, mip_num, mip_min, array_min, array_num);
    }
    RHISampler* RenderGraphResourceCache::GetSampler(const RHISamplerCreateInfo& params) {
        return m_impl->GetSampler(params);
    }
    RenderGraphResourceCache::~RenderGraphResourceCache() {
        MoerDelete(m_impl);
    }
    RenderGraphResourceCache::RenderGraphResourceCache() : m_impl(MoerNew(Impl)) {
    }
}// namespace Moer