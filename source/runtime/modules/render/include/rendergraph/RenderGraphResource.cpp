#include "RenderGraphResource.h"
#include "PassNode.h"
#include "rhi/RHI.h"
namespace  Moer  {
    void RenderGraphResource::ConnectForRead(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto  edge = MoerNew(DepdencyGraph::Edge)(graph,this,pass_node,usage);
    }
    void RenderGraphResource::ConnectForWrite(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph,pass_node,this,usage);
    }
    RenderGraphBuffer::RenderGraphBuffer(const std::string& name, Descriptor desc):RenderGraphResource(name,Type::Buffer,false) {
        
    }
    RenderGraphBuffer::RenderGraphBuffer(RHIBufferRef buffer):RenderGraphResource(buffer->GetName(),Type::Buffer,true) {
    }
    void RenderGraphBuffer::Create() {
        m_buffer = RenderGraphResourceCache::Get().GetBuffer(name,m_desc);
    }


    template<typename ViewClass>
using CreateFuncType = std::function<
    CountableRef<ViewClass>(RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize)>;

    template<typename ViewClass, typename ViewMapType>
    CountableRef<ViewClass> findViewCommon(
        RHITextureRef pTexture,
        uint32_t mip_level,
        uint32_t mip_num,
        uint32_t array_level,
        uint32_t array_num,
        ViewMapType& view_map,
        RHIViewInfo::EViewType view_type,
        CreateFuncType<ViewClass> createFunc
    )
    {
        uint32_t resMipCount = 1;
        uint32_t resArraySize = 1;

        resArraySize = pTexture->GetInfo().array_size;
        resMipCount = pTexture->GetNumMips();

        if (array_level >= resArraySize)
        {
            LOG_WARNING("First array slice is OOB when creating resource view. Clamping");
            array_level = resArraySize - 1;
        }

        if (mip_level >= resMipCount)
        {
            LOG_WARNING("Most detailed mip is OOB when creating resource view. Clamping");
            mip_level = resMipCount - 1;
        }
        
        RHIViewInfo view = RHIViewInfo{};
        view.base_info.view_type = view_type;
        if(view.IsUAV()) {
            view.texture.uav.array_min = array_level;
            view.texture.uav.array_num = array_num;
            view.texture.uav.mip_min = mip_level;
            view.texture.uav.mip_num = mip_num;
        }
        else if(view.IsSRV()) {
            view.texture.srv.array_min = array_level;
            view.texture.srv.array_num = array_num;
            view.texture.srv.mip_min = mip_level;
            view.texture.srv.mip_num = mip_num;
        }
        if (view_map.find(view) == view_map.end())
        {
            view_map[view] = createFunc(pTexture, mip_level, mip_num, array_level, array_num);
        }

        return view_map[view];
    }
    
    RHIUAVRef RenderGraphTexture::GetUAV() {
        static CreateFuncType<RHIUAV> createFunc = [](RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize) {
            return g_rhi->RHICreateTextureUAV(pTexture, PF_UNDEFINED, mip_min, firstArraySlice, arraySize);
        };
        return findViewCommon<RHIUAV>(m_texture,0,1,0,1,mUavs,RHIViewInfo::EViewType::TEXTURE_UAV,createFunc);
    }
    RHISRVRef RenderGraphTexture::GetSRV() {
        static CreateFuncType<RHISRV> createFunc = [](RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize) {
            return g_rhi->RHICreateTextureSRV(pTexture, PF_UNDEFINED, mip_min, mip_num, firstArraySlice, arraySize);
        };
        return findViewCommon<RHISRV>(m_texture,0,1,0,1,mSrvs,RHIViewInfo::EViewType::TEXTURE_SRV,createFunc);
    }
    void RenderGraphTexture::Create() {
        m_texture = RenderGraphResourceCache::Get().GetTexture(name,m_desc);
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, Descriptor desc):RenderGraphResource(name,Type::Texture2D,false) {
        
    }
    RenderGraphTexture::RenderGraphTexture(RHITextureRef texture):RenderGraphResource(texture->GetName(),Type::Texture2D,true) {
    }
    RenderGraphResourceCache& RenderGraphResourceCache::Get() {
        static UniquePtr<RenderGraphResourceCache> m_instance = nullptr;
        if (!m_instance)
            m_instance = std::move(UniquePtr<RenderGraphResourceCache>(MoerNew(RenderGraphResourceCache)()));
        return *m_instance;
    }

    class RenderGraphResourceCache::Impl {
    public:
        RHITextureRef GetTexture(const std::string& name, RenderGraphTexture::Descriptor desc) {
            size_t hash;
            HashCombine(hash,name);
            HashCombine(hash,desc.extent2D.x);
            HashCombine(hash,desc.extent2D.y);
            HashCombine(hash,desc.format);
            HashCombine(hash,desc.usage);
            auto it = m_textures.find(hash);
            if(it != m_textures.end()) { 
                return it->second;
            }
            RHITextureRef texture = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create2D(name.c_str(),desc.extent2D,desc.format).SetArraySize(1)
                                         .SetNumMips(1)
                                         .SetClearAttachment({})
                                         .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));
            m_textures.insert({hash,texture});
            return texture;
        }

        RHIBufferRef GetBuffer(const std::string& name, RenderGraphBuffer::Descriptor desc) {
            size_t hash;
            HashCombine(hash,name);
            HashCombine(hash,desc.size);
            HashCombine(hash,desc.usage);
            auto it = m_buffers.find(hash);
            if(it != m_buffers.end()) {
                return it->second;
            }
            //todo float?
            RHIBufferRef buffer = g_rhi->RHICreateBuffer<float>(desc.size,static_cast<EBufferUsageFlags>(desc.usage));
            m_buffers.insert({hash,buffer});
            return buffer;
        }
        RHIUAVRef GetUAV(RHITextureRef texture, uint32_t mip_num, uint32_t array_min, uint32_t array_num){
           size_t hash;
            HashCombine(hash,texture->GetName());
            HashCombine(hash,mip_num);
            HashCombine(hash,array_min);
            HashCombine(hash,array_num);
            auto it = mUavs.find(hash);
            if(it != mUavs.end()) {
                return it->second;
            }
            RHIUAVRef uav = g_rhi->RHICreateTextureUAV(texture,PF_UNDEFINED,0,array_min,array_num);
            mUavs.insert({hash,uav});
            return uav;
        }
        RHISRVRef GetSRV(RHITextureRef texture, uint32_t mip_num, uint32_t mip_min, uint32_t array_min, uint32_t array_num){
            size_t hash;
            HashCombine(hash,texture->GetName());
            HashCombine(hash,mip_num);
            HashCombine(hash,mip_min);
            HashCombine(hash,array_min);
            HashCombine(hash,array_num);
            auto it = mSrvs.find(hash);
            if(it != mSrvs.end()) {
                return it->second;
            }
            RHISRVRef srv = g_rhi->RHICreateTextureSRV(texture,PF_UNDEFINED,mip_min,mip_num,array_min,array_num);
            mSrvs.insert({hash,srv});
            return srv;
        }
    private:
        mutable  Moer::UnorderedMap<size_t,RHITextureRef> m_textures;
        mutable  Moer::UnorderedMap<size_t,RHIBufferRef> m_buffers;
        mutable  Moer::UnorderedMap<size_t,RHISRVRef> mSrvs;
        mutable  Moer::UnorderedMap<size_t, RHIUAVRef> mUavs;
    };
    
    RHITextureRef RenderGraphResourceCache::GetTexture(const std::string& name, RenderGraphTexture::Descriptor desc) {
        return m_impl->GetTexture(name,desc);
    }
    RHIBufferRef RenderGraphResourceCache::GetBuffer(const std::string& name, RenderGraphBuffer::Descriptor desc) {
        return m_impl->GetBuffer(name,desc);
    }RHIUAVRef RenderGraphResourceCache::GetUAV(RHITextureRef texture, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
        return m_impl->GetUAV(texture,mip_num,array_min,array_num);
    }
    RHISRVRef RenderGraphResourceCache::GetSRV(RHITextureRef texture, uint32_t mip_num, uint32_t mip_min, uint32_t array_min, uint32_t array_num) {
        return m_impl->GetSRV(texture,mip_num,mip_min,array_min,array_num);
    }

}