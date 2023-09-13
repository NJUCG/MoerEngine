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

class RHIPipelineShaderBindingState : public RHIResource {
public:
    explicit RHIPipelineShaderBindingState() : RHIResource(RRT_PIPELINE_BOUND_SHADER_STATE) {}
};

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

#pragma region pipeline

class RHIGraphicsPipelineState : public RHIResource {
public:
    RHIGraphicsPipelineState() : RHIResource(RRT_GRAPHIC_PIPELINE_STATE) {}
};

class RHIComputePipelineState : public RHIResource {
public:
    RHIComputePipelineState() : RHIResource(RRT_COMPUTE_PIPELINE_STATE) {}
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
    RHIUniformBuffer(const RHIUniformBufferLayout& _layout){};
};

/* index, vertex, staging, indirect */
class RHIBuffer : public RHIResource {
public:
    RHIBuffer() : RHIResource(RRT_BUFFER) {}
    RHIBuffer(uint32_t _size, EBufferUsageFlags _usage, uint32_t _stride)
        : RHIResource(RRT_BUFFER),
          size(_size),
          usage(_usage),
          stride(_stride) {}

    EBufferUsageFlags GetUsage() const { return usage; }
    uint32_t          GetSize() const { return size; }
    uint32_t          GetStride() const { return stride; }
    std::string       GetName() const { return name; }
    void              SetName(const std::string& _name) {
        name = _name;
    }

protected:
    EBufferUsageFlags usage;
    uint32_t          size;
    uint32_t          stride;
    std::string       name;
};

class RHITexture: public RHIResource{
public:
    RHITexture(ERHIResourceType _type,
               uint32_t _num_mips,
               uint32_t _num_samples,
               ETextureCreateFlags _flags,
               const RHIClearAttachment& _clear_attachment)
        : RHIResource(_type),
    num_mips(_num_mips),
    num_samples(_num_samples),
    create_flags(_flags),
    clear_attachment(_clear_attachment){}


    std::string       GetName() const { return name; }
    void              SetName(const std::string& _name) {
        name = _name;
    }
protected:
    ETextureCreateFlags create_flags;
    RHIClearAttachment clear_attachment;
    uint32_t num_mips;
    uint32_t num_samples;
    std::string name;

};
#pragma endregion
#endif// !RHI_RESOURCE_H
