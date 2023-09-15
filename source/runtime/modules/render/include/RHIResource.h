#ifndef RHI_RESOURCE_H
#define RHI_RESOURCE_H
#include "RHICommon.h"
#include "API_Macro.h"
#include "RHIResourceInitilizer.h"
#include "PixelFormat.h"
#include <cassert>
#include <atomic>
#include <misc/StatQueue.h>
#include <unordered_set>
#include <string>

class RHI_API RHIResource {
public:
    explicit RHIResource(ERHIResourceType _type = ERHIResourceType::RRT_NONE) : type(_type) {}
    virtual ~RHIResource() = default;

public:
    int32_t AddRef() {
        return m_counter.fetch_add(1) + 1;
    };
    virtual void Destroy() = 0;
    int32_t      DeRef() {
        assert(m_counter >= 0);
        int32_t current = m_counter.fetch_sub(1);
        if (current == 1) {
            Destroy();
        }
        return current - 1;
    };
    int32_t GetRefCount() { return m_counter.load(); }

protected:
    std::atomic<int32_t> m_counter;

private:
    struct ResourceAtomicFlags {
        std::atomic_int32_t ref_count;
        std::atomic_bool    b_pending_deleting;

    public:
        int32_t AddRef(std::memory_order memory_order) {
            return ref_count.fetch_add(1, memory_order) + 1;
        }
        int32_t DeRef(std::memory_order memory_order) {
            return ref_count.fetch_sub(1, memory_order) - 1;
        }
        bool MarkToDelete(std::memory_order memory_order) {
            return b_pending_deleting.exchange(true, memory_order);
        }
        bool UnMarkToDelete(std::memory_order memory_order) {
            return b_pending_deleting.exchange(false, memory_order);
        }
        bool IsDeleteing() {
            assert(b_pending_deleting.load(std::memory_order_relaxed) == 1);
            if (ref_count.load(std::memory_order_acquire) != 0) {
                return true;
            }
            b_pending_deleting.exchange(false, std::memory_order_release);
            return false;
        }
        bool IsValid(std::memory_order memory_order) {
            return !b_pending_deleting.load(memory_order) && ref_count.load(memory_order) > 0;
        }
        int32_t GetRefCount(std::memory_order memory_order) {
            std::unordered_set<uint32_t> s;
            s.count(1);
            return ref_count.load(memory_order);
        }
    };
    ERHIResourceType type;
    //for const resource state change
    mutable ResourceAtomicFlags                      flags;
    static std::atomic<StatMPSCQueue<RHIResource*>*> pending_deletings;
};

class RHISampler : public RHIResource {
public:
    explicit RHISampler() : RHIResource(RRT_SAMPLER) {}
};

class RHIRasterizationState : public RHIResource {
public:
    explicit RHIRasterizationState() : RHIResource(RRT_RASTERIZE_STATE) {}
    virtual bool GetInitializer(struct RHIRasterizationStateInitializer& _init) { return false; }
};

class RHIDepthStencilState : public RHIResource {
public:
    explicit RHIDepthStencilState() : RHIResource(RRT_DEPTH_STENCIL_STATE) {}
    virtual bool GetInitializer(struct RHIDepthStencilStateInitializer& _init) { return false; }
};

class RHIBlendState : public RHIResource {
public:
    explicit RHIBlendState() : RHIResource(RRT_BLEND_STATE) {}
    virtual bool GetInitializer(struct RHIBlendStateInitializer& _init) { return false; }
};

class RHIVertexDescription : public RHIResource {
public:
    explicit RHIVertexDescription() : RHIResource(RRT_VERTEX_DESCRIPTION) {}
};

class RHIPipelineShaderBindingState : public RHIResource {
public:
    explicit RHIPipelineShaderBindingState() : RHIResource(RRT_PIPELINE_BOUND_SHADER_STATE) {}
};

#pragma region shader resources
class RHIShader : public RHIResource {
public:
    RHIShader() = delete;
    RHIShader(ERHIResourceType _type, EShaderType _shader_type) : RHIResource(_type), shader_type(_shader_type) {}
    FORCEINLINE EShaderType GetShaderType() const {
        return shader_type;
    }

    void        SetHash(const SHA256Hash& _hash) { hash = _hash; }
    SHA256Hash  GetHash() const { return hash; }
    EShaderType shader_type;
    SHA256Hash  hash;
};

class RHIVertexShader : public RHIShader {
public:
    RHIVertexShader() : RHIShader(RRT_VERTEX_SHADER, ST_VERTEX) {}
};

class RHIFragmentShader : public RHIShader {
public:
    RHIFragmentShader() : RHIShader(RRT_FRAGMENT_SHADER, ST_FRAGMENT) {}
};

class RHIGeometryShader : public RHIShader {
public:
    RHIGeometryShader() : RHIShader(RRT_GEOMETRY_SHADER, ST_GEOMETRY) {}
};

class RHIComputeShader : public RHIShader {
public:
    RHIComputeShader() : RHIShader(RRT_COMPUTE_SHADER, ST_COMPUTE) {}
};

