#ifndef RHI_RESOURCE_H
#define RHI_RESOURCE_H
#include "API_Macro.h"
#include "PixelFormat.h"
#include "RenderCommon.h"
#include "math/Base.h"

#include "misc/EnumBitOperation.h"
#include "misc/Hash.h"
#include "misc/Ptr.h"
#include "misc/CountableRef.h"

#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResourceInitilizer.h"

#include <cassert>
#include <atomic>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <optional>
#include <bitset>
template<typename TStructuredParam>
concept concept_is_shader_struct = requires(TStructuredParam t) {
    std::is_same<typename TStructuredParam::TypeInfo::TParamPtr, TStructuredParam>();
    t.GetStructMetadata();
};
#pragma region forward definitions
class RHICommandListBase;
class RHITexture;
class RHIAmplificationShader;
class RHIBlendState;
class RHIShaderBoundStateInput;
class RHIBuffer;
class RHIComputePipelineState;
class RHIComputeShader;
class RHIDepthStencilState;
class RHIGeometryShader;
class RHIFence;
class RHIGraphicsPipelineState;
class RHIMeshShader;
class RHIPipelineBinaryDataLibrary;
class RHIFragmentShader;
class RHIRasterizationState;
class RHIRayTracingGeometry;
class RHIRayTracingPipelineState;
class RHIRayTracingScene;
class RHIRayTracingAccelerationStructure;
class RHIRayTracingShader;
class RHIRenderQuery;
class RHIRenderQueryPool;
class RHIResource;
class RHISampler;
class RHIMultisampleState;
class RHIShader;
class RHIShaderLibrary;
class RHIShaderResourceView;
class RHIConstantBufferView;
class RHITexture;
class RHITextureReference;
class RHIShaderRootParameterLayout;
class RHIUnorderedAccessView;
class RHIVertexInputState;
class RHIVertexShader;
class RHIViewableResource;
class RHIViewport;

template<concept_is_shader_struct TStructuredType>
class RHIStructuredBuffer;

using RHIAmplificationShaderRef = CountableRef<RHIAmplificationShader>;
using RHIBlendStateRef          = CountableRef<RHIBlendState>;
using RHIShaderBoundStateRef    = CountableRef<RHIShaderBoundStateInput>;
using RHIBufferRef              = CountableRef<RHIBuffer>;

template<concept_is_shader_struct TStructuredType>
using RHIStructuredBufferRef          = CountableRef<RHIStructuredBuffer<TStructuredType>>;
using RHIComputePipelineStateRef      = CountableRef<RHIComputePipelineState>;
using RHIComputeShaderRef             = CountableRef<RHIComputeShader>;
using RHIDepthStencilStateRef         = CountableRef<RHIDepthStencilState>;
using RHIGeometryShaderRef            = CountableRef<RHIGeometryShader>;
using RHIFenceRef                     = CountableRef<RHIFence>;
using RHIGraphicsPipelineStateRef     = CountableRef<RHIGraphicsPipelineState>;
using RHIMeshShaderRef                = CountableRef<RHIMeshShader>;
using RHIPipelineBinaryDataLibraryRef = CountableRef<RHIPipelineBinaryDataLibrary>;
using RHIFragmentShaderRef            = CountableRef<RHIFragmentShader>;
using RHIRasterizationStateRef        = CountableRef<RHIRasterizationState>;
using RHIRayTracingGeometryRef        = CountableRef<RHIRayTracingGeometry>;
using RHIRayTracingPipelineStateRef   = CountableRef<RHIRayTracingPipelineState>;
using RHIRayTracingSceneRef           = CountableRef<RHIRayTracingScene>;
using RHIRayTracingShaderRef          = CountableRef<RHIRayTracingShader>;
using RHIRenderQueryRef               = CountableRef<RHIRenderQuery>;
using RHIRenderQueryPoolRef           = CountableRef<RHIRenderQueryPool>;
using RHIResourceRef                  = CountableRef<RHIResource>;
using RHISamplerRef                   = CountableRef<RHISampler>;
using RHIMultisampleStateRef          = CountableRef<RHIMultisampleState>;
using RHIShaderRef                    = CountableRef<RHIShader>;
using RHIShaderLibraryRef             = CountableRef<RHIShaderLibrary>;
using RHIShaderResourceViewRef        = CountableRef<RHIShaderResourceView>;
using RHITextureRef                   = CountableRef<RHITexture>;
using RHITextureReferenceRef          = CountableRef<RHITextureReference>;
using RHIShaderRootParameterLayoutRef = CountableRef<RHIShaderRootParameterLayout>;
using RHIUnorderedAccessViewRef       = CountableRef<RHIUnorderedAccessView>;
using RHIVertexInputStateRef          = CountableRef<RHIVertexInputState>;
using RHIVertexShaderRef              = CountableRef<RHIVertexShader>;
using RHIViewableResourceRef          = CountableRef<RHIViewableResource>;
using RHIViewportRef                  = CountableRef<RHIViewport>;
#pragma endregion

class Shader;

#pragma region utils definition
struct VertexElement {
    VertexElement() = default;
    VertexElement(
        uint8_t          _binding_index,
        uint8_t          _offset,
        EPixelFormat     _format,
        uint8_t          _attribute_index,
        uint16_t         _stride,
        EVertexInputRate _input_rate)
        : binding_index(_binding_index),
          offset(_offset),
          format(_format),
          attribute_index(_attribute_index),
          stride(_stride),
          input_rate(_input_rate) {}

    bool operator==(const VertexElement& other) const {
        return binding_index == other.binding_index && offset == other.offset && format == other.format && attribute_index == other.attribute_index && stride == other.stride && input_rate == other.input_rate;
    }
    uint8_t                      binding_index;
    uint8_t                      offset;
    EnumInByte<EPixelFormat>     format;
    uint8_t                      attribute_index;
    uint16_t                     stride;
    EnumInByte<EVertexInputRate> input_rate;
    uint8_t                      reserve_byte;
};
static_assert(sizeof(VertexElement) == 8, "VertexElement doesn't match cache line size");

typedef Moer::StaticArray<VertexElement, MAX_VERTEX_ELEMENT_COUNT> VertexInputStateInitializerList;

#pragma endregion

class RENDER_API RHIResource {
public:
    explicit RHIResource(ERHIResourceType _type = ERHIResourceType::RRT_NONE) : type(_type) {}
    virtual ~RHIResource() = default;

public:
    uint32_t AddRef() {
        //first AddRef() happens before DeRef
        int32_t ref_count = flags.AddRef(std::memory_order_acquire);
        assert(ref_count > 0);
        return ref_count;
    };

    uint32_t DeRef() {
        int32_t ref_count = flags.DeRef(std::memory_order_release);
        assert(ref_count >= 0);
        if (ref_count == 0) {
            Destroy();
        }
        return (uint32_t)ref_count;
    };
    //only for look-up purposes, don't care about sequences
    uint32_t GetRefCount() const { return (uint32_t)flags.GetRefCount(std::memory_order_relaxed); }

    bool IsValid() const { return flags.IsValid(std::memory_order_relaxed); }
    void Delete() {
        if (flags.MarkToDelete(std::memory_order_acquire)) {
            // delete this;
        }
    }
    ERHIResourceType GetResourceType() const {
        return type;
    }

protected:
private:
    void Destroy();
    struct ResourceAtomicFlags {
        std::atomic<uint32_t> packed;

        static constexpr uint32_t s_mark_for_delete_mask = 1 << 31;
        static constexpr uint32_t s_is_deleting_mask     = 1 << 30;
        static constexpr uint32_t s_ref_count_mask       = s_is_deleting_mask - 1;

    public:
        int32_t AddRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_add(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) + 1;
            assert(num_ref < s_mark_for_delete_mask);
            return num_ref;
        }
        int32_t DeRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_sub(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) - 1;
            assert(num_ref >= 0);
            return num_ref;
        }
        bool MarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_or(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            return (current_packed & s_mark_for_delete_mask) != 0;
        }

        bool UnMarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_xor(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            bool current_mark_for_delete = (current_packed & s_mark_for_delete_mask) != 0;
            assert(current_mark_for_delete && "resource is not marked for deleting");
            return current_mark_for_delete;
        }
        bool IsDeleting() {
            /* make sure packed data processing sequence handled correctly - acquire-rel */
            uint32_t current_packed = packed.load(std::memory_order_acquire);
            assert((current_packed & s_mark_for_delete_mask) != 0 && "resource not marked for deleting");
            assert((current_packed & s_is_deleting_mask) != 0 && "resource is currently deleting");
            uint32_t num_ref = current_packed & s_ref_count_mask;
            if (num_ref == 0) {
                return true;
            }
            UnMarkToDelete(std::memory_order_release);
            return false;
        }
        bool IsValid(std::memory_order memory_order) {
            uint32_t current_packed = packed.load(memory_order);
            return (current_packed & s_mark_for_delete_mask) == 0 && (current_packed & s_ref_count_mask) > 0;
        }

        bool IsMarkedForDeleting(std::memory_order memory_order) {
            return (packed.load(memory_order) & s_mark_for_delete_mask) != 0;
        }
        int32_t GetRefCount(std::memory_order memory_order) {
            return packed.load(memory_order) & s_ref_count_mask;
        }
    };
    ERHIResourceType type;
    //for const resource state change
    mutable ResourceAtomicFlags flags;
};

class RHISampler : public RHIResource {
public:
    explicit RHISampler() : RHIResource(RRT_SAMPLER) {}
};
class RHIVertexInputState : public RHIResource {
public:
    RHIVertexInputState() : RHIResource(RRT_VERTEX_STATE_INITIALIZER) {}
    //    virtual bool GetInitializer(VertexInputStateInitializerList& _initializer_list) { return false; }
};
class RHIRasterizationState : public RHIResource {
public:
    explicit RHIRasterizationState() : RHIResource(RRT_RASTERIZE_STATE) {}
    //    virtual bool GetInitializer(struct RHIRasterizationStateInitializer& _init) { return false; }
};

class RHIDepthStencilState : public RHIResource {
public:
    explicit RHIDepthStencilState() : RHIResource(RRT_DEPTH_STENCIL_STATE) {}
    //    virtual bool GetInitializer(struct RHIDepthStencilStateInitializer& _init) { return false; }
};

class RHIMultisampleState : public RHIResource {
public:
    explicit RHIMultisampleState() : RHIResource(RRT_MULTI_SAMPLE_STATE) {}
    //    virtual bool GetInitializer(RHIDepthStencilStateInitializer& _init) { return false; }
};
class RHIBlendState : public RHIResource {
public:
    explicit RHIBlendState() : RHIResource(RRT_BLEND_STATE) {}
    //    virtual bool GetInitializer(struct RHIBlendStateInitializer& _init) { return false; }
};

//class RHIVertexDescription : public RHIResource {
//public:
//    explicit RHIVertexDescription() : RHIResource(RRT_VERTEX_STATE_INITIALIZER) {}
//};

class RHIPipelineShaderBindingState : public RHIResource {
public:
    explicit RHIPipelineShaderBindingState() : RHIResource(RRT_PIPELINE_BOUND_SHADER_STATE) {}
};

#pragma region shader definitions
class RHIShader : public RHIResource {
public:
    RHIShader() = delete;
    RHIShader(ERHIResourceType _type, EShaderType _shader_type, const Shader* _meta_shader) : RHIResource(_type), shader_type(_shader_type), meta_shader(_meta_shader) {}
    FORCEINLINE EShaderType GetShaderType() const {
        return shader_type;
    }

    void          SetHash(const Hash64City& _hash) { hash = _hash; }
    Hash64City    GetHash() const { return hash; }
    const Shader* GetMetaShader() const { return meta_shader; }

    EShaderType shader_type;
    Hash64City  hash;

protected:
    const Shader* meta_shader;
};

class RHIGraphicsShader : public RHIShader {
public:
    RHIGraphicsShader(ERHIResourceType _type, EShaderType _shader_type, const Shader* _meta_shader) : RHIShader(_type, _shader_type, _meta_shader) {}
};

class RHIVertexShader : public RHIGraphicsShader {
public:
    RHIVertexShader(const Shader* _meta_shader) : RHIGraphicsShader(RRT_VERTEX_SHADER, ST_VERTEX, _meta_shader) {}
};

class RHIFragmentShader : public RHIGraphicsShader {
public:
    RHIFragmentShader(const Shader* _meta_shader) : RHIGraphicsShader(RRT_FRAGMENT_SHADER, ST_FRAGMENT, _meta_shader) {}
};

class RHIGeometryShader : public RHIGraphicsShader {
public:
    RHIGeometryShader(const Shader* _meta_shader) : RHIGraphicsShader(RRT_GEOMETRY_SHADER, ST_GEOMETRY, _meta_shader) {}
};

class RHIComputeShader : public RHIShader {
public:
    RHIComputeShader(const Shader* _meta_shader) : RHIShader(RRT_COMPUTE_SHADER, ST_COMPUTE, _meta_shader) {}
};

class RHIMeshShader : public RHIGraphicsShader {
public:
    RHIMeshShader(const Shader* _meta_shader) : RHIGraphicsShader(RRT_MESH_SHADER, ST_MESH, _meta_shader) {}
};
class RHIAmplificationShader : public RHIGraphicsShader {
public:
    RHIAmplificationShader(const Shader* _meta_shader) : RHIGraphicsShader(RRT_AMPLIFICATION_SHADER, ST_AMPLIFICATION, _meta_shader) {}
};

class RHIRayTracingShader : public RHIShader {
public:
    RHIRayTracingShader(EShaderType _shader_type, const Shader* _meta_shader) : RHIShader(RRT_RAY_TRACING_SHADER, _shader_type, _meta_shader) {}
};

class RHIRayGenShader : public RHIRayTracingShader {
public:
    RHIRayGenShader(const Shader* _meta_shader) : RHIRayTracingShader(ST_RAY_GEN, _meta_shader) {}
};
class RHIRayHitShader : public RHIRayTracingShader {
public:
    RHIRayHitShader(const Shader* _meta_shader) : RHIRayTracingShader(ST_RAY_HIT, _meta_shader) {}
};
class RHIRayMissShader : public RHIRayTracingShader {
public:
    RHIRayMissShader(const Shader* _meta_shader) : RHIRayTracingShader(ST_RAY_MISS, _meta_shader) {}
};

class RHIRayCallableShader : public RHIRayTracingShader {
public:
    RHIRayCallableShader(const Shader* _meta_shader) : RHIRayTracingShader(ST_RAY_CALLABLE, _meta_shader) {}
};
#pragma endregion

#pragma region pipeline states definitions

class RHIGraphicsPipelineState : public RHIResource {
public:
    RHIGraphicsPipelineState() : RHIResource(RRT_GRAPHIC_PIPELINE_STATE) {}

