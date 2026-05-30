#pragma once

#include "RenderAPI.h"
#include "RenderGraphResourcePool.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "string/String.h"

#include <cstdint>
#include <limits>
#include <span>

namespace Moer::Render {

using RHICommandList = Render::CommandList;
using RGTextureDesc  = PooledTextureDesc;
using RGBufferDesc   = PooledBufferDesc;

class RGTexture;
class RGBuffer;

struct RGTextureRange {
    ETextureAspectFlags aspect{ETextureAspectFlags::COLOR};
    uint32_t            mip_min{0};
    uint32_t            mip_count{1};
    uint32_t            array_min{0};
    uint32_t            array_count{1};

    RENDER_API bool Contains(const RGTextureRange& other) const;
    RENDER_API bool Overlaps(const RGTextureRange& other) const;
};

struct RGBufferRange {
    uint64_t offset{0};
    uint64_t size{0};

    RENDER_API bool IsWholeResource() const;
    RENDER_API bool Contains(const RGBufferRange& other) const;
    RENDER_API bool Overlaps(const RGBufferRange& other) const;
};

enum class ERGResourceKind : uint8_t {
    Texture,
    Buffer
};

struct RGTransientResource {
    static constexpr uint32_t invalid_pass = std::numeric_limits<uint32_t>::max();

    bool               enabled{false};
    Render::EQueueType owner_queue{Render::EQueueType::Ignore};
    uint32_t           owner_pass{invalid_pass};

    void ResetCompileState();
    bool HasOwner() const;
    bool NeedsOwnerTransfer(Render::EQueueType next_queue) const;
    void SetOwner(Render::EQueueType queue, uint32_t pass_index);
};

struct RGResourceCompileInfo {
    static constexpr uint32_t invalid_pass = std::numeric_limits<uint32_t>::max();

    uint32_t first_pass{invalid_pass};
    uint32_t last_pass{invalid_pass};
    bool     access_write{false};

    void Reset();
    void RecordAccess(uint32_t pass_index, bool writes);
};

struct RGTextureStateRange {
    RGTextureRange        range{};
    Render::ETextureState state{Render::ETextureState::UNDEFINED};
    Render::EQueueType    queue{Render::EQueueType::Ignore};
    uint32_t              last_pass{RGResourceCompileInfo::invalid_pass};
    bool                  last_write{false};
};

struct RGBufferStateRange {
    RGBufferRange        range{};
    Render::EBufferState state{Render::EBufferState::UNDEFINED};
    Render::EQueueType   queue{Render::EQueueType::Ignore};
    uint32_t             last_pass{RGResourceCompileInfo::invalid_pass};
    bool                 last_write{false};
};

class RGResource {
public:
    static constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();

    RGResource(StringView resource_name, ERGResourceKind resource_kind, bool imported_resource, bool transient_resource);
    virtual ~RGResource() = default;

    uint32_t Index() const {
        return index;
    }

    String                name{};
    ERGResourceKind       kind{ERGResourceKind::Texture};
    uint32_t              index{invalid_index};
    bool                  imported{false};
    bool                  exported{false};
    RGTransientResource   transient{};
    Render::EQueueType    owner_queue{Render::EQueueType::Ignore};
    RGResourceCompileInfo compile{};
};

class RGTexture final : public RGResource {
public:
    RGTexture(StringView name, const RGTextureDesc& desc);
    RGTexture(StringView name, PooledTextureRef texture, bool imported);

    const RGTextureDesc& Desc() const {
        return m_desc;
    }
    const PooledTextureRef& Pooled() const {
        return m_texture;
    }
    const Render::TextureRef& RHI() const {
        return m_texture->RHI();
    }
    bool IsAllocated() const {
        return m_texture && m_texture->IsAllocated();
    }
    void Bind(PooledTextureRef texture);
    void ReleaseTransient();
    Render::TextureView GetView(const RGTextureRange& range = {}) const;

    Render::ETextureState            final_state{Render::ETextureState::UNDEFINED};
    Moer::Array<RGTextureStateRange> state_ranges{};

private:
    RGTextureDesc    m_desc{};
    PooledTextureRef m_texture{};
};

class RGBuffer final : public RGResource {
public:
    RGBuffer(StringView name, const RGBufferDesc& desc);
    RGBuffer(StringView name, PooledBufferRef buffer, bool imported);

    const RGBufferDesc& Desc() const {
        return m_desc;
    }
    const PooledBufferRef& Pooled() const {
        return m_buffer;
    }
    const Render::BufferRef& RHI() const {
        return m_buffer->RHI();
    }
    bool IsAllocated() const {
        return m_buffer && m_buffer->IsAllocated();
    }
    void Bind(PooledBufferRef buffer);
    void ReleaseTransient();
    Render::BufferView GetView(const RGBufferRange& range = {}) const;