class RHIMeshShader : public RHIShader {
public:
    RHIMeshShader() : RHIShader(RRT_MESH_SHADER, ST_MESH) {}
};
class RHIAmplificationShader : public RHIShader {
public:
    RHIAmplificationShader() : RHIShader(RRT_AMPLIFICATION_SHADER, ST_AMPLIFICATION) {}
};
class RHIRayGenShader : public RHIShader {
public:
    RHIRayGenShader() : RHIShader(RRT_RAY_TRACING_SHADER, ST_RAY_GEN) {}
};
class RHIRayHitShader : public RHIShader {
public:
    RHIRayHitShader() : RHIShader(RRT_RAY_TRACING_SHADER, ST_RAY_HIT) {}
};
class RHIRayMissShader : public RHIShader {
public:
    RHIRayMissShader() : RHIShader(RRT_RAY_TRACING_SHADER, ST_RAY_MISS) {}
};
#pragma endregion

#pragma region pipeline

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

class RHIUniformBufferLayout : public RHIResource {
public:
    RHIUniformBufferLayout() : RHIResource(RRT_UNIFORM_BUFFER_LAYOUT) {}
};

class RHIUniformBuffer : public RHIResource {
public:
    RHIUniformBuffer() : RHIResource(RRT_UNIFORM_BUFFER) {}
    explicit RHIUniformBuffer(const RHIUniformBufferLayout& _layout){};
};

struct RHIBufferInfo {
    uint32_t          size;
    uint32_t          stride;
    EBufferUsageFlags flags;

    RHIBufferInfo() = default;
    RHIBufferInfo(uint32_t _size, uint32_t _stride, EBufferUsageFlags _flags)
        : size(_size),
          stride(_stride),
          flags(_flags) {}
};
/* index, vertex, staging, indirect */
class RHIBuffer : public RHIResource {
public:
    RHIBuffer(const RHIBufferInfo& _info) : RHIResource(RRT_BUFFER), info(_info) {}

    const RHIBufferInfo& GetDesc() const { return info; }
    std::string          GetName() const { return name; }
    void                 SetName(const std::string& _name) {
        name = _name;
    }

protected:
    std::string name;

private:
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
        int2               _extent,
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

    ETextureUsageFlags usage = ETextureUsageFlags::NONE;

    ETextureLayout layout = TEXTURE_LAYOUT_UNDEFINED;

    int2 extent = int2(1, 1);

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
        hash_combine(hash, GetHash(target.format));
        hash_combine(hash, GetHash(target.array_size));
        hash_combine(hash, GetHash(target.usage));
        hash_combine(hash, GetHash(target.layout));
        hash_combine(hash, GetHash(target.extent));
        hash_combine(hash, GetHash(target.depth));
        hash_combine(hash, GetHash(target.uav_format));
        hash_combine(hash, GetHash(target.num_mips));
        hash_combine(hash, GetHash(target.num_samples));
        hash_combine(hash, GetHash(target.clear_attachment));
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
    static RHITextureCreateInfo Create2D(const char* _name, int2 _size, EPixelFormat _format){
        return Create2D(_name).}

    RHITextureCreateInfo& SetUsageFlags(ETextureUsageFlags _usage) {
        usage = _usage;
        return *this;
    }
    RHITextureCreateInfo& AddUsageFlags(ETextureUsageFlags _usage) {
        usage |= _usage;
        return *this;
    }
    RHITextureCreateInfo& SetClearAttachment(RHIClearAttachment& _attachment) {
        clear_attachment = _attachment;
        return *this;
    }
    RHITextureCreateInfo& SetExtent(const int2 _extent) {
        extent = _extent;
        return *this;
    }
    RHITextureCreateInfo& SetExtent(int32_t _x, int32_t _y) {
        extent = int2(_x, _y);
        return *this;
    }
    RHITextureCreateInfo& SetExtent(uint32_t _extent) {
        extent = int2(_extent);
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

class RHITexture : public RHIResource {
public:
    RHITexture(const RHITextureCreateInfo& _info) : texture_info(_info) {
        SetName(_info.name);
    }

    virtual const RHITextureInfo& GetInfo() const { return texture_info; }

    virtual class RHITextureRef* GetTextureRef() { return nullptr; }

    virtual void* GetNativeResource() const { return nullptr; }

    int3 GetExtent3D() const {
        const RHITextureInfo& info = GetInfo();
        switch (info.dimension) {
            case ETextureDimension::TEX_2D: return {info.extent.x, info.extent.y, 1};
            case ETextureDimension::TEX_2D_ARRAY: return {info.extent.x, info.extent.y, info.array_size};
            case ETextureDimension::TEX_3D: return {info.extent.x, info.extent.y, info.depth};
            case ETextureDimension::TEX_CUBE: return {info.extent.x, info.extent.y, 1};
            case ETextureDimension::TEX_CUBE_ARRAY: return {info.extent.x, info.extent.y, info.array_size};
        }
        return {0, 0, 0};
    }
    void SetName(const std::string _name) {
        name = _name;
    }

private:
    friend class RHITextureRef;
    explicit RHITexture(ERHIResourceType _type) : RHIResource(_type) {}
    RHITextureInfo texture_info;
    std::string    name;
};

#pragma endregion
#endif// !RHI_RESOURCE_H