    bool IsValid() const { return b_valid; }
    void SetValid(bool _b_valid) { b_valid = _b_valid; }

private:
    bool b_valid = true;
};

class RHIComputePipelineState : public RHIResource {
public:
    RHIComputePipelineState() : RHIResource(RRT_COMPUTE_PIPELINE_STATE) {}
    bool IsValid() const { return b_valid; }
    void SetValid(bool _b_valid) { b_valid = _b_valid; }

private:
    bool b_valid = true;
};

class RHIRayTracingPipelineState : public RHIResource {
public:
    RHIRayTracingPipelineState() : RHIResource(RRT_RAY_TRACING_PIPELINE_STATE) {}
};

#pragma endregion

#pragma region rhi shader definitions

struct RHIResourceParameterLayout {
    uint16_t               offset;
    uint16_t               stride;
    uint8_t                slot;
    uint8_t                space;
    EShaderBindingBaseType base_type;
};

template<typename RootParameter>
concept concept_is_root_parameter_struct = requires(RootParameter t) {
    RootParameter::TypeInfo::GetStructMetadata();
    t.GetMembers();
};

struct RHIShaderResourceParameter {
    RHIResource* resource;
    uint16_t     slot;
    uint16_t     space;
};

struct RHIShaderConstantParameter {
    EShaderType shader_type;
    uint32_t    byte_offset_in_raw_data;
    uint16_t    size_in_32bit;
    uint16_t    slot;
    uint16_t    space;
};
struct RHIAttachmentBindingParameter {
};
struct RHIBatchedShaderParameters {
    //CBV SRV UAV SAMPLER
    template<concept_is_root_parameter_struct TRootParameter>
    void SetParameters(const Shader* shader, const TRootParameter& params) {
        size_t data_size = sizeof(TRootParameter);
        SetParameters(shader, data_size, (uint8_t*)&params);
    }

    template<concept_is_root_parameter_struct TRootParameter>
    void SetParameters(RHIShader* shader, const TRootParameter& params) {
        size_t data_size = sizeof(TRootParameter);
        SetParameters(shader, data_size, (uint8_t*)&params);
    }

    const uint8_t* GetConstData(uint32_t byte_offset) const {
        return &raw_data[byte_offset];
    }

    const Moer::Array<RHIShaderResourceParameter>& GetResourceParameters() const {
        return resource_parameters;
    }

    const Moer::Array<RHIShaderConstantParameter>& GetConstantParameters() const {
        return constant_parameters;
    }

    const Moer::Array<uint8_t>& GetRawData() const {
        return raw_data;
    }

private:
    void SetParameters(const Shader* shader, size_t _data_size, uint8_t* data_source);
    void SetParameters(RHIShader* shader, size_t _data_size, uint8_t* data_source);
    //offset in raw_data, size, slot and space
    Moer::Array<RHIShaderResourceParameter> resource_parameters;
    Moer::Array<RHIShaderConstantParameter> constant_parameters;
    Moer::Array<uint8_t>                    raw_data;
};
//todo: may not inherit from RHIResource
class RHIShaderRootParameterLayout : public RHIResource {
public:
    RHIShaderRootParameterLayout() : RHIResource(RRT_ROOT_PARAMETER_LAYOUT) {}
    ~RHIShaderRootParameterLayout() {}

    Moer::Array<RHIResourceParameterLayout> resource_parameters;
};

#pragma endregion

#pragma region viewable resources definitions
/* resources which is can be access by shaders via UAV/SRV, like buffer, texture, etc. */
class RHIViewableResource : public RHIResource {
public:
    std::string GetName() const {
        return name;
    }

protected:
    RHIViewableResource(ERHIResourceType _type) : RHIResource(_type) {}
    std::string name;
};

struct RHIBufferInfo {
    uint32_t          size;
    uint32_t          stride;
    EBufferUsageFlags usage;

    RHIBufferInfo() = default;
    RHIBufferInfo(uint32_t _size, uint32_t _stride, EBufferUsageFlags _usage)
        : size(_size),
          stride(_stride),
          usage(_usage) {}

    static RHIBufferInfo GetNull() {
        return {
            0,
            0,
            EBufferUsageFlags::NONE};
    }
    bool IsNull() const {
        return usage == EBufferUsageFlags::NONE && size == stride && size == 0;
    }
};

struct RHIBufferCreateInfo : public RHIBufferInfo {

    RHIBufferCreateInfo() = default;
    RHIBufferCreateInfo(uint32_t _size, uint32_t _stride, EBufferUsageFlags _usage)
        : RHIBufferInfo(
              _size,
              _stride,
              _usage) {}

    static RHIBufferCreateInfo Create(uint32_t _size = 0, uint32_t _stride = 0, EBufferUsageFlags _usage = EBufferUsageFlags::NONE) {
        return {
            _size,
            _stride,
            _usage};
    }
    RHIBufferCreateInfo& SetSize(uint32_t _size) {
        size = _size;
        return *this;
    }
    RHIBufferCreateInfo& SetStride(uint32_t _stride) {
        stride = _stride;
        return *this;
    }
    RHIBufferCreateInfo& SetUsage(EBufferUsageFlags _usage) {
        usage = _usage;
        return *this;
    }
};

/* index, vertex, staging, indirect */
class RHIBuffer : public RHIViewableResource {
public:
    /**
     * @brief Construct a new RHIBuffer object
     * 
     * @param _info 
     */
    RHIBuffer(const RHIBufferInfo& _info) : RHIViewableResource(RRT_BUFFER), info(_info) {}

    const RHIBufferInfo& GetInfo() const { return info; }
    void                 SetName(const std::string& _name) {
        name = _name;
    }
    uint32_t          GetSize() const { return info.size; }
    uint32_t          GetStride() const { return info.stride; }
    EBufferUsageFlags GetUsage() const { return info.usage; }

protected:
    /**
     * @brief Create an empty RHIBuffer, do nothing in rhi backend
     * 
     */
    RHIBuffer() : RHIViewableResource(RRT_BUFFER) {}
    std::string name;

protected:
    RHIBufferInfo info;
};

struct RHITextureInfo {
    RHITextureInfo() = default;
    RHITextureInfo(const RHITextureInfo& other) { *this = other; }

    RHITextureInfo(ETextureDimension _dimension) : dimension(_dimension) {}

    RHITextureInfo(
        ETextureDimension  _dimension,
        ETextureUsageFlags _usage,
        ETextureLayout     _layout,
        EPixelFormat       _format,
        EClearAttachment   _clear_attachment,
        Moer::Vector2i     _extent,
        uint16_t           _depth,
        uint8_t            _num_mips,
        uint8_t            _num_samples)
        : dimension(_dimension),
          usage(_usage),
          layout(_layout),
          format(_format),
          clear_attachment(_clear_attachment),
          extent(_extent),
          num_mips(_num_mips),
          num_samples(_num_samples) {}

    ETextureUsageFlags usage = ETextureUsageFlags::UNDEFINED;

    ETextureLayout layout = TEXTURE_LAYOUT_UNDEFINED;

    Moer::Vector2i extent = Moer::Vector2i(1, 1);

    /** Depth of the texture if the dimension is 3D. */
    uint16_t depth = 1;

    /** The number of array elements in the texture. (Keep at 1 if dimension is 3D). */
    uint16_t array_size = 1;

    /** Number of mips in the texture mip-map chain. */
    uint8_t num_mips = 1;

    /** Number of samples in the texture. >1 for MSAA. */
    uint8_t num_samples = 1;

    /** Texture dimension to use when creating the RHI texture. */
    ETextureDimension dimension = ETextureDimension::TEX_2D;

    /** Pixel format used to create RHI texture. */
    EPixelFormat format = PF_UNDEFINED;

    /** Texture format used when creating the UAV. PF_Unknown means to use the default one (same as Format). */
    EPixelFormat uav_format = PF_UNDEFINED;

    RHIClearAttachment clear_attachment;

    /* A mask representing which GPUs to create the resource on, in a multi-GPU system. */
    //     GPUMask = FRHIGPUMask::All();

    friend uint32_t GetHash(const RHITextureInfo& target) {
        uint32_t hash = GetHash(target.dimension);
        HashCombine(hash, GetHash(target.format));
        HashCombine(hash, GetHash(target.array_size));
        HashCombine(hash, GetHash(target.usage));
        HashCombine(hash, GetHash(target.layout));
        HashCombine(hash, GetHash(target.extent));
        HashCombine(hash, GetHash(target.depth));
        HashCombine(hash, GetHash(target.uav_format));
        HashCombine(hash, GetHash(target.num_mips));
        HashCombine(hash, GetHash(target.num_samples));
        HashCombine(hash, GetHash(target.clear_attachment));
        return hash;
    }
    bool operator==(const RHITextureInfo& other) const {
        return dimension == other.dimension && usage == other.usage && format == other.format && layout == other.layout && uav_format == other.uav_format && extent == other.extent && depth == other.depth && array_size == other.array_size && num_mips == other.num_mips && num_samples == other.num_samples && clear_attachment == other.clear_attachment;
    }

    bool operator!=(const RHITextureInfo& other) const {
        return !(*this == other);
    }

    RHITextureInfo& operator=(const RHITextureInfo& other) = default;
};

struct RHITextureCreateInfo : public RHITextureInfo {

    RHITextureCreateInfo() = default;

    // Constructor with minimal argument set. Name and dimension are always required.
    RHITextureCreateInfo(const char* _name, ETextureDimension _dimension)
        : RHITextureInfo(_dimension),
          name(_name) {
    }

    // Constructor for when you already have an FRHITextureDesc
    RHITextureCreateInfo(
        RHITextureInfo const& _info,
        const char*           _name)
        : RHITextureInfo(_info) {}

    static RHITextureCreateInfo Create(const char* _name, ETextureDimension _dimension) {
        return {_name, _dimension};
    }
    static RHITextureCreateInfo Create2D(const char* _name) {
        return {_name, ETextureDimension::TEX_2D};
    }

    static RHITextureCreateInfo Create3D(const char* _name) {
        return {_name, ETextureDimension::TEX_3D};
    }
    static RHITextureCreateInfo Create2DArray(const char* _name) {
        return {_name, ETextureDimension::TEX_2D_ARRAY};
    }
    static RHITextureCreateInfo CreateCube(const char* _name) {
        return {_name, ETextureDimension::TEX_CUBE};
    }
    static RHITextureCreateInfo CreateCubeArray(const char* _name) {
        return {_name, ETextureDimension::TEX_CUBE_ARRAY};
    }
    static RHITextureCreateInfo Create2D(const char* _name, Moer::Vector2i _size, EPixelFormat _format) {
        return Create2D(_name).SetExtent(_size).SetFormat(_format);
    }

    RHITextureCreateInfo& SetUsageFlags(ETextureUsageFlags _usage) {
        usage = _usage;
        return *this;
    }
    RHITextureCreateInfo& AddUsageFlags(ETextureUsageFlags _usage) {
        usage |= _usage;
        return *this;
    }
    RHITextureCreateInfo& SetClearAttachment(RHIClearAttachment _attachment) {
        clear_attachment = _attachment;
        return *this;
    }
    RHITextureCreateInfo& SetExtent(const Moer::Vector2i _extent) {
        extent = _extent;
        return *this;
    }
    RHITextureCreateInfo& SetExtent(int32_t _x, int32_t _y) {
        extent = Moer::Vector2i(_x, _y);
        return *this;
    }
    RHITextureCreateInfo& SetExtent(uint32_t _extent) {
        extent = Moer::Vector2i(_extent);
        return *this;
    }
    RHITextureCreateInfo& SetDepth(uint16_t _depth) {
        depth = _depth;
        return *this;
    }
    RHITextureCreateInfo& SetArraySize(uint16_t _array_size) {
        array_size = _array_size;
        return *this;
    }
    RHITextureCreateInfo& SetNumMips(uint8_t _num_mips) {
        num_mips = _num_mips;
        return *this;
    }
    RHITextureCreateInfo& SetNumSamples(uint8_t _num_samples) {
        num_samples = _num_samples;
        return *this;
    }
    RHITextureCreateInfo& SetDimension(ETextureDimension _dimension) {
        dimension = _dimension;
        return *this;
    }
    RHITextureCreateInfo& SetInitialLayout(ETextureLayout _texture_layout) {
        layout = _texture_layout;
        return *this;
    }
    RHITextureCreateInfo& SetFormat(EPixelFormat _format) {
        format = _format;
        return *this;
    }
    RHITextureCreateInfo& SetUAVFormat(EPixelFormat _uav_format) {
        uav_format = _uav_format;
        return *this;
    }
    std::string name;
};

class RHITexture : public RHIViewableResource {
public:
    virtual const RHITextureInfo& GetInfo() const { return texture_info; }

    virtual class RHITextureReference* GetTextureRef() { return nullptr; }

    virtual void* GetNativeResource() const { return nullptr; }

    virtual void* GetNativeShaderResourceView() const {
        // Override this in derived classes to expose access to the native texture resource
        return nullptr;
    }

    Moer::Vector3i GetExtent3D() const {
        const RHITextureInfo& info = GetInfo();
        switch (info.dimension) {
            case ETextureDimension::TEX_2D: return {info.extent.x, info.extent.y, 1};
            case ETextureDimension::TEX_2D_ARRAY: return {info.extent.x, info.extent.y, info.array_size};
            case ETextureDimension::TEX_3D: return {info.extent.x, info.extent.y, info.depth};
            case ETextureDimension::TEX_CUBE: return {info.extent.x, info.extent.y, 1};
            case ETextureDimension::TEX_CUBE_ARRAY: return {info.extent.x, info.extent.y, info.array_size};
        }
        return Moer::Vector3i(0, 0, 0);
    }

    Moer::Vector3i GetMipDimension(uint8_t _mip_index) const {
        const RHITextureInfo& info = GetInfo();
        return Moer::Vector3i(std::max(info.extent.x >> _mip_index, 1),
                              std::max(info.extent.y >> _mip_index, 1),
                              std::max(info.depth >> _mip_index, 1));
    }
    void SetName(const std::string& _name) {
        name = _name;
    }

    bool IsMultiSampled() const {
        return GetInfo().num_samples > 1;
    }

    bool HasClearAttachment() const {
        return GetInfo().clear_attachment.attachment != EClearAttachment::NONE;
    }

    uint32_t GetNumMips() const {
        return GetInfo().num_mips;
    }
    EPixelFormat GetFormat() const {
        return GetInfo().format;
    }
    uint32_t GetNumSamples() const {
        return GetInfo().num_samples;
    }
    ETextureUsageFlags GetUsageFlags() const {
        return GetInfo().usage;
    }
    EPixelFormat GetUAVFormat() const {
        return GetInfo().uav_format;
    }
    RHIClearAttachment GetClearAttachment() const { return GetInfo().clear_attachment; }

protected:
    RHITexture(const RHITextureCreateInfo& _info);

private:
    friend class RHITextureReference;
    explicit RHITexture(ERHIResourceType _type) : RHIViewableResource(_type) {}
    RHITextureInfo texture_info;
};

