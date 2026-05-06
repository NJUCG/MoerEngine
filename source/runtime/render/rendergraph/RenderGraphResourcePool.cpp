#include "rendergraph/RenderGraphResourcePool.h"

#include "misc/Assert.h"
#include "rhi/RHI.h"

#include <cassert>
#include <utility>

namespace Moer::Render {
namespace {

PooledTextureDesc MakeTextureDesc(Render::Texture& texture) {
    return texture.GetInfo();
}

PooledBufferDesc MakeBufferDesc(Render::Buffer& buffer) {
    return buffer.GetInfo();
}

} // namespace

PooledTexture::PooledTexture(
    StringView               name,
    const PooledTextureDesc& desc,
    Render::TextureRef       texture,
    PooledTexturePool*       owner,
    bool                     reusable
) :
    m_name(name),
    m_desc(desc),
    m_texture(std::move(texture)),
    m_owner(owner),
    m_reusable(reusable) {}

bool PooledTexture::CanReuse(const PooledTextureDesc& desc) const {
    return m_reusable && m_texture.Get() != nullptr && m_desc == desc;
}

void PooledTexture::MarkUsed(StringView name) {
    m_name        = String(name);
    m_idle_frames = 0;
}

bool PooledTexture::TickIdle(uint32_t retire_after_idle_frames) {
    if (++m_idle_frames <= retire_after_idle_frames) {
        return false;
    }
    m_texture = {};
    return true;
}

PooledBuffer::PooledBuffer(
    StringView              name,
    const PooledBufferDesc& desc,
    Render::BufferRef       buffer,
    PooledBufferPool*       owner,
    bool                    reusable
) :
    m_name(name),
    m_desc(desc),
    m_buffer(std::move(buffer)),
    m_owner(owner),
    m_reusable(reusable) {}

bool PooledBuffer::CanReuse(const PooledBufferDesc& desc) const {
    return m_reusable && m_buffer.Get() != nullptr && m_desc == desc;
}

void PooledBuffer::MarkUsed(StringView name) {
    m_name        = String(name);
    m_idle_frames = 0;
}

bool PooledBuffer::TickIdle(uint32_t retire_after_idle_frames) {
    if (++m_idle_frames <= retire_after_idle_frames) {
        return false;
    }
    m_buffer = {};
    return true;
}

PooledTexturePool::PooledTexturePool(uint32_t retire_after_idle_frames) :
    m_retire_after_idle_frames(retire_after_idle_frames) {}

PooledTexturePool& PooledTexturePool::Global() {
    static PooledTexturePool pool{};
    return pool;
}

PooledTextureRef PooledTexturePool::RegisterExternal(StringView name, Render::TextureRef texture) {
    assert(texture.Get() != nullptr);
    return MakeShared<PooledTexture>(name, MakeTextureDesc(*texture.Get()), std::move(texture), this, false);
}

PooledTextureRef PooledTexturePool::Allocate(StringView name, const PooledTextureDesc& desc) {
    std::lock_guard lock(m_mutex);
    return AllocateLocked(name, desc);
}

Moer::Array<PooledTextureAllocationResult>
PooledTexturePool::AllocateBatch(std::span<const PooledTextureAllocationRequest> requests) {
    Moer::Array<PooledTextureAllocationResult> results{};
    results.reserve(requests.size());
    if (requests.empty()) {
        return results;
    }

    std::lock_guard lock(m_mutex);
    for (const PooledTextureAllocationRequest& request : requests) {
        results.push_back(PooledTextureAllocationResult{AllocateLocked(request.name, request.desc)});
    }
    return results;
}

void PooledTexturePool::Tick() {
    std::lock_guard lock(m_mutex);
    for (auto it = m_resources.begin(); it != m_resources.end();) {
        PooledTextureRef& resource = *it;
        if (!resource || !resource->IsReusable()) {
            it = m_resources.erase(it);
            continue;
        }
        if (resource.use_count() == 1) {
            if (resource->TickIdle(m_retire_after_idle_frames)) {
                it = m_resources.erase(it);
                continue;
            }
        } else {
            resource->m_idle_frames = 0;
        }
        ++it;
    }
}

void PooledTexturePool::Reset() {
    std::lock_guard lock(m_mutex);
    m_resources.clear();
}

uint32_t PooledTexturePool::LiveCount() const {
    std::lock_guard lock(m_mutex);
    return static_cast<uint32_t>(m_resources.size());
}

PooledTextureRef PooledTexturePool::AllocateLocked(StringView name, const PooledTextureDesc& desc) {
    MOER_ASSERT(
        desc.extent.x > 0 && desc.extent.y > 0 && desc.depth > 0,
        "Pooled texture allocation requires a non-zero extent"
    );
    MOER_ASSERT(desc.format != PF_UNDEFINED, "Pooled texture allocation requires a valid pixel format");

    for (PooledTextureRef& resource : m_resources) {
        if (resource.use_count() == 1 && resource->CanReuse(desc)) {
            resource->MarkUsed(name);
            return resource;
        }
    }

    Render::TextureRef texture = Render::RenderDevice::Get().CreateTexture(name, desc);
    PooledTextureRef resource = MakeShared<PooledTexture>(name, desc, std::move(texture), this, true);
    m_resources.push_back(resource);
    return resource;
}

PooledBufferPool::PooledBufferPool(uint32_t retire_after_idle_frames) :
    m_retire_after_idle_frames(retire_after_idle_frames) {}

PooledBufferPool& PooledBufferPool::Global() {
    static PooledBufferPool pool{};
    return pool;
}

PooledBufferRef PooledBufferPool::RegisterExternal(StringView name, Render::BufferRef buffer) {
    assert(buffer.Get() != nullptr);
    return MakeShared<PooledBuffer>(name, MakeBufferDesc(*buffer.Get()), std::move(buffer), this, false);
}

PooledBufferRef PooledBufferPool::Allocate(StringView name, const PooledBufferDesc& desc) {
    std::lock_guard lock(m_mutex);
    return AllocateLocked(name, desc);
}

Moer::Array<PooledBufferAllocationResult>
PooledBufferPool::AllocateBatch(std::span<const PooledBufferAllocationRequest> requests) {
    Moer::Array<PooledBufferAllocationResult> results{};
    results.reserve(requests.size());
    if (requests.empty()) {
        return results;
    }

    std::lock_guard lock(m_mutex);
    for (const PooledBufferAllocationRequest& request : requests) {
        results.push_back(PooledBufferAllocationResult{AllocateLocked(request.name, request.desc)});
    }
    return results;
}

void PooledBufferPool::Tick() {
    std::lock_guard lock(m_mutex);
    for (auto it = m_resources.begin(); it != m_resources.end();) {
        PooledBufferRef& resource = *it;
        if (!resource || !resource->IsReusable()) {
            it = m_resources.erase(it);
            continue;
        }
        if (resource.use_count() == 1) {
            if (resource->TickIdle(m_retire_after_idle_frames)) {
                it = m_resources.erase(it);
                continue;
            }
        } else {
            resource->m_idle_frames = 0;
        }
        ++it;
    }
}

void PooledBufferPool::Reset() {
    std::lock_guard lock(m_mutex);
    m_resources.clear();
}

uint32_t PooledBufferPool::LiveCount() const {
    std::lock_guard lock(m_mutex);
    return static_cast<uint32_t>(m_resources.size());
}

PooledBufferRef PooledBufferPool::AllocateLocked(StringView name, const PooledBufferDesc& desc) {
    MOER_ASSERT(desc.size > 0 && desc.stride > 0, "Pooled buffer allocation requires a non-zero size and stride");

    for (PooledBufferRef& resource : m_resources) {
        if (resource.use_count() == 1 && resource->CanReuse(desc)) {
            resource->MarkUsed(name);
            return resource;
        }
    }

    Render::BufferRef buffer = Render::RenderDevice::Get().CreateBuffer(name, desc);
    PooledBufferRef resource = MakeShared<PooledBuffer>(name, desc, std::move(buffer), this, true);
    m_resources.push_back(resource);
    return resource;
}

} // namespace Moer::Render