    Render::EBufferState            final_state{Render::EBufferState::UNDEFINED};
    Moer::Array<RGBufferStateRange> state_ranges{};

private:
    RGBufferDesc    m_desc{};
    PooledBufferRef m_buffer{};
};

RENDER_API bool RGTextureStateWrites(Render::ETextureState state);
RENDER_API bool RGBufferStateWrites(Render::EBufferState state);

struct RGTextureAccess {
    RGTexture*            texture{nullptr};
    RGTextureRange        range{};
    Render::ETextureState state{Render::ETextureState::SHADER_RESOURCE};
    Render::EQueueType    queue{Render::EQueueType::Ignore};

    RGTextureAccess() = default;
    RGTextureAccess(
        RGTexture*            target,
        RGTextureRange        target_range,
        Render::ETextureState target_state,
        Render::EQueueType    target_queue
    )
        : texture(target), range(target_range), state(target_state), queue(target_queue) {}
};

struct RGBufferAccess {
    RGBuffer*            buffer{nullptr};
    RGBufferRange        range{};
    Render::EBufferState state{Render::EBufferState::SHADER_RESOURCE};
    Render::EQueueType   queue{Render::EQueueType::Ignore};

    RGBufferAccess() = default;
    RGBufferAccess(
        RGBuffer*            target,
        RGBufferRange        target_range,
        Render::EBufferState target_state,
        Render::EQueueType   target_queue
    )
        : buffer(target), range(target_range), state(target_state), queue(target_queue) {}
};

struct RGTextureView {
    RGTexture*     texture{nullptr};
    RGTextureRange range{};
};

struct RGBufferView {
    RGBuffer*     buffer{nullptr};
    RGBufferRange range{};
};

template<Render::ETextureState State>
struct RGTextureStaticAccess {
    RGTextureView view{};

    RGTextureStaticAccess& operator=(const RGTextureView& resource_view) {
        view = resource_view;
        return *this;
    }

    operator RGTextureView&() {
        return view;
    }
    operator const RGTextureView&() const {
        return view;
    }

    RGTextureAccess ToAccess(Render::EQueueType queue) const {
        return RGTextureAccess{view.texture, view.range, State, queue};
    }
};

template<Render::EBufferState State>
struct RGBufferStaticAccess {
    RGBufferView view{};

    RGBufferStaticAccess& operator=(const RGBufferView& resource_view) {
        view = resource_view;
        return *this;
    }

    operator RGBufferView&() {
        return view;
    }
    operator const RGBufferView&() const {
        return view;
    }

    RGBufferAccess ToAccess(Render::EQueueType queue) const {
        return RGBufferAccess{view.buffer, view.range, State, queue};
    }
};

struct RGTextureArrayAccess {
    RGTextureView         view{};
    Render::ETextureState state{Render::ETextureState::UNDEFINED};

    RGTextureAccess ToAccess(Render::EQueueType queue) const {
        return RGTextureAccess{view.texture, view.range, state, queue};
    }
};

struct RGBufferArrayAccess {
    RGBufferView         view{};
    Render::EBufferState state{Render::EBufferState::UNDEFINED};

    RGBufferAccess ToAccess(Render::EQueueType queue) const {
        return RGBufferAccess{view.buffer, view.range, state, queue};
    }
};

class RGTextureAccessArray {
public:
    explicit RGTextureAccessArray(uint32_t capacity = 0) {
        m_accesses.reserve(capacity);
    }

    void AddAccess(RGTextureView view, Render::ETextureState state) {
        m_accesses.push_back(RGTextureArrayAccess{view, state});
    }

    std::span<const RGTextureArrayAccess> Accesses() const {
        return std::span<const RGTextureArrayAccess>(m_accesses.data(), m_accesses.size());
    }

    uint32_t Size() const {
        return static_cast<uint32_t>(m_accesses.size());
    }

private:
    Moer::Array<RGTextureArrayAccess> m_accesses{};
};

class RGBufferAccessArray {
public:
    explicit RGBufferAccessArray(uint32_t capacity = 0) {
        m_accesses.reserve(capacity);
    }

    void AddAccess(RGBufferView view, Render::EBufferState state) {
        m_accesses.push_back(RGBufferArrayAccess{view, state});
    }

    std::span<const RGBufferArrayAccess> Accesses() const {
        return std::span<const RGBufferArrayAccess>(m_accesses.data(), m_accesses.size());
    }

    uint32_t Size() const {
        return static_cast<uint32_t>(m_accesses.size());
    }

private:
    Moer::Array<RGBufferArrayAccess> m_accesses{};
};

} // namespace Moer::Render