#pragma endregion

#pragma region shader param
struct PipelineParametersBinding {
};

#pragma endregion

#pragma region syncronization

enum class EFenceUsageFlags {
    TIMELINE = 1 << 0,
    BINARY   = 1 << 1,
    PRESENT  = 1 << 2,
};
ENUM_BIT_OP_IMPL(EFenceUsageFlags, FLAG)

struct RHIFenceCreateInfo {
    EFenceUsageFlags usage = EFenceUsageFlags::TIMELINE;
};
/* fences in dx12, fence and timeline semaphore in vulkan */
class RHIFence : public RHIResource {
public:
    RHIFence() : RHIResource(RRT_GPU_FENCE) {}
    virtual uint64_t GetValue() const     = 0;
    virtual void     Wait(uint64_t value) = 0;

protected:
};

struct RHIBarrierInfo {
    RHIBarrierInfo() : src_stage(ERHIPipelineStageFlags::PS_TOP_OF_PIPE),
                       dst_stage(ERHIPipelineStageFlags::PS_BOTTOM_OF_PIPE),
                       src_access(ERHIAccessFlags::UNDEFINED),
                       dst_access(ERHIAccessFlags::UNDEFINED) {}
    RHIBarrierInfo(ERHIPipelineStageFlags _src_stage,
                   ERHIPipelineStageFlags _dst_stage,
                   ERHIAccessFlags        _src_access,
                   ERHIAccessFlags        _dst_access)
        : src_stage(_src_stage),
          dst_stage(_dst_stage),
          src_access(_src_access),
          dst_access(_dst_access) {}
    ERHIPipelineStageFlags src_stage;
    ERHIPipelineStageFlags dst_stage;

    ERHIAccessFlags src_access;
    ERHIAccessFlags dst_access;

    RHIBarrierInfo& SetSrcStage(ERHIPipelineStageFlags _src_stage) {
        src_stage = _src_stage;
        return *this;
    }
    RHIBarrierInfo& SetDstStage(ERHIPipelineStageFlags _dst_stage) {
        dst_stage = _dst_stage;
        return *this;
    }

    RHIBarrierInfo& SetSrcAccessFlags(ERHIAccessFlags _src_access_flags) {
        src_access = _src_access_flags;
        return *this;
    }

    RHIBarrierInfo& SetDstAccessFlags(ERHIAccessFlags _dst_access_flags) {
        dst_access = _dst_access_flags;
        return *this;
    }
};

struct RHITextureBarrierInfo : public RHIBarrierInfo {
    RHITextureBarrierInfo()
        : RHIBarrierInfo(),
          p_texture(nullptr),
          src_layout(TEXTURE_LAYOUT_UNDEFINED),
          dst_layout(TEXTURE_LAYOUT_UNDEFINED), sub_resource_range(),
          src_queue_type(ECommandQueueType::UNDEFINED),
          dst_queue_type(ECommandQueueType::UNDEFINED) {}
    RHITextureBarrierInfo(
        ERHIPipelineStageFlags _src_stage,
        ERHIPipelineStageFlags _dst_stage,
        ERHIAccessFlags        _src_access,
        ERHIAccessFlags        _dst_access,
        RHITexture*            _p_texture,
        ETextureLayout         _src_layout,
        ETextureLayout         _dst_layout,
        RHISubresourceRange    _sub_resource_range,
        ECommandQueueType      _src_queue_type,
        ECommandQueueType      _dst_queue_type)
        : RHIBarrierInfo(_src_stage,
                         _dst_stage,
                         _src_access,
                         _dst_access),
          p_texture(_p_texture),
          src_layout(_src_layout),
          dst_layout(_dst_layout),
          sub_resource_range(_sub_resource_range),
          src_queue_type(_src_queue_type),
          dst_queue_type(_dst_queue_type) {}
    static RHITextureBarrierInfo Create() {
        return {};
    }

    RHITexture*         p_texture;
    ETextureLayout      src_layout;
    ETextureLayout      dst_layout;
    RHISubresourceRange sub_resource_range;
    ECommandQueueType   src_queue_type;
    ECommandQueueType   dst_queue_type;

    RHITextureBarrierInfo& SetTexture(RHITexture* _p_texture) {
        p_texture = _p_texture;
        return *this;
    }
    RHITextureBarrierInfo& SetSrcTextureLayout(ETextureLayout _src_layout) {
        src_layout = _src_layout;
        return *this;
    }
    RHITextureBarrierInfo& SetDstTextureLayout(ETextureLayout _dst_layout) {
        dst_layout = _dst_layout;
        return *this;
    }

    RHITextureBarrierInfo& SetSubResourceRange(const RHISubresourceRange& _sub_resource_range) {
        sub_resource_range = _sub_resource_range;
        return *this;
    }

    RHITextureBarrierInfo& SetSrcQueueType(ECommandQueueType _src_queue_type) {
        src_queue_type = _src_queue_type;
        return *this;
    }

    RHITextureBarrierInfo& SetDstQueueType(ECommandQueueType _dst_queue_type) {
        dst_queue_type = _dst_queue_type;
        return *this;
    }
};

struct RHIBufferBarrierInfo : public RHIBarrierInfo {
    RHIBufferBarrierInfo(
        ERHIPipelineStageFlags _src_stage,
        ERHIPipelineStageFlags _dst_stage,
        ERHIAccessFlags        _src_access,
        ERHIAccessFlags        _dst_access,
        ECommandQueueType      _src_queue_type,
        ECommandQueueType      _dst_queue_type,
        RHIBuffer*             _p_buffer,
        uint64_t               _offset,
        uint64_t               _size)
        : RHIBarrierInfo(_src_stage,
                         _dst_stage,
                         _src_access,
                         _dst_access),
          p_buffer(_p_buffer),
          src_queue_type(_src_queue_type),
          dst_queue_type(_dst_queue_type),
          offset(_offset),
          size(_size) {
    }
    RHIBufferBarrierInfo() : RHIBarrierInfo(),
                             p_buffer(nullptr),
                             offset(0),
                             src_queue_type(ECommandQueueType::UNDEFINED),
                             dst_queue_type(ECommandQueueType::UNDEFINED),
                             size(0) {}

    static RHIBufferBarrierInfo Create() {
        return {};
    }
    RHIBufferBarrierInfo& SetBuffer(RHIBuffer* _buffer) {
        p_buffer = _buffer;
        return *this;
    }
    RHIBufferBarrierInfo& SetOffset(uint64_t _offset) {
        offset = _offset;
        return *this;
    }
    RHIBufferBarrierInfo& SetSize(uint64_t _size) {
        size = _size;
        return *this;
    }

    RHIBufferBarrierInfo& SetSrcQueueType(ECommandQueueType _src_queue_type) {
        src_queue_type = _src_queue_type;
        return *this;
    }

    RHIBufferBarrierInfo& SetDstQueueType(ECommandQueueType _dst_queue_type) {
        dst_queue_type = _dst_queue_type;
        return *this;
    }
    RHIBuffer*        p_buffer;
    ECommandQueueType src_queue_type;
    ECommandQueueType dst_queue_type;
    uint64_t          offset;
    uint64_t          size;
};

using RHIMemeryBarrierInfo = RHIBarrierInfo;

struct RHIBarrierDependencyInfo {
    // EBarrierDependencyScope      scope{};
    // uint32_t                     memory_barrier_count  = 0;
    // const RHIMemeryBarrierInfo*  p_memory_barriers     = nullptr;
    // uint32_t                     buffer_barrier_count  = 0;
    // const RHIBufferBarrierInfo*  p_buffer_barriers     = nullptr;
    // uint32_t                     texture_barrier_count = 0;
    // const RHITextureBarrierInfo* p_texture_barriers    = nullptr;

    Moer::Array<RHIMemeryBarrierInfo>  memory_barriers;
    Moer::Array<RHIBufferBarrierInfo>  buffer_barriers;
    Moer::Array<RHITextureBarrierInfo> texture_barriers;
};

#pragma endregion

struct RHIViewportInfo {
    uint32_t     max_frame_in_flight;
    EPixelFormat backbuffer_format;
};

struct RHIViewportNextBackBufferInfo {
    uint32_t  backbuffer_index;
    RHIFence* backbuffer_ready_fence;
};
class RHIViewport : public RHIResource {

public:
    RHIViewport() : RHIResource(RRT_VIEWPORT) {}
    virtual ~RHIViewport(){};
    virtual void* GetNativeSwapchain() const { return nullptr; }
    virtual void* GetNativeWindow(void** _params) const { return nullptr; }

    /**
     * @brief resize viewport, must manually wait for queue that write to frame view complete
     * 
     * @param _size new size
     */
    virtual void OnResize(Extent2D _size) = 0;
    virtual void Present(RHIFence* _render_finished) {}
    /**
     * @brief Get the Next Frame View object
     * 
     * @return RHIView* result view
     */
    virtual RHIViewportNextBackBufferInfo GetNextFrameBackBufferInfo() = 0;

    /**
     * @brief wait for queue tasks complete
     * 
     * @param _command_queue target queue
     * @param _optional_fence for dx12
     */
    virtual void WaitForQueueComplete(class RHICommandQueue* _command_queue, RHIFence* _optional_fence) = 0;

    virtual const RHIViewportInfo& GetViewportInfo() const { return info; }

    virtual ViewPort GetViewportExtent() const = 0;

protected:
    RHIViewportInfo info;
};

#pragma region viewable resources view definitions
struct RHIViewInfo {
    enum class EViewType : uint8_t {
        BUFFER_SRV,
        BUFFER_UAV,
        TEXTURE_SRV,
        TEXTURE_UAV,
        BUFFER_CBV,
        TEXTURE_CBV
    };
    enum class EBufferType : uint8_t {
        UNDEFINED,
        STRUCTURED,
        ACCELERATION_STRUCTURE,
        UNIFROM,
        TEXTURE,
        /* a raw buffer can also be called a byte address buffer */
        RAW
    };

    struct BaseViewInfo {
        EViewType    view_type;
        EPixelFormat format{PF_UNDEFINED};
    };

    struct Buffer : public BaseViewInfo {
        struct ViewInfo;
        EBufferType buffer_type;
        uint8_t     b_is_atomic_counter : 1;
        /* An append and consume buffer is a special type of an unordered resource that
         * supports adding and removing values from the end of a buffer similar to the way a stack works.
         * An append and consume buffer must be a structured buffer */
        uint8_t b_is_append_buffer : 1;
        uint8_t : 6;
        uint32_t byte_offset;
        uint32_t num_elements;
        uint32_t stride;

    protected:
        ViewInfo GetViewInfo(RHIBuffer* target) const;
    };
    /* equivalent to VkImageView */
    struct Texture : public BaseViewInfo {
        struct ViewInfo;
        uint8_t           b_disable_srgb : 1;
        ETextureDimension dimension : uint32_t(ETextureDimension::NumBits);
        uint8_t /*padding*/ : 7 - uint32_t(ETextureDimension::NumBits);
        uint8_t  mip_min;
        uint8_t  mip_num;
        uint16_t array_min;
        uint16_t array_num;

    protected:
        ViewInfo GetViewInfo(RHITexture* target) const;
    };

    struct BufferSRV : public Buffer {
        struct Initializer;
        struct ViewInfo;

        ViewInfo GetViewInfo(RHIBuffer*) const;
    };
    struct BufferUAV : public Buffer {
        struct Initializer;
        struct ViewInfo;
        ViewInfo GetViewInfo(RHIBuffer*) const;
    };
    struct TextureSRV : public Texture {
        struct Initializer;
        struct ViewInfo;
        ViewInfo GetViewInfo(RHITexture*) const;
    };
    struct TextureUAV : public Texture {
        struct Initializer;
        struct ViewInfo;
        ViewInfo GetViewInfo(RHITexture*) const;
    };

    struct BufferCBV : public Buffer {
        struct Initializer;
        struct ViewInfo;
        ViewInfo GetViewInfo(RHIBuffer*) const;
    };
    union {
        BaseViewInfo base_info;
        union {
            BufferSRV srv;
            BufferUAV uav;
            BufferCBV cbv;
        } buffer;
        union {
            TextureSRV srv;
            TextureUAV uav;
        } texture;
    };

    bool IsSRV() const { return base_info.view_type == EViewType::BUFFER_SRV || base_info.view_type == EViewType::TEXTURE_SRV; }
    bool IsUAV() const { return base_info.view_type == EViewType::BUFFER_UAV || base_info.view_type == EViewType::TEXTURE_UAV; }
    bool IsCBV() const { return base_info.view_type == EViewType::BUFFER_CBV || base_info.view_type == EViewType::TEXTURE_CBV; }

    bool IsBuffer() const { return base_info.view_type == EViewType::BUFFER_SRV || base_info.view_type == EViewType::BUFFER_UAV; }
    bool IsTexture() const { return !IsBuffer(); }

    bool operator==(const RHIViewInfo& other) {
        return memcmp(this, &other, sizeof(*this)) == 0;
    }

    bool operator!=(const RHIViewInfo& other) {
        return !(*this == other);
    }

    RHIViewInfo() : RHIViewInfo(EViewType::BUFFER_SRV) {}

    static BufferSRV::Initializer  CreateBufferSRVInfo();
    static BufferUAV::Initializer  CreateBufferUAVInfo();
    static TextureSRV::Initializer CreateTextureSRVInfo();
    static TextureUAV::Initializer CreateTextureUAVInfo();
    static BufferCBV::Initializer  CreateBufferUBVInfo();

protected:
    RHIViewInfo(EViewType _type) {
        base_info.view_type = _type;
    }
};
using RHITextureUAVCreateInfo = RHIViewInfo::TextureUAV::Initializer;

using RHITextureSRVCreateInfo = RHIViewInfo::TextureSRV::Initializer;
using RHIBufferUAVCreateInfo  = RHIViewInfo::BufferUAV::Initializer;
using RHIBufferSRVCreateInfo  = RHIViewInfo::BufferSRV::Initializer;
using RHIBufferUBVCreateInfo  = RHIViewInfo::BufferCBV::Initializer;
struct RHIViewInfo::Buffer::ViewInfo {
    uint32_t     byte_offset;
    uint32_t     byte_stride;
    uint32_t     num_elements;
    uint32_t     byte_size;
    EBufferType  type;
    EPixelFormat format;

    //empty buffer view resource
    bool b_null_view;
};

struct RHIViewInfo::Texture::ViewInfo {
    uint16_t array_min;
    uint16_t array_size;

    EPixelFormat format;

    ETextureDimension dimension;
    uint8_t           b_all_mips;
    uint8_t           b_all_array_slices;
};

