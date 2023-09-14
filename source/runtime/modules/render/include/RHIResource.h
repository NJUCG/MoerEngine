#ifndef RHI_RESOURCE_H
#define RHI_RESOURCE_H
#include "RHICommon.h"
#include "API_Macro.h"
#include "RHIResourceInitilizer.h"
#include <cassert>
#include <atomic>
#include <misc/StatQueue.h>
#include <unordered_set>

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

struct RHITextureInfo{
    RHITextureInfo() = default;
    RHITextureInfo(const RHITextureInfo& other){ *this = other;}

    RHITextureInfo(ETextureDimension _dimension): dimension(_dimension){}

    ETextureDimension dimension;
    ETextureCreateFlags flags;


};

class RHITexture : public RHIResource {
public:
    RHITexture(ERHIResourceType          _type,
               uint32_t                  _num_mips,
               uint32_t                  _num_samples,
               ETextureCreateFlags       _flags,
               const RHIClearAttachment& _clear_attachment)
        : RHIResource(_type),
          num_mips(_num_mips),
          num_samples(_num_samples),
          create_flags(_flags),
          clear_attachment(_clear_attachment) {}

    std::string GetName() const { return name; }
    void        SetName(const std::string& _name) {
        name = _name;
    }

protected:
    ETextureCreateFlags create_flags;
    RHIClearAttachment  clear_attachment;
    uint32_t            num_mips;
    uint32_t            num_samples;
    std::string         name;
};

class RHITexture2D : public RHITexture {
public:
    RHITexture2D(uint32_t                  _num_mips,
                 uint32_t                  _num_samples,
                 ETextureCreateFlags       _flags,
                 const RHIClearAttachment& _clear_attachment)
        : RHITexture(RRT_TEXTURE2D, _num_mips, _num_samples, _flags, _clear_attachment) {}
};

class RHITexture2DArray : public RHITexture {
public:
};

class RHITexture3D : public RHITexture {
};

class RHITextureCube : public RHITexture {
};
#pragma endregion
#endif// !RHI_RESOURCE_H
