#pragma once
#include "DepdencyGraph.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
namespace Moer {

enum class ERDGViewableResourceType : uint8 {
    Texture,
    Buffer,
    NUM
};

class FRDGResource {
public:
    FRDGResource(const FRDGResource&) = delete;
    virtual ~FRDGResource()           = default;

    // Name of the resource for debugging purpose.
    const char* Name = nullptr;

    /** Marks this resource as actually used by a resource. This is to track what dependencies on pass was actually unnecessary. */
    inline void MarkResourceAsUsed() {}

    FRHIResource* GetRHI() const {
        IF_RDG_ENABLE_DEBUG(ValidateRHIAccess());
        return ResourceRHI;
    }

protected:
    FRDGResource(const char* InName) : Name(InName) {}

    FRHIResource* GetRHIUnchecked() const {
        return ResourceRHI;
    }

    bool HasRHI() const {
        return ResourceRHI != nullptr;
    }

    FRHIResource* ResourceRHI = nullptr;

private:
    // friend FRDGBuilder;
    // friend FRDGUserValidation;
    // friend FRDGBarrierValidation;
};

class FRDGViewableResource : public FRDGResource {
public:
    /** The type of this resource; useful for casting between types. */
    const ERDGViewableResourceType Type;

    /** Whether this resource is externally registered with the graph (i.e. the user holds a reference to the underlying resource outside the graph). */
    bool IsExternal() const {
        return bExternal;
    }

    /** Whether this resource is has been queued for extraction at the end of graph execution. */
    bool IsExtracted() const {
        return bExtracted;
    }

    /** Whether a prior pass added to the graph produced contents for this resource. External resources are not considered produced
	 *  until used for a write operation. This is a union of all subresources, so any subresource write will set this to true.
	 */
    bool HasBeenProduced() const {
        return bProduced;
    }

protected:
    FRDGViewableResource(const char* InName, ERDGViewableResourceType InType);

    bool IsCullRoot() const {
        return bExternal || bExtracted;
    }

    static const ERHIAccess DefaultEpilogueAccess = ERHIAccess::SRVMask;

    enum class EAccessMode : uint8 {
        Internal,
        External
    };

    /** Whether this is an externally registered resource. */
    uint8 bExternal : 1;

    /** Whether this is an extracted resource. */
    uint8 bExtracted : 1;

    /** Whether any sub-resource has been used for write by a pass. */
    uint8 bProduced : 1;

    // struct FAccessModeState {
    //     bool IsExternalAccess() const {
    //         return ActiveMode == EAccessMode::External;
    //     }

    //     FAccessModeState() :
    //         Pipelines(ERHIPipeline::None),
    //         Mode(EAccessMode::Internal),
    //         bLocked(0),
    //         bQueued(0) {}

    //     ERHIAccess   Access = ERHIAccess::None;
    //     ERHIPipeline Pipelines : 2;
    //     EAccessMode  Mode : 1;
    //     uint8        bLocked : 1;
    //     uint8        bQueued : 1;

    //     /** The actual access mode replayed on the setup pass timeline. */
    //     EAccessMode ActiveMode = EAccessMode::Internal;

    // } AccessModeState;
    EAccessMode AccessMode = EAccessMode::Internal;

    FRDGPassHandle            AcquirePass;
    FRDGPassHandle            DiscardPass;
    FRDGPassHandle            FirstPass;
    FRDGPassHandlesByPipeline LastPasses;

    /** Number of references in passes and deferred queries. */
    uint32 ReferenceCount;

    /** Scratch index allocated for the resource in the pass being setup. */
    uint32 PassStateIndex = 0;

    /** The state of the resource at the graph epilogue. */
    ERHIAccess EpilogueAccess = DefaultEpilogueAccess;

private:
    static const uint32 DeallocatedReferenceCount = ~0;

    void SetRHI(FRHIResource* Resource) {
        check(!ResourceRHI);
        ResourceRHI = Resource;
    }

    void SetExternalAccessMode(ERHIAccess InAccess, ERHIPipeline InPipelines) {
        check(!AccessModeState.bLocked);

        AccessModeState.Mode      = EAccessMode::External;
        AccessModeState.Access    = InAccess;
        AccessModeState.Pipelines = InPipelines;

        EpilogueAccess = InAccess;
    }

    friend bool IsExtendedLifetimeResource(FRDGViewableResource*);