struct RHIViewInfo::BufferUAV::ViewInfo : public RHIViewInfo::Buffer::ViewInfo {

    //hlsl usage
    bool b_atomic_counter;

    //append buffer must be unordered access structured buffer
    bool b_append_buffer;
};

struct RHIViewInfo::BufferSRV::ViewInfo : public RHIViewInfo::Buffer::ViewInfo {
};

struct RHIViewInfo::TextureSRV::ViewInfo : public RHIViewInfo::Texture::ViewInfo {
    uint8_t mip_min;
    uint8_t mip_num;
};

struct RHIViewInfo::TextureUAV::ViewInfo : public RHIViewInfo::Texture::ViewInfo {
    //texture uav support one mip level
    uint8_t mip_level;
};

struct RHIViewInfo::BufferCBV::ViewInfo : public RHIViewInfo::Buffer::ViewInfo {
};

static_assert(sizeof(RHIViewInfo) == 16, "Packing of RHIViewInfo is unexpected.");

//for rhi CommandList to create BufferSRV
struct RHIViewInfo::BufferSRV::Initializer : public RHIViewInfo {
    friend RHIViewInfo;
    friend RHICommandListBase;

protected:
    Initializer() : RHIViewInfo(EViewType::BUFFER_SRV) {
        buffer.srv.format = PF_UNDEFINED;
    }

public:
    Initializer& SetType(EBufferType _type) {
        assert(_type != EBufferType::UNDEFINED);
        buffer.srv.buffer_type = _type;
        return *this;
    }
    Initializer& SetType(RHIBuffer* _buffer) {
        buffer.srv.buffer_type = EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::BYTE_ADDRESS_BUFFER)    ? EBufferType::RAW :
                                 EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::STRUCTURED_BUFFER)      ? EBufferType::STRUCTURED :
                                 EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::ACCELERATION_STRUCTURE) ? EBufferType::ACCELERATION_STRUCTURE :
                                                                                                                  EBufferType::UNDEFINED;
        return *this;
    }
    Initializer& SetFormat(EPixelFormat _format) {
        buffer.srv.format = _format;
        return *this;
    }
    Initializer& SetByteOffset(uint32_t _byte_offset) {
        buffer.srv.byte_offset = _byte_offset;
        return *this;
    }
    Initializer& SetStride(uint32_t _stride) {
        buffer.srv.stride = _stride;
        return *this;
    }
    Initializer& SetNumElements(uint32_t _num_elements) {
        buffer.srv.num_elements = _num_elements;
        return *this;
    }
};

//for rhi CommandList to create BufferUAV
struct RHIViewInfo::BufferUAV::Initializer : public RHIViewInfo {
    friend RHIViewInfo;
    friend RHICommandListBase;

protected:
    Initializer() : RHIViewInfo(EViewType::BUFFER_UAV) {
        buffer.uav.format = PF_UNDEFINED;
    }

public:
    Initializer& SetType(EBufferType _type) {
        assert(_type != EBufferType::UNDEFINED);
        buffer.uav.buffer_type = _type;
        return *this;
    }
    Initializer& SetType(RHIBuffer* _buffer) {
        buffer.uav.buffer_type = EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::BYTE_ADDRESS_BUFFER)    ? EBufferType::RAW :
                                 EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::STRUCTURED_BUFFER)      ? EBufferType::STRUCTURED :
                                 EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::ACCELERATION_STRUCTURE) ? EBufferType::ACCELERATION_STRUCTURE :
                                                                                                                  EBufferType::UNDEFINED;
        return *this;
    }
    Initializer& SetFormat(EPixelFormat _format) {
        buffer.uav.format = _format;
        return *this;
    }
    Initializer& SetByteOffset(uint32_t _byte_offset) {
        buffer.uav.byte_offset = _byte_offset;
        return *this;
    }
    Initializer& SetStride(uint32_t _stride) {
        buffer.uav.stride = _stride;
        return *this;
    }
    Initializer& SetNumElements(uint32_t _num_elements) {
        buffer.uav.num_elements = _num_elements;
        return *this;
    }
};

//for rhi CommandList to create TextureSRV
struct RHIViewInfo::TextureSRV::Initializer : public RHIViewInfo {
    friend RHIViewInfo;
    friend RHICommandListBase;

protected:
    Initializer() : RHIViewInfo(EViewType::TEXTURE_SRV) {
        texture.srv.format = PF_UNDEFINED;
    }

public:
    Initializer& SetDimension(ETextureDimension _dimension) {
        texture.srv.dimension = _dimension;
        return *this;
    }
    Initializer& SetDimension(RHITexture* _texture) {
        return SetDimension(_texture->GetInfo().dimension);
    }
    Initializer& SetFormat(EPixelFormat _format) {
        texture.srv.format = _format;
        return *this;
    }
    Initializer& SetMipRange(uint8_t _mip_min, uint8_t _mip_num) {
        texture.srv.mip_min = _mip_min;
        texture.srv.mip_num = _mip_num;
        return *this;
    }
    Initializer& SetArrayRange(uint16_t _array_min, uint16_t _array_num) {
        texture.srv.array_min = _array_min;
        texture.srv.array_num = _array_num;
        return *this;
    }
    Initializer& SetDisableSRGB(bool _b_disable_srgb) {
        texture.srv.b_disable_srgb = _b_disable_srgb;
        return *this;
    }
};

//for rhi CommandList to create TextureUAV
struct RHIViewInfo::TextureUAV::Initializer : public RHIViewInfo {
    friend RHIViewInfo;
    friend RHICommandListBase;

protected:
    Initializer() : RHIViewInfo(EViewType::TEXTURE_UAV) {
        texture.uav.mip_num = 1;
        texture.uav.format  = PF_UNDEFINED;
    }

public:
    Initializer& SetDimension(ETextureDimension _dimension) {
        texture.uav.dimension = _dimension;
        return *this;
    }
    Initializer& SetDimension(RHITexture* _texture) {
        return SetDimension(_texture->GetInfo().dimension);
    }
    Initializer& SetFormat(EPixelFormat _format) {
        texture.uav.format = _format;
        return *this;
    }
    Initializer& SetMipLevel(uint8_t _mip_min) {
        texture.uav.mip_min = _mip_min;
        return *this;
    }
    Initializer& SetArrayRange(uint16_t _array_min, uint16_t _array_num) {
        texture.uav.array_min = _array_min;
        texture.uav.array_num = _array_num;
        return *this;
    }
    Initializer& SetDisableSRGB(bool _b_disable_srgb) {
        texture.uav.b_disable_srgb = _b_disable_srgb;
        return *this;
    }
};

struct RHIViewInfo::BufferCBV::Initializer : public RHIViewInfo {
    friend RHIViewInfo;
    friend RHICommandListBase;

protected:
    Initializer() : RHIViewInfo(EViewType::BUFFER_CBV) {}

public:
    Initializer& SetType(EBufferType _type) {
        assert(_type != EBufferType::UNDEFINED);
        buffer.cbv.buffer_type = _type;
        return *this;
    }
    Initializer& SetType(RHIBuffer* _buffer) {
        buffer.cbv.buffer_type = EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::UNIFORM_BUFFER) ? EBufferType::UNIFROM :
                                 EnumHasAnyFlag(_buffer->GetUsage(), EBufferUsageFlags::TEXTURE_BUFFER) ? EBufferType::TEXTURE :
                                                                                                          EBufferType::UNDEFINED;
        return *this;
    }
    // Initializer& SetFormat(EPixelFormat _format) {
    //     buffer.ubv.format = _format;
    //     return *this;
    // }
    Initializer& SetByteOffset(uint32_t _byte_offset) {
        buffer.cbv.byte_offset = _byte_offset;
        return *this;
    }
    Initializer& SetStride(uint32_t _stride) {
        buffer.cbv.stride = _stride;
        return *this;
    }
    Initializer& SetNumElements(uint32_t _num_elements) {
        buffer.cbv.num_elements = _num_elements;
        return *this;
    }
};

FORCEINLINE RHIViewInfo::BufferSRV::Initializer RHIViewInfo::CreateBufferSRVInfo() {
    return {};
}

FORCEINLINE RHIViewInfo::BufferUAV::Initializer RHIViewInfo::CreateBufferUAVInfo() {
    return {};
}

FORCEINLINE RHIViewInfo::TextureSRV::Initializer RHIViewInfo::CreateTextureSRVInfo() {
    return {};
}
FORCEINLINE RHIViewInfo::TextureUAV::Initializer RHIViewInfo::CreateTextureUAVInfo() {
    return {};
}

FORCEINLINE RHIViewInfo::BufferCBV::Initializer RHIViewInfo::CreateBufferUBVInfo() {
    return {};
}

class RHIView : public RHIResource {
public:
    RHIView(ERHIResourceType _type, RHIViewableResource* _viewable_resource, const RHIViewInfo& _info)
        : RHIResource(_type), resource(_viewable_resource), info(_info) {
        // assert(_viewable_resource != nullptr && "ViewableResource is invalid");
    }

    RHIViewableResource* GetResource() const {
        return resource;
    }

    RHIBuffer* GetBuffer() const {
        return info.IsBuffer() ? dynamic_cast<RHIBuffer*>(resource.Get()) : nullptr;
    }

    RHITexture* GetTexture() const {
        return info.IsTexture() ? dynamic_cast<RHITexture*>(resource.Get()) : nullptr;
    }

    bool IsBuffer() const {
        return info.IsBuffer();
    }

    bool IsTexture() const {
        return info.IsTexture();
    }

    bool IsSRV() const {
        return info.IsSRV();
    }

    bool IsUAV() const {
        return info.IsUAV();
    }

    bool IsCBV() const {
        return info.IsCBV();
    }

protected:
    const RHIViewInfo info;

private:
    CountableRef<RHIViewableResource> resource;
};

class RHIConstantBufferView : public RHIView {
public:
    explicit RHIConstantBufferView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIView(RRT_UNORDERED_ACCESS_VIEW, _resource, _viewInfo) {
        assert(_viewInfo.IsUAV() && "view must be uav");
    }
};

class RHIUnorderedAccessView : public RHIView {
public:
    explicit RHIUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIView(RRT_UNORDERED_ACCESS_VIEW, _resource, _viewInfo) {
        assert(_viewInfo.IsUAV() && "view must be uav");
    }
};

class RHIShaderResourceView : public RHIView {
public:
    explicit RHIShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIView(RRT_SHADER_RESOURCE_VIEW, _resource, _viewInfo) {
        assert(_viewInfo.IsSRV() && "view must be srv");
    }
};
#pragma endregion

#pragma region graphic pipeline definitions
/* converted attachment/RT data */
class RHIColorAttachmentView {
public:
    RHIColorAttachmentView()                                         = default;
    RHIColorAttachmentView(RHIColorAttachmentView&&)                 = default;
    RHIColorAttachmentView(const RHIColorAttachmentView&)            = default;
    RHIColorAttachmentView& operator=(RHIColorAttachmentView&&)      = default;
    RHIColorAttachmentView& operator=(const RHIColorAttachmentView&) = default;

    explicit RHIColorAttachmentView(RHITexture* _texture, EAttachmentLoadOp _load_op)
        : texture(_texture),
          mip_index(0),
          array_index(-1),
          load_op(_load_op),
          store_op(EAttachmentStoreOp::STORE) {}
    explicit RHIColorAttachmentView(RHITexture* _texture, EAttachmentLoadOp _load_op, uint32_t _mip_index, uint32_t _array_index)
        : texture(_texture),
          mip_index(_mip_index),
          load_op(_load_op),
          array_index(_array_index),
          store_op(EAttachmentStoreOp::STORE) {}
    explicit RHIColorAttachmentView(RHITexture* _texture, uint32_t _mip_index, uint32_t _array_index, EAttachmentLoadOp _load_op, EAttachmentStoreOp _store_op)
        : texture(_texture),
          mip_index(_mip_index),
          array_index(_array_index),
          load_op(_load_op),
          store_op(_store_op) {}

    bool operator==(const RHIColorAttachmentView& other) const {
        return texture == other.texture &&
               mip_index == other.mip_index &&
               array_index == other.array_index &&
               load_op == other.load_op &&
               store_op == other.store_op;
    }

public:
    RHITexture* texture   = nullptr;
    uint32_t    mip_index = 0;

    uint32_t array_index = 0;

    EAttachmentLoadOp  load_op  = EAttachmentLoadOp::NONE;
    EAttachmentStoreOp store_op = EAttachmentStoreOp::NONE;
};

/* converted attachment/RT data */
class RHIDepthAttachmentView {
public:
    EAttachmentStoreOp GetStencilStoreOp() const { return stencil_store_op; }

    explicit RHIDepthAttachmentView()
        : texture(nullptr),
          depth_load_op(EAttachmentLoadOp::NONE),
          depth_store_op(EAttachmentStoreOp::NONE),
          stencil_load_op(EAttachmentLoadOp::NONE),
          stencil_store_op(EAttachmentStoreOp::NONE) {
        Validate();
    }

    //common case
    explicit RHIDepthAttachmentView(
        RHITexture*        _texture,
        EAttachmentLoadOp  _load_op,
        EAttachmentStoreOp _store_op)
        : texture(_texture),
          depth_load_op(_load_op),
          depth_store_op(_store_op),
          stencil_load_op(_load_op),
          stencil_store_op(_store_op) {
        Validate();
    }

    explicit RHIDepthAttachmentView(
        RHITexture*        _texture,
        EAttachmentLoadOp  _depth_load_op,
        EAttachmentStoreOp _depth_store_op,
        EAttachmentLoadOp  _stencil_load_op,
        EAttachmentStoreOp _stencil_store_op)
        : texture(_texture),
          depth_load_op(_depth_load_op),
          depth_store_op(_depth_store_op),
          stencil_load_op(_stencil_load_op),
          stencil_store_op(_stencil_store_op) {
        Validate();
    }

    void Validate() const {
        // VK and Metal MAY leave the attachment in an undefined state if the StoreAction is DontCare. So we can't assume read-only implies it should be DontCare unless we know for sure it will never be used again.
        // ensureMsgf(DepthStencilAccess.IsDepthWrite() || DepthStoreAction == ERenderTargetStoreAction::ENoAction, TEXT("Depth is read-only, but we are performing a store.  This is a waste on mobile.  If depth can't change, we don't need to store it out again"));
        /*ensureMsgf(DepthStencilAccess.IsStencilWrite() || StencilStoreAction == ERenderTargetStoreAction::ENoAction, TEXT("Stencil is read-only, but we are performing a store.  This is a waste on mobile.  If stencil can't change, we don't need to store it out again"));*/
    }

    bool operator==(const RHIDepthAttachmentView& other) const {
        return texture == other.texture &&
               depth_load_op == other.depth_load_op &&
               depth_store_op == other.depth_store_op &&
               stencil_load_op == other.stencil_load_op &&
               stencil_store_op == other.stencil_store_op;
    }

public:
    RHITexture* texture;

