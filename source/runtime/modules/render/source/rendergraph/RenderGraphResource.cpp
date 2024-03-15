#include "rendergraph/RenderGraphResource.h"
#include "rendergraph/PassNode.h"
#include "rendergraph/RenderGraph.h"
#include "resources/GlobalRenderResources.h"
#include "rhi/RHI.h"
namespace Moer {
    void RenderGraphResource::ConnectForRead(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, this, pass_node, usage);
    }
    void RenderGraphResource::ConnectForWrite(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, pass_node, this, usage);
    }
    RenderGraphResource::RenderGraphResource(const std::string& name, Type type, bool imported) : Node(name), m_type(type),m_imported(imported) {
    }
    RenderGraphBuffer::RenderGraphBuffer(const std::string& name, Descriptor desc) : RenderGraphResource(name, Type::Buffer, false) {
    }
    RenderGraphBuffer::RenderGraphBuffer(const std::string& name, RHIBufferRef buffer) : RenderGraphResource(buffer->GetName(), Type::Buffer, true) {
    }
    void RenderGraphBuffer::Create() {
        //  m_buffer = RenderGraphResourceCache::Get().GetBuffer(name, m_desc);
    }

    // template<typename ViewClass>
    // using CreateFuncType = std::function<
    //     CountableRef<ViewClass>(RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize)>;
    //
    // template<typename ViewClass, typename ViewMapType>
    // CountableRef<ViewClass> findViewCommon(
    //     RHITextureRef             pTexture,
    //     uint32_t                  mip_level,
    //     uint32_t                  mip_num,
    //     uint32_t                  array_level,
    //     uint32_t                  array_num,
    //     ViewMapType&              view_map,
    //     RHIViewInfo::EViewType    view_type,
    //     CreateFuncType<ViewClass> createFunc) {
    //     uint32_t resMipCount  = 1;
    //     uint32_t resArraySize = 1;
    //
    //     resArraySize = pTexture->GetInfo().array_size;
    //     resMipCount  = pTexture->GetNumMips();
    //
    //     if (array_level >= resArraySize) {
    //         LOG_WARNING("First array slice is OOB when creating resource view. Clamping");
    //         array_level = resArraySize - 1;
    //     }
    //
    //     if (mip_level >= resMipCount) {
    //         LOG_WARNING("Most detailed mip is OOB when creating resource view. Clamping");
    //         mip_level = resMipCount - 1;
    //     }
    //
    //     RHIViewInfo view         = RHIViewInfo{};
    //     view.base_info.view_type = view_type;
    //     if (view.IsUAV()) {
    //         view.texture.uav.array_min = array_level;
    //         view.texture.uav.array_num = array_num;
    //         view.texture.uav.mip_min   = mip_level;
    //         view.texture.uav.mip_num   = mip_num;
    //     } else if (view.IsSRV()) {
    //         view.texture.srv.array_min = array_level;
    //         view.texture.srv.array_num = array_num;
    //         view.texture.srv.mip_min   = mip_level;
    //         view.texture.srv.mip_num   = mip_num;
    //     }
    //     if (view_map.find(view) == view_map.end()) {
    //         view_map[view] = createFunc(pTexture, mip_level, mip_num, array_level, array_num);
    //     }
    //
    //     return view_map[view];
    // }

    RHIUAVRef RenderGraphTexture::GetUAV() {
        return RenderGraphResourceCache::Get().GetUAV(m_texture, 1, 0, 1);
        // static CreateFuncType<RHIUAV> createFunc = [](RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize) {
        //     return g_rhi->RHICreateTextureUAV(pTexture, PF_UNDEFINED, mip_min, firstArraySlice, arraySize);
        // };
        // return findViewCommon<RHIUAV>(m_texture, 0, 1, 0, 1, mUavs, RHIViewInfo::EViewType::TEXTURE_UAV, createFunc);
    }
    RHISRVRef RenderGraphTexture::GetSRV() {
        return RenderGraphResourceCache::Get().GetSRV(m_texture, 0, 1, 0, 1);
        // static CreateFuncType<RHISRV> createFunc = [](RHITextureRef pTexture, uint32_t mip_min, uint32_t mip_num, uint32_t firstArraySlice, uint32_t arraySize) {
        //     return g_rhi->RHICreateTextureSRV(pTexture, PF_UNDEFINED, mip_min, mip_num, firstArraySlice, arraySize);
        // };
        // return findViewCommon<RHISRV>(m_texture, 0, 1, 0, 1, mSrvs, RHIViewInfo::EViewType::TEXTURE_SRV, createFunc);
    }
    void RenderGraphTexture::Create() {
        if (m_imported)
            return;
        if (m_texture) {
            LOG_ERROR("Texture already created");
            return;
        }
        m_texture = RenderGraphResourceCache::Get().GetTexture(name, m_desc.extent2D, m_desc.format, m_desc.usage, m_desc.mipLevels, m_desc.arrayLayers);
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, Descriptor desc) : RenderGraphResource(name, Type::Texture2D, false), m_desc(desc) {
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, RHITextureRef texture) : RenderGraphResource(name, Type::Texture2D, true), m_texture(texture) {
    }

}