    // friend FRDGBuilder;
    // friend FRDGUserValidation;
    // friend FRDGBarrierBatchBegin;
    // friend FRDGResourceDumpContext;
    // friend FRDGTrace;
    // friend FRDGPass;
};

class PassNode;
// class RHIGraphicsCommandList;
class RENDER_API RenderGraphResource : public DepdencyGraph::Node {
public:
    enum class Type {
        Buffer,
        Texture1D,
        Texture2D,
        Texture3D,
        TextureCube,
        Texture2DMultisample,
        ALL
    };
    void ConnectForRead(DepdencyGraph& graph, PassNode*, DepdencyGraph::ResourceDesc _desc);
    void ConnectForWrite(DepdencyGraph& graph, PassNode*, DepdencyGraph::ResourceDesc _desc);
    RenderGraphResource(std::string_view name, Type type, bool imported = false);
    Type GetType() const {
        return m_type;
    }

    virtual uint32_t ResloveResourceUsage(
        const DepdencyGraph::ResourceDesc&,
        RHIBarrierDependencyInfo& barrier_info,
        EPassType                 pass_type
    ) = 0;
    //Pass to create this resource
    PassNode* create_pass{nullptr};
    // Pass to destroy this resource
    PassNode* destroy_pass{nullptr};

    //Create Real Resource Before Execute
    virtual void Create() {};
    virtual void Destroy() {};
    virtual ~RenderGraphResource() = default;

protected:
    bool m_imported{false};
    Type m_type;
};

class RENDER_API RenderGraphBuffer : public RenderGraphResource {
public:
    using Usage = EBufferRuntimeUsageFlags;
    struct Descriptor {
        uint32_t size;
        Usage    usage;
    };
    RenderGraphBuffer(std::string_view name, Descriptor desc);
    RenderGraphBuffer(std::string_view name, RHIBufferRef);
    void     Create() override;
    uint32_t ResloveResourceUsage(
        const DepdencyGraph::ResourceDesc&,
        RHIBarrierDependencyInfo& barrier_info,
        EPassType                 pass_type
    ) override;
    RHISRVRef    GetSRV() const;
    RHIUAVRef    GetUAV() const;
    RHIBufferRef GetBuffer() const;

protected:
    RHIBufferRef m_buffer{nullptr};
    Descriptor   m_desc{};
    Usage        m_usage;
};

// class HardWareTexture;
// using HWTextureRef = CountableRef<HardWareTexture>;

class RENDER_API RenderGraphTexture : public RenderGraphResource {
public:
    using Usage = ETextureStateFlags;
    struct Descriptor {
        Extent2D           extent2D;
        uint16_t           depth;
        EPixelFormat       format;
        ETextureUsageFlags usage;
        uint32_t           mipLevels{1};
        uint32_t           arrayLayers{1};
    };
    RHIUAVRef GetUAV(
        EPixelFormat format    = PF_UNDEFINED,
        uint32_t     mip_num   = -1,
        uint32_t     array_min = -1,
        uint32_t     array_num = -1
    );
    RHISRVRef GetSRV(
        EPixelFormat format    = PF_UNDEFINED,
        uint32_t     mip_min   = -1,
        uint32_t     mip_num   = -1,
        uint32_t     array_min = -1,
        uint32_t     array_num = -1
    );
    static ETextureLayout GetTextureLayout(Usage _state);
    RHITextureRef         GetTexture() const;
    EPixelFormat          GetFormat() const;
    void                  Create() override;
    RenderGraphTexture(std::string_view name, Descriptor desc);
    RenderGraphTexture(std::string_view name, RHITextureRef tex);
    RenderGraphTexture(std::string_view name, RenderGraphTexture* parent, RHISubresourceRange sub_res);
    uint32_t ResloveResourceUsage(
        const DepdencyGraph::ResourceDesc&,
        RHIBarrierDependencyInfo& barrier_info,
        EPassType                 type
    ) override;

protected:
    RHITextureRef m_texture;
    Descriptor    m_desc;

    RHISubresourceRange GetSubResource() const;

    RenderGraphTexture* m_parent{nullptr};
    bool                m_is_sub_resource{false};
    RHISubresourceRange m_sub_res{};

    struct MipRange {
        uint32_t mip_min : 16 {0};
        uint32_t mip_num : 16 {1};
    };
    struct ArrayRange {
        uint32_t array_min : 16 {0};
        uint32_t array_num : 16 {1};
    };
};

struct TextureSubResource {
    uint32_t mip_min{0};
    uint32_t mip_num{1};
    uint32_t array_min{0};
    uint32_t array_num{1};
};

class RENDER_API RenderGraphSubResource : public RenderGraphTexture {
    RenderGraphSubResource(std::string_view _name, RenderGraphTexture* _parent, TextureSubResource _sub_res);
};

} // namespace Moer