    EAttachmentLoadOp  depth_load_op;
    EAttachmentStoreOp depth_store_op;
    EAttachmentLoadOp  stencil_load_op;

private:
    EAttachmentStoreOp stencil_store_op;
};

struct RHIShaderMapRef {

    Shader* meta_shader;

    RHIVertexShader*   GetVertexShader();
    RHIFragmentShader* GetFragmentShader();
    RHIGeometryShader* GetGeometryShader();

    RHIMeshShader*          GetMeshShader();
    RHIAmplificationShader* GetAmplificationShader();
};

struct RHIShaderBoundStateInput : public RHIResource {

    RHIShaderBoundStateInput() : RHIResource(RRT_SHADER_BOUND_STATE){};
    RHIShaderBoundStateInput(
        RHIVertexInputState* _vertex_input_state,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader)
        : RHIResource(RRT_SHADER_BOUND_STATE),
          p_vertex_input_state(_vertex_input_state),
          p_vertex_shader(_vertex_shader),
          p_fragment_shader(_fragment_shader),
          p_geometry_shader(_geometry_shader) {}

    RHIShaderBoundStateInput(
        RHIFragmentShader*      _fragment_shader,
        RHIMeshShader*          _mesh_shader,
        RHIAmplificationShader* _amplification_shader)
        : RHIResource(RRT_SHADER_BOUND_STATE),
          p_fragment_shader(_fragment_shader),
          p_mesh_shader(_mesh_shader),
          p_amplification_shader(_amplification_shader) {}

    //todo: shader library support for tracking shader stage resources

    RHIVertexShader*   GetVertexShader() const { return p_vertex_shader; }
    RHIFragmentShader* GetFragmentShader() const { return p_fragment_shader; }

    RHIGeometryShader* GetGeometryShader() const { return p_geometry_shader; }
    void               SetGeometryShader(RHIGeometryShader* _geometry_shader) { p_geometry_shader = _geometry_shader; }

    RHIMeshShader*          GetMeshShader() const { return p_mesh_shader; }
    RHIAmplificationShader* GetAmplificationShader() const { return p_amplification_shader; }

    void SetMeshShader(RHIMeshShader* _mesh_shader) { p_mesh_shader = _mesh_shader; }
    void SetAmplificationShader(RHIAmplificationShader* _amplification_shader) { p_amplification_shader = _amplification_shader; }
    //fields

    RHIVertexInputState* p_vertex_input_state = nullptr;
    RHIVertexShader*     p_vertex_shader      = nullptr;
    RHIFragmentShader*   p_fragment_shader    = nullptr;
    RHIGeometryShader*   p_geometry_shader    = nullptr;

    //todo: query support for mesh shaders
    RHIMeshShader*          p_mesh_shader          = nullptr;
    RHIAmplificationShader* p_amplification_shader = nullptr;
};

//for shader parameter binding usage
struct ColorAttachmementBinding {
    ColorAttachmementBinding() = default;

    ColorAttachmementBinding(RHITexture* _texture, EAttachmentLoadOp _load_op, uint8_t _mip_index, uint8_t _array_index)
        : texture(_texture),
          load_op(_load_op),
          mip_index(_mip_index),
          array_index(_array_index) {}

    ColorAttachmementBinding(RHITexture* _texture, RHITexture* _resolve_texture, EAttachmentLoadOp _load_op, uint8_t _mip_index, uint8_t _array_index)
        : texture(_texture),
          resolve_texture(_resolve_texture),
          load_op(_load_op),
          mip_index(_mip_index),
          array_index(_array_index) {}
    RHITexture* GetTexture() const {
        return texture;
    }
    RHITexture* GetResolveTexture() const {
        return resolve_texture;
    }
    EAttachmentLoadOp GetLoadOp() const {
        return load_op;
    }
    uint8_t GetMipIndex() const {
        return mip_index;
    }
    uint8_t GetArrayIndex() const {
        return array_index;
    }

    void SetTexture(RHITexture* _texture) {
        texture = _texture;
    }
    void SetResolveTexture(RHITexture* _resolve_texture) {
        resolve_texture = _resolve_texture;
    }
    void SetLoadOp(EAttachmentLoadOp _load_op) {
        load_op = _load_op;
    }
    void SetMipIndex(uint8_t _mip_index) {
        mip_index = _mip_index;
    }
    void SetArrayIndex(uint16_t _array_index) {
        array_index = _array_index;
    }

private:
    ShaderParameterPtr<RHITexture*> texture         = nullptr;
    ShaderParameterPtr<RHITexture*> resolve_texture = nullptr;
    EAttachmentLoadOp               load_op         = EAttachmentLoadOp::NONE;
    uint8_t                         mip_index       = 0;
    uint16_t                        array_index     = 0;
};
// static_assert(sizeof(ColorAttachmementBinding) == 16);

struct DepthStencilBinding {
    DepthStencilBinding() = default;

    inline DepthStencilBinding(RHITexture*       _texture,
                               EAttachmentLoadOp _depth_load_op,
                               EAttachmentLoadOp _stencil_load_op)
        : texture(_texture),
          depth_load_op(_depth_load_op),
          stencil_load_op(_stencil_load_op) {}

    RHITexture* GetTexture() const {
        return texture;
    }
    EAttachmentLoadOp GetDepthLoadOp() const {
        return depth_load_op;
    }
    EAttachmentLoadOp GetStencilLoadOp() const {
        return stencil_load_op;
    }
    void SetDepthTexture(RHITexture* _texture) {
        texture = _texture;
    }
    void SetDepthLoadOp(EAttachmentLoadOp _depth_load_op) {
        depth_load_op = _depth_load_op;
    }

    void SetStencilLoadOp(EAttachmentLoadOp _stencil_load_op) {
        stencil_load_op = _stencil_load_op;
    }

private:
    ShaderParameterPtr<RHITexture*> texture         = nullptr;
    EAttachmentLoadOp               depth_load_op   = EAttachmentLoadOp::NONE;
    EAttachmentLoadOp               stencil_load_op = EAttachmentLoadOp::NONE;
};
static_assert(sizeof(ShaderParameterPtr<RHITexture*>) % SHADER_PARAMETER_PTR_ALIGNMENT == 0);

struct alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) AttachmentBindingSlots {

    ColorAttachmementBinding& operator[](uint32_t _index) {
        return color_attachments_binding[_index];
    }

    const ColorAttachmementBinding& operator[](uint32_t index) const {
        return color_attachments_binding[index];
    }
    template<typename Lambda>
    void ForEachColorAttachment(Lambda lambda) {
        for (auto& color_attachment : color_attachments_binding) {
            if (color_attachment.GetTexture() == nullptr) {
                break;
            }
            lambda(color_attachment);
        }
    }

    template<typename Lambda>
    void ForEachColorAttachment(Lambda lambda) const {
        for (auto& color_attachment : color_attachments_binding) {
            if (color_attachment.GetTexture() == nullptr) {
                break;
            }
            lambda(color_attachment);
        }
    }

    uint32_t GetColorAttachmentCount() const {
        uint32_t count = 0;
        for (; count < MAX_PASS_ATTACHMENT_COUNT && color_attachments_binding[count].GetTexture() != nullptr; count++) {
        }
        return count;
    }

    Moer::StaticArray<ColorAttachmementBinding, MAX_PASS_ATTACHMENT_COUNT> color_attachments_binding;
    DepthStencilBinding                                                    depth_stencil_binding;
    Rect2D                                                                 resolve_rect;

    SubpassSettings subpass_settings;
    uint8_t         multi_view_count;
    RHITexture*     shading_rate_texture = nullptr;
};
// static_assert(sizeof(AttachmentBindingSlots) == 240);
// static_assert(offsetof(AttachmentBindingSlots, depth_stencil_binding) == 192);

struct GraphicsPipelineAttachmentInfo {
    GraphicsPipelineAttachmentInfo()
        : attachment_formats(CreateArray<MAX_PASS_ATTACHMENT_COUNT, uint8_t>((uint8_t)ETextureUsageFlags::UNDEFINED)),
          attachment_flags(CreateArray<MAX_PASS_ATTACHMENT_COUNT, ETextureUsageFlags>(ETextureUsageFlags::UNDEFINED)) {}
    uint32_t                                                         attachments_count;
    Moer::StaticArray<uint8_t, MAX_PASS_ATTACHMENT_COUNT>            attachment_formats;
    Moer::StaticArray<ETextureUsageFlags, MAX_PASS_ATTACHMENT_COUNT> attachment_flags;
    EPixelFormat                                                     depth_stencil_attachment_format;
    ETextureUsageFlags                                               depth_stencil_attachment_flag = ETextureUsageFlags::UNDEFINED;

    EAttachmentLoadOp  depth_attachment_load_op    = EAttachmentLoadOp::NONE;
    EAttachmentStoreOp depth_attachment_store_op   = EAttachmentStoreOp::NONE;
    EAttachmentLoadOp  stencil_attachment_load_op  = EAttachmentLoadOp::NONE;
    EAttachmentStoreOp stencil_attachment_store_op = EAttachmentStoreOp::NONE;

    uint16_t num_samples      = 0;
    uint8_t  multi_view_count = 0;

    /* quoted from https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_fragment_density_map.html
     * allows an application to specify areas of the render target where the fragment shader may be invoked fewer times.
     * These fragments are broadcasted out to multiple pixels to cover the render target.
     * */
    bool b_has_fragment_density_attachment = false;
};

class RHIGraphicsPipelineStateInitializer {
public:
    using TAttachmentFormats = Moer::StaticArray<uint8_t, MAX_PASS_ATTACHMENT_COUNT>;
    using TAttachmentFlags   = Moer::StaticArray<ETextureUsageFlags, MAX_PASS_ATTACHMENT_COUNT>;

    RHIGraphicsPipelineStateInitializer()
        : blend_state(nullptr),
          rasterizer_state(nullptr),
          multisample_state(nullptr),
          depth_stencil_state(nullptr),
          color_attachment_count(0),
          color_attachment_formats(CreateArray<MAX_PASS_ATTACHMENT_COUNT, uint8_t>((uint8_t)PF_UNDEFINED)),
          color_attachment_flags(CreateArray<MAX_PASS_ATTACHMENT_COUNT, ETextureUsageFlags>(ETextureUsageFlags::UNDEFINED)),
          depth_stencil_format(PF_UNDEFINED),
          depth_stencil_flag(ETextureUsageFlags::UNDEFINED),
          subpass_settings({SubpassSettings::Type::NONE, 0}),
          b_depth_bound(false),
          multi_view_count(1),
          b_has_fragment_density_attachments(false),
          shading_rate(EVariousShadingRate::VSR_1_1x1),
          hash_key(0) {}

    RHIGraphicsPipelineStateInitializer(
        RHIBlendState*            _blend_state,
        RHIRasterizationState*    _rasterizer_state,
        RHIMultisampleState*      _multisample_state,
        RHIDepthStencilState*     _depth_stencil_state,
        EPrimitiveTopology        _primitive_topology,
        uint32_t                  _color_attachment_count,
        const TAttachmentFormats& _color_attachment_formats,
        const TAttachmentFlags&   _color_attachment_flags,
        EPixelFormat              _depth_stencil_format,
        ETextureUsageFlags        _depth_stencil_flag,
        const SubpassSettings&    _subpass_settings,
        bool                      _b_depth_bound,
        uint8_t                   _multi_view_count,
        bool                      _b_has_fragment_density_attachments,
        EVariousShadingRate       _shading_rate)
        : blend_state(_blend_state),
          rasterizer_state(_rasterizer_state),
          multisample_state(_multisample_state),
          depth_stencil_state(_depth_stencil_state),
          primitive_topology(_primitive_topology),
          color_attachment_count(_color_attachment_count),
          color_attachment_formats(_color_attachment_formats),
          color_attachment_flags(_color_attachment_flags),
          depth_stencil_format(_depth_stencil_format),
          depth_stencil_flag(_depth_stencil_flag),
          subpass_settings(_subpass_settings),
          b_depth_bound(_b_depth_bound),
          multi_view_count(_multi_view_count),
          b_has_fragment_density_attachments(_b_has_fragment_density_attachments),
          shading_rate(_shading_rate),
          hash_key(0) {}
    static constexpr ETextureUsageFlags relevant_color_attachment_flag_mask = ETextureUsageFlags::SRGB;
    static constexpr ETextureUsageFlags relevant_depth_stencil_flag_mask    = ETextureUsageFlags::SRGB | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT;
    static bool                         IsSameColorAttachmentInPSO(ETextureUsageFlags lhs, ETextureUsageFlags rhs) {
        auto l = lhs & relevant_color_attachment_flag_mask;
        auto r = rhs & relevant_color_attachment_flag_mask;
        return l == r;
    }
    static bool IsSameDepthAttachmentInPSO(ETextureUsageFlags lhs, ETextureUsageFlags rhs) {
        auto l = lhs & relevant_depth_stencil_flag_mask;
        auto r = rhs & relevant_depth_stencil_flag_mask;
        return l == r;
    }
    static bool IsSameColorAttachmentArray(const TAttachmentFlags& lhs, const TAttachmentFlags& rhs) {
        bool b_same = true;
        for (int i = 0; i < lhs.size(); ++i) {
            b_same &= IsSameColorAttachmentInPSO(lhs[i], rhs[i]);
        }
        return b_same;
    }

    bool operator==(const RHIGraphicsPipelineStateInitializer& other) const {
        return shader_stage.p_vertex_input_state == other.shader_stage.p_vertex_input_state && shader_stage.p_vertex_shader == other.shader_stage.p_vertex_shader &&
               shader_stage.p_fragment_shader == other.shader_stage.p_fragment_shader &&
               shader_stage.GetMeshShader() == other.shader_stage.GetMeshShader() &&
               shader_stage.GetGeometryShader() == other.shader_stage.GetGeometryShader() &&
               shader_stage.GetAmplificationShader() == other.shader_stage.GetAmplificationShader() &&
               blend_state == other.blend_state && rasterizer_state == other.rasterizer_state &&
               depth_stencil_state == other.depth_stencil_state &&
               primitive_topology == other.primitive_topology &&
               b_depth_bound == other.b_depth_bound && multi_view_count == other.multi_view_count &&
               shading_rate == other.shading_rate &&
               b_has_fragment_density_attachments == other.b_has_fragment_density_attachments &&
               color_attachment_count == other.color_attachment_count &&
               color_attachment_formats == other.color_attachment_formats &&
               IsSameColorAttachmentArray(color_attachment_flags, other.color_attachment_flags) &&
               depth_stencil_format == other.depth_stencil_format &&
               IsSameDepthAttachmentInPSO(depth_stencil_flag, other.depth_stencil_flag) && subpass_settings == other.subpass_settings;
    }

