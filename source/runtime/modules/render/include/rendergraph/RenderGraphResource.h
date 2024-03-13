#pragma once
#include "DepdencyGraph.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
namespace Moer {
    class PassNode;
    class RENDER_API RenderGraphResource : public DepdencyGraph::Node {
    public:
        enum class Type {
            Buffer,
            Texture1D,
            Texture2D,
            Texture3D,
            TextureCube,
            Texture2DMultisample,
        };
        void ConnectForRead(DepdencyGraph& graph, PassNode*, uint32_t usage);
        void ConnectForWrite(DepdencyGraph& graph, PassNode*, uint32_t usage);
        RenderGraphResource(const std::string & name,Type type,bool imported = false);
        //Pass to create this resource
        PassNode * create_pass{nullptr};
        // Pass to destroy this resource
        PassNode * destroy_pass{nullptr};

        //Create Real Resource Before Execute
        virtual void Create() {};
        virtual void Destroy() {};
        virtual ~ RenderGraphResource() = default;
    protected:
        bool m_imported{false};
        
    };

    class RENDER_API RenderGraphBuffer : public RenderGraphResource {
    public:
        using Usage = EBufferUsageFlags;
        struct Descriptor {
            uint32_t          size;
            Usage usage;
        };
        RenderGraphBuffer(const std::string & name,Descriptor desc);
        RenderGraphBuffer(RHIBufferRef);
        void Create() override;
    protected:
        RHIBufferRef m_buffer;
        Descriptor   m_desc;
    };

    // class HardWareTexture;
    // using HWTextureRef = CountableRef<HardWareTexture>;
    

    class RENDER_API RenderGraphTexture : public RenderGraphResource {
    public:
        using Usage = ETextureUsageFlags;
        struct Descriptor {
            Extent2D           extent2D;
            uint16_t           depth;
            EPixelFormat       format;
            Usage usage;
            uint32_t           mipLevels{1};
            uint32_t           arrayLayers{1};
        };
        RHIUAVRef GetUAV();
        RHISRVRef GetSRV();
        void Create() override;
        RenderGraphTexture(const std::string & name,Descriptor desc);
        RenderGraphTexture(RHITextureRef);
    protected:
        RHITextureRef m_texture;
        Descriptor    m_desc;

        struct ViewInfoHashFunc
        {
            std::size_t operator()(const RHIViewInfo& view_info) const
            {
               size_t hash = 0;
               HashCombine(hash,view_info.base_info.view_type);
               HashCombine(hash,view_info.base_info.format);
               if(view_info.IsSRV()) {
                  HashCombine(hash,view_info.texture.srv.mip_min);
                  HashCombine(hash,view_info.texture.srv.mip_num);
                  HashCombine(hash,view_info.texture.srv.array_min);
                  HashCombine(hash,view_info.texture.srv.array_num);
               }
                if(view_info.IsUAV()) {
                    HashCombine(hash,view_info.texture.uav.mip_min);
                    HashCombine(hash,view_info.texture.uav.mip_num);
                    HashCombine(hash,view_info.texture.uav.array_min);
                    HashCombine(hash,view_info.texture.uav.array_num);
                }
                return hash;
            }
        };

        
        mutable std::unordered_map<RHIViewInfo, RHISRVRef, ViewInfoHashFunc> mSrvs;
        mutable std::unordered_map<RHIViewInfo, RHIUAVRef, ViewInfoHashFunc> mUavs;
    };

    class RenderGraphResourceCache {
    public:
        static RenderGraphResourceCache & Get();
        RHITextureRef GetTexture(const std::string & name,RenderGraphTexture::Descriptor);
        RHIBufferRef GetBuffer(const std::string & name,RenderGraphBuffer::Descriptor);
        RHIUAVRef   GetUAV(RHITextureRef texture,uint32_t mip_num = 0, uint32_t array_min =0, uint32_t array_num= 1);
        RHISRVRef   GetSRV(RHITextureRef texture,uint32_t mip_num = 0, uint32_t mip_min = 0, uint32_t array_min =0, uint32_t array_num= 1);
    protected:
        class Impl;
        Impl * m_impl;
    };
}