#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "string/String.h"

#include <cstdint>
#include <mutex>
#include <span>

namespace Moer::Render {

using PooledTextureDesc = Render::TextureInfo;
using PooledBufferDesc  = Render::BufferInfo;

class PooledTexturePool;
class PooledBufferPool;

class RENDER_API PooledTexture {
public:
    PooledTexture(
        StringView               name,
        const PooledTextureDesc& desc,
        Render::TextureRef       texture,
        PooledTexturePool*       owner,
        bool                     reusable
    );

    StringView Name() const {
        return m_name;
    }
    const PooledTextureDesc& Desc() const {
        return m_desc;
    }
    const Render::TextureRef& RHI() const {
        return m_texture;
    }
    PooledTexturePool* Owner() const {
        return m_owner;
    }
    bool IsAllocated() const {
        return m_texture.Get() != nullptr;
    }
    bool IsReusable() const {
        return m_reusable;
    }

private:
    friend class PooledTexturePool;

    bool CanReuse(const PooledTextureDesc& desc) const;
    void MarkUsed(StringView name);
    bool TickIdle(uint32_t retire_after_idle_frames);

    String             m_name{};
    PooledTextureDesc  m_desc{};
    Render::TextureRef m_texture{};
    PooledTexturePool* m_owner{nullptr};
    uint32_t           m_idle_frames{0};
    bool               m_reusable{false};
};

class RENDER_API PooledBuffer {
public:
    PooledBuffer(
        StringView              name,
        const PooledBufferDesc& desc,
        Render::BufferRef       buffer,
        PooledBufferPool*       owner,
        bool                    reusable
    );

    StringView Name() const {
        return m_name;
    }
    const PooledBufferDesc& Desc() const {
        return m_desc;
    }
    const Render::BufferRef& RHI() const {
        return m_buffer;
    }
    PooledBufferPool* Owner() const {
        return m_owner;
    }
    bool IsAllocated() const {
        return m_buffer.Get() != nullptr;
    }
    bool IsReusable() const {
        return m_reusable;
    }

private:
    friend class PooledBufferPool;

    bool CanReuse(const PooledBufferDesc& desc) const;
    void MarkUsed(StringView name);
    bool TickIdle(uint32_t retire_after_idle_frames);

    String            m_name{};
    PooledBufferDesc  m_desc{};
    Render::BufferRef m_buffer{};
    PooledBufferPool* m_owner{nullptr};
    uint32_t          m_idle_frames{0};
    bool              m_reusable{false};
};

using PooledTextureRef = SharedPtr<PooledTexture>;
using PooledBufferRef  = SharedPtr<PooledBuffer>;

struct PooledTextureAllocationRequest {
    String            name{};
    PooledTextureDesc desc{};
};

struct PooledTextureAllocationResult {
    PooledTextureRef texture{};
};

struct PooledBufferAllocationRequest {
    String           name{};
    PooledBufferDesc desc{};
};

struct PooledBufferAllocationResult {
    PooledBufferRef buffer{};
};

class RENDER_API PooledTexturePool {
public:
    explicit PooledTexturePool(uint32_t retire_after_idle_frames = 3);

    static PooledTexturePool& Global();

    PooledTextureRef RegisterExternal(StringView name, Render::TextureRef texture);
    PooledTextureRef Allocate(StringView name, const PooledTextureDesc& desc);
    Moer::Array<PooledTextureAllocationResult>
             AllocateBatch(std::span<const PooledTextureAllocationRequest> requests);
    void     Tick();
    void     Reset();
    uint32_t LiveCount() const;

private:
    PooledTextureRef AllocateLocked(StringView name, const PooledTextureDesc& desc);

    mutable std::mutex            m_mutex{};
    Moer::Array<PooledTextureRef> m_resources{};
    uint32_t                      m_retire_after_idle_frames{3};
};

class RENDER_API PooledBufferPool {
public:
    explicit PooledBufferPool(uint32_t retire_after_idle_frames = 3);

    static PooledBufferPool& Global();

    PooledBufferRef RegisterExternal(StringView name, Render::BufferRef buffer);
    PooledBufferRef Allocate(StringView name, const PooledBufferDesc& desc);
    Moer::Array<PooledBufferAllocationResult>
             AllocateBatch(std::span<const PooledBufferAllocationRequest> requests);
    void     Tick();
    void     Reset();
    uint32_t LiveCount() const;

private:
    PooledBufferRef AllocateLocked(StringView name, const PooledBufferDesc& desc);

    mutable std::mutex           m_mutex{};
    Moer::Array<PooledBufferRef> m_resources{};
    uint32_t                     m_retire_after_idle_frames{3};
};

} // namespace Moer::Render