    uint32_t CalcValidColorAttachmentCount() const {
        if (color_attachment_count > 0) {
            int32_t last_index = -1;
            for (int i = (int)color_attachment_count; i >= 0; i--) {
                if (color_attachment_formats[i] != PF_UNDEFINED) {
                    last_index = i;
                    break;
                }
            }
            return (uint32_t)(last_index + 1);
        }
        return color_attachment_count;
    }

    RHIShaderBoundStateInput shader_stage;
    RHIBlendStateRef         blend_state;
    RHIRasterizationStateRef rasterizer_state;
    RHIMultisampleStateRef   multisample_state;
    RHIDepthStencilStateRef  depth_stencil_state;

    EPrimitiveTopology primitive_topology;
    uint32_t           color_attachment_count;
    TAttachmentFormats color_attachment_formats;
    TAttachmentFlags   color_attachment_flags;
    EPixelFormat       depth_stencil_format;
    ETextureUsageFlags depth_stencil_flag;

    SubpassSettings subpass_settings;

    bool    b_depth_bound;
    uint8_t multi_view_count = 1;

    //for VSR
    bool                b_has_fragment_density_attachments;
    EVariousShadingRate shading_rate;

    uint64_t hash_key;
};

class RayTracingPipelineStateInfo {
protected:
    uint64_t hash_ray_gen;
    uint64_t hash_ray_miss;
    uint64_t hash_ray_hit;
    uint64_t hash_ray_callable;

public:
    //should be set in shaders
    uint32_t max_attribute_byte_size = 8;
    //should be set in shaders
    uint32_t max_payload_byte_size      = 24;
    bool     b_allow_hit_group_indexing = true;

    bool operator==(const RayTracingPipelineStateInfo& value) const {
        return max_attribute_byte_size == value.max_attribute_byte_size && max_payload_byte_size == value.max_payload_byte_size && b_allow_hit_group_indexing == value.b_allow_hit_group_indexing && hash_ray_gen == value.hash_ray_gen && hash_ray_miss == value.hash_ray_miss && hash_ray_hit == value.hash_ray_hit && hash_ray_callable == value.hash_ray_callable;
    }
};
class RayTracingPipelineStateInitializer : RayTracingPipelineStateInfo {
public:
    RayTracingPipelineStateInitializer() = default;

    const Moer::Array<RHIRayTracingShader*>& GetRayGenTable() const { return ray_gen_table; }
    const Moer::Array<RHIRayTracingShader*>& GetRayMissTable() const { return ray_miss_table; }
    const Moer::Array<RHIRayTracingShader*>& GetRayHitTable() const { return ray_hit_table; }
    const Moer::Array<RHIRayTracingShader*>& GetRayCallableTable() const { return ray_callable_table; }

    void SetRayGenShaderTable(const Moer::Array<RHIRayTracingShader*>& _ray_gen_shaders, uint64_t _hash = 0) {
        ray_gen_table = _ray_gen_shaders;
        hash_ray_gen  = _hash ? _hash : ComputeHash(Moer::Array<RHIRayTracingShader*>(_ray_gen_shaders.begin(), _ray_gen_shaders.end()));
    }

    void SetRayMissShaderTable(const Moer::Array<RHIRayTracingShader*>& _ray_miss_shaders, uint64_t _hash = 0) {
        ray_miss_table = _ray_miss_shaders;
        hash_ray_miss  = _hash ? _hash : ComputeHash(Moer::Array<RHIRayTracingShader*>(_ray_miss_shaders.begin(), _ray_miss_shaders.end()));
    }
    void SetRayHitShaderTable(const Moer::Array<RHIRayTracingShader*>& _ray_hit_shaders, uint64_t _hash = 0) {
        ray_hit_table = _ray_hit_shaders;
        hash_ray_hit  = _hash ? _hash : ComputeHash(Moer::Array<RHIRayTracingShader*>(_ray_hit_shaders.begin(), _ray_hit_shaders.end()));
    }
    void SetRayCallableShaderTable(const Moer::Array<RHIRayTracingShader*>& _ray_callable_shaders, uint64_t _hash = 0) {
        ray_callable_table = _ray_callable_shaders;
        hash_ray_callable  = _hash ? _hash : ComputeHash(Moer::Array<RHIRayTracingShader*>(_ray_callable_shaders.begin(), _ray_callable_shaders.end()));
    }
    friend uint32_t GetHash(const RayTracingPipelineStateInitializer& value) {
        uint32_t hash = GetHash(value.max_attribute_byte_size);
        HashCombine(hash, value.max_payload_byte_size);
        HashCombine(hash, value.b_allow_hit_group_indexing);
        //todo: combine shader hashes
        return hash;
    }

protected:
    uint64_t ComputeHash(const Moer::Array<RHIRayTracingShader*>& target) {
        for (RHIRayTracingShader* shader : target) {
            //todo: handle sha256 hash combining and convert shader sha256 to uint64_t
            shader->GetHash();
        }
        return 0;
    }

    RHIRayTracingPipelineStateRef     base_pipeline_handle;
    Moer::Array<RHIRayTracingShader*> ray_gen_table;
    Moer::Array<RHIRayTracingShader*> ray_miss_table;
    Moer::Array<RHIRayTracingShader*> ray_hit_table;
    Moer::Array<RHIRayTracingShader*> ray_callable_table;
};

/* struct for RenderPassInfo Only, constructed by texture_view and Pass-Required texture layout */
struct RenderAttachmentView {
    friend RHIViewInfo;
    RHIView*           texture_view    = nullptr;
    ETextureLayout     required_layout = TEXTURE_LAYOUT_UNDEFINED;
    RHIClearAttachment clear_attachment{};
};

//struct AttachmentAction{
//    union{
//        uint8_t value;
//
//        struct{
//            EAttachmentLoadOp  lo_0 : 2;
//            EAttachmentStoreOp so_0 : 2;
//            EAttachmentLoadOp  lo_1 : 2;
//            EAttachmentStoreOp lo_2 : 2;
//        } depth_stencil_ops;
//        struct{
//            EAttachmentLoadOp lo : 4;
//            EAttachmentStoreOp so : 4;
//        } color_ops;
//
//    };
//    AttachmentAction(EAttachmentLoadOp _color_load, EAttachmentStoreOp _color_store){
//        color_ops.lo = _color_load;
//        color_ops.so = _color_store;
//    }
//    AttachmentAction(EAttachmentLoadOp _depth_load, EAttachmentStoreOp _depth_store)
//    operator uint8_t()const{
//        return value;
//    }
//};

enum EAttachmentAction : uint8_t {
    /* for inner definition use, do not use directly */
    INNER_DEPTH_MASK_OFFSET = 4,
#define MAKE_COLOR_ACTION_MASK(LOAD, STORE) ((uint8_t)EAttachmentLoadOp::LOAD << (uint32_t)EAttachmentStoreOp::NumBits | (uint8_t)EAttachmentStoreOp::STORE)
    AC_NO_LOAD_NO_STORE = MAKE_COLOR_ACTION_MASK(NONE, NONE),
    AC_LOAD_NO_STORE    = MAKE_COLOR_ACTION_MASK(LOAD, NONE),
    AC_LOAD_STORE       = MAKE_COLOR_ACTION_MASK(LOAD, STORE),
    AC_CLEAR_NO_STORE   = MAKE_COLOR_ACTION_MASK(CLEAR, NONE),
    AC_NO_LOAD_STORE    = MAKE_COLOR_ACTION_MASK(NONE, STORE),
    AC_CLEAR_STORE      = MAKE_COLOR_ACTION_MASK(CLEAR, STORE),
    AC_CLEAR_RESOLVE    = MAKE_COLOR_ACTION_MASK(CLEAR, MULTISAMPLE_RESOLVE),
    AC_LOAD_RESOLVE     = MAKE_COLOR_ACTION_MASK(CLEAR, MULTISAMPLE_RESOLVE),
#undef MAKE_COLOR_ACTION_MASK

#define MAKE_DEPTH_STENCIL_MASK(DEPTH, STENCIL) ((uint8_t)EAttachmentAction::DEPTH << (uint32_t)INNER_DEPTH_MASK_OFFSET | (uint8_t)EAttachmentAction::STENCIL)
    AC_DS_NO_LOAD_NO_STORE   = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_STORE              = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_STORE, AC_NO_LOAD_STORE),
    AC_DS_DEPTH_STORE        = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_STENCIL_STORE      = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_NO_LOAD_STORE),
    AC_DS_CLEAR_STORE        = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_STORE, AC_CLEAR_STORE),
    AC_DS_LOAD_STORE         = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_LOAD_STORE),
    AC_DS_LOAD_STORE_DEPTH   = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_LOAD_DEPTH         = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_LOAD_STORE_STENCIL = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_LOAD_STORE),

    AC_DS_CLEAR_NO_STORE                 = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_NO_STORE),
    AC_DS_LOAD_NO_STORE                  = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_LOAD_NO_STORE),
    AC_DS_CLEAR_STORE_DEPTH              = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_STORE, AC_CLEAR_NO_STORE),
    AC_DS_CLEAR_STORE_STENCIL            = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_STORE),
    AC_DS_CLEAR_RESOLVE_DEPTH            = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_RESOLVE, AC_CLEAR_NO_STORE),
    AC_DS_CLEAR_RESOLVE_STENCIL          = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_RESOLVE),
    AC_DS_LOAD_DEPTH_CLEAR_STENCIL_STORE = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_CLEAR_STORE),
    AC_DS_CLEAR_STENCIL_STORE_STENCIL    = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_CLEAR_STORE)
#undef MAKE_DEPTH_STENCIL_MASK
};
FORCEINLINE EAttachmentAction GetDepthAction(EAttachmentAction _depth_stencil_action) {
    return (EAttachmentAction)(_depth_stencil_action >> (uint32_t)INNER_DEPTH_MASK_OFFSET);
}
FORCEINLINE EAttachmentAction GetStencilAction(EAttachmentAction _depth_stencil_action) {
    return (EAttachmentAction)(_depth_stencil_action & ((1 << (uint32_t)INNER_DEPTH_MASK_OFFSET) - 1));
}
FORCEINLINE EAttachmentLoadOp GetLoadOp(EAttachmentAction _load_store_action) {
    return (EAttachmentLoadOp)(_load_store_action >> (uint32_t)EAttachmentStoreOp::NumBits);
}
FORCEINLINE EAttachmentStoreOp GetStoreOp(EAttachmentAction _load_store_action) {
    return (EAttachmentStoreOp)(_load_store_action & ((1 << (uint32_t)EAttachmentStoreOp::NumBits)) - 1);
}
using RHITextureRef = CountableRef<RHITexture>;
struct RHIRenderPassInfo {
    /* different from attachment info, this is used for specific RenderPass only*/
    struct ColorAttachmentInfo {
        RenderAttachmentView color_attachment_view;
        RenderAttachmentView resolve_attachment_view;
        EAttachmentAction    color_attachment_action;
    };

    struct DepthStencilAttachmentInfo {
        RenderAttachmentView depth_stencil_attachment_view{};
        RenderAttachmentView resolve_attachment_view{};
        EAttachmentAction    depth_stencil_action;
    };
    static_assert(sizeof(ColorAttachmentInfo) == sizeof(DepthStencilAttachmentInfo));

    Moer::StaticArray<ColorAttachmentInfo, MAX_PASS_ATTACHMENT_COUNT> color_attachments;
    DepthStencilAttachmentInfo                                        depth_stencil_attachment;

    Rect2D render_area;

    //for various shading rate
    RHITextureRef      shading_rate_texture;
    EVRSRateCombinerOp vrs_rate_combiner_op;

    //for vulkan subpass
    SubpassSettings subpass_settings;

    uint8_t multi_view_count = 0;

    RHIRenderPassInfo()                                    = default;
    RHIRenderPassInfo(const RHIRenderPassInfo&)            = default;
    RHIRenderPassInfo& operator=(const RHIRenderPassInfo&) = default;

    explicit RHIRenderPassInfo(RenderAttachmentView _color_attachment_view,
                               EAttachmentAction    _color_attachment_action,
                               RenderAttachmentView _resolve_attachment_view = {}) {
        assert(_color_attachment_view.texture_view && _color_attachment_view.texture_view->IsTexture() && "color attachment source must be a texture");
        assert(_resolve_attachment_view.texture_view == nullptr || _resolve_attachment_view.texture_view->GetTexture()->IsMultiSampled() && "resolve attachment source must support multisampling");
        color_attachments[0].color_attachment_view   = _color_attachment_view;
        color_attachments[0].color_attachment_action = _color_attachment_action;
        color_attachments[0].resolve_attachment_view = _resolve_attachment_view;
    }
    explicit RHIRenderPassInfo(int32_t               _num_color_attachments,
                               RenderAttachmentView* _color_attachment_views,
                               EAttachmentAction     _color_attachment_action) {
        assert(_num_color_attachments > 0 && _num_color_attachments <= MAX_PASS_ATTACHMENT_COUNT && "color attachment count not valid");
        for (int i = 0; i < _num_color_attachments; ++i) {
            assert(_color_attachment_views[i].texture_view != nullptr);
            color_attachments[i].color_attachment_view   = _color_attachment_views[i];
            color_attachments[i].color_attachment_action = _color_attachment_action;
        }
        depth_stencil_attachment.depth_stencil_attachment_view = {};
        depth_stencil_attachment.depth_stencil_action          = AC_DS_NO_LOAD_NO_STORE;
        depth_stencil_attachment.resolve_attachment_view       = {};
    }

    explicit RHIRenderPassInfo(int32_t               _num_color_attachments,
                               RenderAttachmentView* _color_attachment_views,
                               EAttachmentAction     _color_attachment_action,
                               RenderAttachmentView* _resolve_attachment_views) {
        assert(_num_color_attachments > 0 && _num_color_attachments <= MAX_PASS_ATTACHMENT_COUNT && "color attachment count not valid");
        assert(_color_attachment_views && _resolve_attachment_views);
        for (int i = 0; i < _num_color_attachments; ++i) {
            assert(_color_attachment_views[i].texture_view != nullptr && _resolve_attachment_views[i].texture_view != nullptr);
            color_attachments[i].color_attachment_view   = _color_attachment_views[i];
            color_attachments[i].color_attachment_action = _color_attachment_action;

            color_attachments[i].resolve_attachment_view = _resolve_attachment_views[i];
        }

        depth_stencil_attachment.depth_stencil_attachment_view = {};
        depth_stencil_attachment.depth_stencil_action          = AC_DS_NO_LOAD_NO_STORE;
        depth_stencil_attachment.resolve_attachment_view       = {};
    }

    explicit RHIRenderPassInfo(int32_t               _num_color_attachments,
                               RenderAttachmentView* _color_attachment_views,
                               EAttachmentAction     _color_attachment_action,
                               RenderAttachmentView  _depth_stencil_attachment_view,
                               EAttachmentAction     _depth_stencil_action) {
        assert(_num_color_attachments > 0 && _num_color_attachments <= MAX_PASS_ATTACHMENT_COUNT && "color attachment count not valid");
        assert(_color_attachment_views && _depth_stencil_attachment_view.texture_view);
        for (int i = 0; i < _num_color_attachments; ++i) {
            assert(_color_attachment_views[i].texture_view != nullptr);
            color_attachments[i].color_attachment_view   = _color_attachment_views[i];
            color_attachments[i].color_attachment_action = _color_attachment_action;
        }
        assert(_depth_stencil_attachment_view.texture_view);
        depth_stencil_attachment.depth_stencil_attachment_view = _depth_stencil_attachment_view;
        depth_stencil_attachment.depth_stencil_action          = _depth_stencil_action;
        depth_stencil_attachment.resolve_attachment_view       = {};
    }
    explicit RHIRenderPassInfo(int32_t               _num_color_attachments,
                               RenderAttachmentView* _color_attachment_views,
                               EAttachmentAction     _color_attachment_action,
                               RenderAttachmentView* _resolve_attachment_views,
                               RenderAttachmentView  _depth_stencil_attachment_view,
                               RenderAttachmentView  _depth_stencil_attachment_view_resolve,
                               EAttachmentAction     _depth_stencil_action) {
        assert(_num_color_attachments > 0 && _num_color_attachments <= MAX_PASS_ATTACHMENT_COUNT && "color attachment count not valid");
        assert(_color_attachment_views && _resolve_attachment_views && _depth_stencil_attachment_view.texture_view);
        for (int i = 0; i < _num_color_attachments; ++i) {
            assert(_color_attachment_views[i].texture_view != nullptr && _resolve_attachment_views[i].texture_view != nullptr);
            color_attachments[i].color_attachment_view   = _color_attachment_views[i];
            color_attachments[i].color_attachment_action = _color_attachment_action;
            color_attachments[i].resolve_attachment_view = _resolve_attachment_views[i];
        }
        assert(_depth_stencil_attachment_view.texture_view);
        depth_stencil_attachment.depth_stencil_attachment_view = _depth_stencil_attachment_view;
        depth_stencil_attachment.depth_stencil_action          = _depth_stencil_action;
        depth_stencil_attachment.resolve_attachment_view       = _depth_stencil_attachment_view_resolve;
    }

    //depth stencil only, to distinguish from color only initilization
    explicit RHIRenderPassInfo(
        RenderAttachmentView _depth_stencil_attachment_view,
        EAttachmentAction    _depth_stencil_action,
        int32_t              var_ignore                             = 1,
        RenderAttachmentView _depth_stencil_attachment_view_resolve = {}) {
        assert(_depth_stencil_attachment_view.texture_view);
        depth_stencil_attachment.depth_stencil_attachment_view = _depth_stencil_attachment_view;
        depth_stencil_attachment.depth_stencil_action          = _depth_stencil_action;
        depth_stencil_attachment.resolve_attachment_view       = _depth_stencil_attachment_view_resolve;
    }
    explicit RHIRenderPassInfo(RenderAttachmentView _color_attachment_view,
                               EAttachmentAction    _color_attachment_action,
                               RenderAttachmentView _depth_stencil_attachment_view,
                               EAttachmentAction    _depth_stencil_action) {
        assert(_color_attachment_view.texture_view && _color_attachment_view.texture_view->IsTexture() && "color attachment source must be a texture");
        color_attachments[0].color_attachment_view   = _color_attachment_view;
        color_attachments[0].color_attachment_action = _color_attachment_action;
        color_attachments[0].resolve_attachment_view = {};

        assert(_depth_stencil_attachment_view.texture_view);

        depth_stencil_attachment.depth_stencil_attachment_view = _depth_stencil_attachment_view;
        depth_stencil_attachment.depth_stencil_action          = _depth_stencil_action;
        depth_stencil_attachment.resolve_attachment_view       = {};
    }
    explicit RHIRenderPassInfo(RenderAttachmentView _color_attachment_view,
                               EAttachmentAction    _color_attachment_action,
                               RenderAttachmentView _resolve_attachment_view,
                               RenderAttachmentView _depth_stencil_attachment_view,
                               EAttachmentAction    _depth_stencil_action,
                               RenderAttachmentView _depth_stencil_attachment_view_resolve) {
        assert(_color_attachment_view.texture_view && _color_attachment_view.texture_view->IsTexture() && "color attachment source must be a texture");
        assert(_resolve_attachment_view.texture_view == nullptr || _resolve_attachment_view.texture_view->GetTexture()->IsMultiSampled() && "resolve attachment source must support multisampling");
        color_attachments[0].color_attachment_view   = _color_attachment_view;
        color_attachments[0].color_attachment_action = _color_attachment_action;
        color_attachments[0].resolve_attachment_view = _resolve_attachment_view;

        assert(_depth_stencil_attachment_view.texture_view);

        depth_stencil_attachment.depth_stencil_attachment_view = _depth_stencil_attachment_view;
        depth_stencil_attachment.depth_stencil_action          = _depth_stencil_action;
        depth_stencil_attachment.resolve_attachment_view       = _depth_stencil_attachment_view_resolve;
    }

    FORCEINLINE int32_t GetNumColorAttachments() const {
        int32_t count = 0;
        for (; count < MAX_PASS_ATTACHMENT_COUNT; count++) {
            const ColorAttachmentInfo& color_attachment_info = color_attachments[count];
            if (!color_attachment_info.color_attachment_view.texture_view) {
                break;
            }
        }
        return count;
    }

    GraphicsPipelineAttachmentInfo GeneratePipelineAttachmentInfo() const {
        GraphicsPipelineAttachmentInfo target;
        target.num_samples             = 1;
        int32_t color_attachment_index = 0;
        for (; color_attachment_index < MAX_PASS_ATTACHMENT_COUNT; color_attachment_index++) {
            const ColorAttachmentInfo& color_attachment_info = color_attachments[color_attachment_index];
            auto*                      texture_view          = color_attachment_info.color_attachment_view.texture_view;
            if (!texture_view) {
                break;
            }
            target.attachment_formats[color_attachment_index] = texture_view->GetTexture()->GetFormat();
            target.attachment_flags[color_attachment_index]   = texture_view->GetTexture()->GetUsageFlags();
            target.num_samples |= texture_view->GetTexture()->GetNumSamples();
        }
        target.attachments_count = color_attachment_index;
        //set empty value
        for (; color_attachment_index < MAX_PASS_ATTACHMENT_COUNT; ++color_attachment_index) {
            target.attachment_formats[color_attachment_index] = PF_UNDEFINED;
        }
        auto* depth_stencil_view = depth_stencil_attachment.depth_stencil_attachment_view.texture_view;
        if (depth_stencil_view) {
            target.depth_stencil_attachment_format = depth_stencil_view->GetTexture()->GetFormat();
            target.depth_stencil_attachment_flag   = depth_stencil_view->GetTexture()->GetUsageFlags();
            target.num_samples |= depth_stencil_view->GetTexture()->GetNumSamples();
        } else {
            target.depth_stencil_attachment_format = PF_UNDEFINED;
        }
        auto depth_action   = GetDepthAction(depth_stencil_attachment.depth_stencil_action);
        auto stencil_action = GetStencilAction(depth_stencil_attachment.depth_stencil_action);

        target.depth_attachment_load_op  = GetLoadOp(depth_action);
        target.depth_attachment_store_op = GetStoreOp(depth_action);

        target.stencil_attachment_load_op  = GetLoadOp(stencil_action);
        target.stencil_attachment_store_op = GetStoreOp(stencil_action);

        target.multi_view_count                  = multi_view_count;
        target.b_has_fragment_density_attachment = shading_rate_texture.Get() != nullptr;

        return target;
    };
};

#pragma endregion

enum ERayTracingGeometryType : uint8_t {
    RTGT_TRIANGLES,
    RTGT_AABBS
};
enum class ERayTracingGeometryFlags : uint8_t {
    NONE,
    GEOMETRY_OPAQUE                 = 1 << 0,
    NO_DUPLICATE_ANY_HIT_INVOCATION = 1 << 1
};
enum class ERayTracingInstanceFlags : uint8_t {
    NONE,
    TRIANGLE_CULL_DISABLE = 1 << 0,
    //triangle flip face
    TRIANGLE_FRONT_COUNTERCLOCKWISE = 1 << 1,
    FORCE_OPAQUE                    = 1 << 2,
    FORCE_NO_OPAQUE                 = 1 << 3
};
enum class ERayTracingAccelerationStructureBuildFlags : uint8_t {
    NONE,
    ALLOW_UPDATE      = 1 << 0,
    ALLOW_COMPACTION  = 1 << 1,
    PREFER_FAST_TRACE = 1 << 2,
    PREFER_FAST_BUILD = 1 << 3,
    MINIMIZE_MEMORY   = 1 << 4

};

enum class ERayTracingAccelerationStructureCopyMode : uint8_t {
    CLONE       = 0,
    COMPACT     = 0x1,
    SERIALIZE   = 0x2,
    DESERIALIZE = 0x3
};

enum class ERayTracingAccelerationStructureType {
    TOP_LEVEL    = 0,
    BOTTOM_LEVEL = 0x1
};

enum class ERayTracingGeometryInitializerUsage : uint8_t {
    // create buffer and shader params, ready for later usages
    FULL_INITIALIZE,
    // no buffer or shader params, used by streaming system to stream into
    INTERMEDIATE_DST,
    // buffer created but not shader params, use for steaming system intermediate data transfer
    INTERMEDIATE_SRC
};

#pragma region ray -tracing

struct RayTracingAccelerationStructureSizeInfo {
    uint64_t result_size         = 0;
    uint64_t build_scratch_size  = 0;
    uint64_t update_scratch_size = 0;
};

struct RayTracingGeometryElement {
    RHIBufferRef vertex_buffer;

    uint64_t vertex_offset;

    uint64_t vertex_buffer_stride;

    uint32_t max_vertex_count;
    uint32_t start_primitive_id;

    uint32_t num_primitives;

    EPixelFormat             vertex_element_type = PF_R32G32B32_SFLOAT;
    ERayTracingGeometryFlags geometry_flags      = ERayTracingGeometryFlags::NONE;
    bool                     enabled;
    uint8_t                  padding_0;
};
static_assert(sizeof(RayTracingGeometryElement) == 40);

struct RayTracingGeometryInitializer {
    Moer::Array<RayTracingGeometryElement> elements;

    RHIBufferRef index_buffer;

    uint64_t index_offset;

    ERayTracingGeometryType  geo_type = ERayTracingGeometryType::RTGT_TRIANGLES;
    ERayTracingGeometryFlags flags;

    ERayTracingGeometryInitializerUsage        usage;
    ERayTracingAccelerationStructureBuildFlags build_flags;

    uint32_t max_primitive_count = 0;

    // opt: for intermediate usage
    RHIRayTracingGeometry* src_geometry;
};

struct Matrix {
    float matrix[3][4];
};
struct RayTracingGeometryInstance {

    //BLAS
    RHIRayTracingGeometry* p_geometry;

    // a mesh may have multiple instances
    Moer::Array<Matrix>& transforms;

    Moer::Array<uint32_t> scene_instance_data_offsets;

    RHIShaderResourceViewRef transform_data_srv = nullptr;
    uint32_t                 num_transforms;

    uint32_t              default_index = 0;
    Moer::Array<uint32_t> custom_index;

    //sum of previous geo elements, for calculating offset in sbt(prev_element_count * shader_slot_per_element)
    Moer::Array<uint32_t> prev_element_count;

    //each geo copy have one bit
    Moer::Array<std::bitset<32>> activation_masks;

    uint32_t instance_mask : 8;

    ERayTracingInstanceFlags flags = ERayTracingInstanceFlags::NONE;
};

struct RayTracingSceneInitializer {
    Moer::Array<RHIRayTracingGeometryRef> scene_geometries;

    Moer::Array<RHIRayTracingGeometry*> instance_geometries;

    Moer::Array<uint32_t> instance_previous_transform_sum;

    Moer::Array<uint32_t> instance_previous_element_sum;

    // to support multi-layer ray-tracing: certain instance may exist on certain layer/layers
    Moer::Array<uint32_t> instance_count_per_layer;
    uint32_t              num_total_elements;

    uint32_t shader_slots_per_geometry_element;

    uint32_t callable_shader_slot_num;

    uint32_t miss_shader_slot_num = 1;

    RayTracingSceneInitializer()                                        = default;
    RayTracingSceneInitializer(RayTracingSceneInitializer&&)            = default;
    RayTracingSceneInitializer& operator=(RayTracingSceneInitializer&&) = default;
};

class RHIRayTracingAccelerationStructure : public RHIResource {
public:
    RHIRayTracingAccelerationStructure() : RHIResource(RRT_RAYTRACING_ACCELERATION_STRUCTURE) {
    }

    //create result info
    RayTracingAccelerationStructureSizeInfo GetSize() const {
        return size_info;
    }

protected:
    RayTracingAccelerationStructureSizeInfo size_info{};
};

class RHIRayTracingGeometry : RHIRayTracingAccelerationStructure {
public:
    RHIRayTracingGeometry() = default;
    RHIRayTracingGeometry(const RayTracingGeometryInitializer& _init)
        : initializer(_init),
          usage(_init.usage) {}

    virtual void SetInitializer(const RayTracingGeometryInitializer& init) {
        initializer = init;
    }
    const RayTracingGeometryInitializer& GetInitializer() { return initializer; }

    uint32_t GetElementCount() {
        return initializer.elements.size();
    }

protected:
    RayTracingGeometryInitializer       initializer{};
    ERayTracingGeometryInitializerUsage usage = ERayTracingGeometryInitializerUsage::FULL_INITIALIZE;
};

class RHIRayTracingScene : RHIResource {
public:
    RHIRayTracingScene() : RHIResource(RRT_RAYTRACING_SCENE) {}

    virtual const RayTracingGeometryInitializer& GetInitializer() const = 0;
    //for multi-layer ray-tracing
    virtual uint32_t GetLayerBufferOffset(uint32_t LayerIndex) const = 0;
};

#pragma endregion

#pragma region render query
class RHIRenderQuery : public RHIResource {
public:
    RHIRenderQuery() : RHIResource(RRT_RENDER_QUERY) {}
};

class RHIPooledRenderQuery {
public:
    RHIPooledRenderQuery() = default;
    RHIPooledRenderQuery(RHIRenderQueryPool* _pool, RHIRenderQueryRef _query_ref)
        : query_ref(_query_ref), pool(_pool) {}
    ~RHIPooledRenderQuery();

    RHIPooledRenderQuery(const RHIPooledRenderQuery&)            = delete;
    RHIPooledRenderQuery& operator=(const RHIPooledRenderQuery&) = delete;

    RHIPooledRenderQuery(RHIPooledRenderQuery&&)            = default;
    RHIPooledRenderQuery& operator=(RHIPooledRenderQuery&&) = default;

    bool IsValid() { return query_ref.IsValid(); }

    RHIRenderQuery* GetQuery() const { return query_ref; }

    void Release();

protected:
    RHIRenderQueryRef   query_ref;
    RHIRenderQueryPool* pool;
};

class RHIRenderQueryPool : public RHIResource {
public:
    RHIRenderQueryPool() : RHIResource(RRT_RENDER_QUERY_POOL) {}
    virtual ~RHIRenderQueryPool() {}
    virtual RHIPooledRenderQuery AllocateQuery() = 0;

private:
    friend class RHIPooledRenderQuery;
    virtual void ReleaseQuery(RHIRenderQueryRef&& _query) = 0;
};

#pragma endregion
//todo: for rdg usage
#pragma region RDG resource creater
//
///** Used to specify a texture metadata plane when creating a view. from UE 5.3*/
enum class ERHITexturePlane : uint8_t {
    // The primary plane is used with default compression behavior.
    Primary = 0,

    // The primary plane is used without decompressing it.
    PRIMARY_COMPRESSED = 1,

    // The depth plane is used with default compression behavior.
    Depth = 2,

    // The stencil plane is used with default compression behavior.
    Stencil = 3,

    // The HTile plane is used.
    HTile = 4,

    // the FMask plane is used.
    FMask = 5,

    // the CMask plane is used.
    CMask = 6,

    // This enum is packed into various structures. Avoid adding new
    // members without verifying structure sizes aren't increased.
    Num,
    NumBits = 3,

    CompressedSurface = PRIMARY_COMPRESSED,
};
static_assert((1u << uint32_t(ERHITexturePlane::NumBits)) >= uint32_t(ERHITexturePlane::Num), "Not enough bits in the ERHITexturePlane enum");
//
//struct RHITextureSRVInfo {
//    explicit RHITextureSRVInfo(uint8_t _mip_level = 0, uint8_t _num_mips = 0, EPixelFormat _format = PF_UNDEFINED)
//        : format(_format),
//          mip_level(_mip_level),
//          num_mips(_num_mips),
//          array_index(0),
//          array_size(0) {}
//
//    explicit RHITextureSRVInfo(uint8_t _mip_level, uint8_t _num_mips, uint16_t _array_index, uint16_t _array_size, EPixelFormat _format = PF_UNDEFINED)
//        : format(_format),
//          mip_level(_mip_level),
//          num_mips(_num_mips),
//          array_index(_array_index),
//          array_size(_array_size) {}
//
//    EPixelFormat format;
//    uint8_t      mip_level;
//    uint8_t      num_mips;
//
//    uint8_t array_index;
//    uint8_t array_size;
//
//    bool disable_srgb;
//    ERHITexturePlane                 meta_plane = ERHITexturePlane::Primary;
//    std::optional<ETextureDimension> dimension_override;
//
//    FORCEINLINE bool operator==(const RHITextureSRVInfo& other) const {
//        return format == other.format && mip_level == other.mip_level && num_mips == other.num_mips && array_index == other.array_index && array_size == other.array_size && meta_plane == other.meta_plane && dimension_override == other.dimension_override;
//    }
//    FORCEINLINE bool operator!=(const RHITextureSRVInfo& other) const {
//        return !(*this == other);
//    }
//    friend uint32_t GetHash(const RHITextureSRVInfo& value){
//        uint32_t hash = (uint32_t)value.format | (uint32_t)value.mip_level << 8 | (uint32_t)value.num_mips << 16| (uint32_t)value.disable_srgb << 24;
//        hash_combine(hash, (uint32_t)value.array_index | (uint32_t)value.array_size << 16);
//        hash_combine(hash, value.dimension_override.has_value() ? (uint32_t)(value.dimension_override.value()) : std::numeric_limits<uint32_t>::max());
//        hash_combine(hash, (uint32_t)value.meta_plane);
//        return hash;
//    }
//};
//
//struct RHITextureSRVCreateInfo : RHITextureSRVInfo {
//    RHITextureSRVCreateInfo() = default;
//    RHITextureSRVCreateInfo(uint8_t _mip_level = 0, uint8_t _num_mips = 0, EPixelFormat _format = PF_UNDEFINED)
//        : RHITextureSRVInfo(_mip_level, _num_mips, _format) {}
//
//    static RHITextureSRVCreateInfo Create(uint8_t _mip_level = 0, uint8_t _num_mips = 0, EPixelFormat _format = PF_UNDEFINED) {
//        return {_mip_level, _num_mips, _format};
//    }
//    RHITextureSRVCreateInfo& SetFormat(EPixelFormat _format) {
//        format = _format;
//        return *this;
//    }
//    RHITextureSRVCreateInfo& SetMipLevel(uint8_t _mip_level) {
//        mip_level = _mip_level;
//        return *this;
//    }
//    RHITextureSRVCreateInfo& SetNumMips(uint8_t _num_mips) {
//        num_mips = _num_mips;
//        return *this;
//    }
//    RHITextureSRVCreateInfo& SetArrayIndex(uint8_t _array_index) {
//        array_index = _array_index;
//        return *this;
//    }
//
//    RHITextureSRVCreateInfo& SetArrayCount(uint8_t _array_count) {
//        _array_count = _array_count;
//        return *this;
//    }
//
//    RHITextureSRVCreateInfo& SetDisableSRGB(bool _disable_srgb){
//        disable_srgb = _disable_srgb;
//        return *this;
//    }
//    RHITextureSRVCreateInfo& SetMetaTexturePlane(ERHITexturePlane _meta_plane){
//        meta_plane = _meta_plane;
//        return *this;
//    }
//    RHITextureSRVCreateInfo& SetDimensionOverride(ETextureDimension _dimension_override){
//        dimension_override = _dimension_override;
//        return *this;
//    }
//
//};
//struct RHITextureUAVInfo {
//public:
//    RHITextureUAVInfo() = default;
//
//    explicit RHITextureUAVInfo(uint8_t _mip_level = 0, EPixelFormat _format = PF_UNDEFINED, uint16_t _array_index = 0, uint16_t _array_size = 0)
//        : format(_format),
//          mip_level(_mip_level),
//          array_index(_array_index),
//          array_size(_array_size) {}
//
//    explicit RHITextureUAVInfo(ERHITexturePlane _meta_plane)
//        : meta_plane(_meta_plane) {}
//
//    EPixelFormat format;
//    uint8_t      mip_level;
//
//    uint8_t array_index;
//    uint8_t array_size;
//
//    ERHITexturePlane                 meta_plane = ERHITexturePlane::Primary;
//    std::optional<ETextureDimension> dimension_override;
//    FORCEINLINE bool                 operator==(const RHITextureUAVInfo& other) const {
//        return format == other.format && mip_level == other.mip_level && array_index == other.array_index && array_size == other.array_size && meta_plane == other.meta_plane && dimension_override == other.dimension_override;
//    }
//    FORCEINLINE bool operator!=(const RHITextureUAVInfo& other) const {
//        return !(*this == other);
//    }
//    friend uint32_t GetHash(const RHITextureUAVInfo& value){
//        uint32_t hash = (uint32_t)value.format | (uint32_t)value.mip_level << 8 | (uint32_t)value.array_index << 16;
//        hash_combine(hash, value.dimension_override.has_value() ? (uint32_t)(value.dimension_override.value()) : std::numeric_limits<uint32_t>::max());
//        hash_combine(hash, (uint32_t)value.array_size | (uint32_t)value.meta_plane << 16);
//        return hash;
//    }
//};
//
//struct RHITextureUAVCreateInfo : public RHITextureUAVInfo{
//    RHITextureUAVCreateInfo(uint8_t _mip_level = 0, EPixelFormat _format = PF_UNDEFINED, uint16_t _array_index = 0, uint16_t _array_size = 0):
//                                    RHITextureUAVInfo(_mip_level, _format, _array_index, _array_size){}
//    RHITextureUAVCreateInfo Create(uint8_t _mip_level = 0, EPixelFormat _format = PF_UNDEFINED, uint16_t _array_index = 0, uint16_t _array_size = 0){
//        return {_mip_level, _format, _array_index, _array_size};
//    }
//
//    RHITextureUAVCreateInfo& SetFormat(EPixelFormat _format) {
//        format = _format;
//        return *this;
//    }
//    RHITextureUAVCreateInfo& SetMipLevel(uint8_t _mip_level) {
//        mip_level = _mip_level;
//        return *this;
//    }
//    RHITextureUAVCreateInfo& SetArrayIndex(uint8_t _array_index) {
//        array_index = _array_index;
//        return *this;
//    }
//
//    RHITextureUAVCreateInfo& SetArrayCount(uint8_t _array_count) {
//        _array_count = _array_count;
//        return *this;
//    }
//
//    RHITextureUAVCreateInfo& SetMetaTexturePlane(ERHITexturePlane _meta_plane){
//        meta_plane = _meta_plane;
//        return *this;
//    }
//    RHITextureUAVCreateInfo& SetDimensionOverride(ETextureDimension _dimension_override){
//        dimension_override = _dimension_override;
//        return *this;
//    }
//};
//
//struct RHIBufferSRVInfo {
//    explicit RHIBufferSRVInfo() = default;
//    explicit RHIBufferSRVInfo(EPixelFormat _format) : format(_format) {}
//    explicit RHIBufferSRVInfo(uint32_t _byte_offset, uint32_t _num_elements) : byte_offset(_byte_offset), num_elements(_num_elements) {}
//    bool operator==(const RHIBufferSRVInfo& other) const {
//        return format == other.format && byte_offset == other.byte_offset && num_elements == other.num_elements;
//    }
//    bool operator!=(const RHIBufferSRVInfo& other) const {
//        return !(*this == other);
//    }
//    EPixelFormat format;
//    uint32_t     byte_offset;
//    uint32_t     num_elements;
//};
//struct RHIBufferUAVInfo{
//    explicit RHIBufferUAVInfo(EPixelFormat _format): format(_format){}
//    EPixelFormat format;
//    uint32_t byte_offset;
//    uint32_t num_elements;
//
//};
//struct RHIBufferSRVCreateInfo : RHIBufferSRVInfo {
//    RHIBufferSRVCreateInfo() = default;
//    RHIBufferSRVCreateInfo(EPixelFormat _format) : RHIBufferSRVInfo(_format) {}
//
//    static RHIBufferSRVCreateInfo Create(EPixelFormat _format){
//        return {_format};
//    }
//    RHIBufferSRVCreateInfo& SetFormat(EPixelFormat _format) {
//        format = _format;
//        return *this;
//    }
//    RHIBufferSRVCreateInfo& SetByteOffset(uint32_t _byte_offset) {
//        byte_offset = _byte_offset;
//        return *this;
//    }
//    RHIBufferSRVCreateInfo& SetNumElements(uint32_t _num_elements) {
//        num_elements = _num_elements;
//        return *this;
//    }
//};
#pragma endregion

class RHITextureReference final : public RHITexture {
public:
    RENDER_API RHITextureReference(RHITexture* _texture, RHIShaderResourceView* _bindless_view);

    RENDER_API ~RHITextureReference();

    RENDER_API virtual class RHITextureReference* GetTextureRef() override;
    //    RENDER_API virtual RHIDescriptorHandle GetDefaultBindlessHandle() const override;

    RENDER_API virtual void*                 GetNativeResource() const override;
    RENDER_API virtual void*                 GetNativeShaderResourceView() const override;
    RENDER_API virtual const RHITextureInfo& GetInfo() const override;

    inline RHITexture*            GetReferencedTexture() const { return texture_ref.Get(); }
    inline RHIShaderResourceView* GetBindlessView() const { return bindless_view.Get(); }

    static inline RHITexture* GetDefaultTexture() { return default_texture; }

private:
    void SetReferencedTexture(RHITexture* _texture_ref) {
        texture_ref = _texture_ref;
    }

    RHITextureRef texture_ref;

    RHIShaderResourceViewRef bindless_view;

    RENDER_API static RHITextureRef default_texture;
};

class RHIShaderLibrary : public RHIResource {
public:
    RHIShaderLibrary(EShaderPlatform _platform, std::string const& _name)
        : RHIResource(RRT_SHADER_LIBRARY),
          platform(_platform),
          library_name(_name),
          library_id(GetHash(_name)) {}
    virtual ~RHIShaderLibrary() = default;

    FORCEINLINE EShaderPlatform GetPlatform() const { return platform; }
    FORCEINLINE const std::string& GetName() const { return library_name; }
    FORCEINLINE uint32_t           GetId() const { return library_id; }

    virtual bool       IsNativeLibrary() const                                    = 0;
    virtual int32_t    GetNumShaderMaps() const                                   = 0;
    virtual int32_t    GetNumShaders() const                                      = 0;
    virtual int32_t    GetNumShadersForShaderMap(int32_t ShaderMapIndex) const    = 0;
    virtual int32_t    GetShaderIndex(int32_t ShaderMapIndex, int32_t i) const    = 0;
    virtual SHA256Hash GetShaderHash(int32_t ShaderMapIndex, int32_t ShaderIndex) = 0;
    virtual int32_t    FindShaderMapIndex(const SHA256Hash& Hash)                 = 0;
    virtual int32_t    FindShaderIndex(const SHA256Hash& Hash)                    = 0;
    //    virtual bool PreloadShader(int32_t ShaderIndex, FGraphEventArray& OutCompletionEvents) { return false; }
    //    virtual bool PreloadShaderMap(int32_t ShaderMapIndex, FGraphEventArray& OutCompletionEvents) { return false; }
    //    virtual bool PreloadShaderMap(int32_t ShaderMapIndex, FCoreDelegates::FAttachShaderReadRequestFunc AttachShaderReadRequestFunc) { return false; }
    virtual void ReleasePreloadedShader(int32_t ShaderIndex) {}

    virtual CountableRef<RHIShader> CreateShader(int32_t ShaderIndex) { return nullptr; }
    virtual void                    Teardown(){};

protected:
    EShaderPlatform platform;
    std::string     library_name;
    uint32_t        library_id;
};

class RHIPipelineBinaryDataLibrary : public RHIResource {
public:
    RHIPipelineBinaryDataLibrary(EShaderPlatform InPlatform, std::string const& FilePath) : RHIResource(RRT_PIPELINE_BINARY_DATA_LIBRARY), platform(InPlatform) {}
    virtual ~RHIPipelineBinaryDataLibrary() = default;

    FORCEINLINE EShaderPlatform GetPlatform() const { return platform; }

protected:
    EShaderPlatform platform;
};

#endif// !RHI_RESOURCE_H
