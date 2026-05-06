#pragma once

#include "RenderAPI.h"
#include "RenderGraphHandle.h"
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

struct RGTextureRange {
    ETextureAspectFlags aspect{ETextureAspectFlags::COLOR};
    uint32_t            mip_min{0};
    uint32_t            mip_count{1};
    uint32_t            array_min{0};
    uint32_t            array_count{1};

    // Overlaps when aspect flags intersect and mip/array intervals both intersect.
    RENDER_API bool Contains(const RGTextureRange& other) const;
    RENDER_API bool Overlaps(const RGTextureRange& other) const;
};

struct RGBufferRange {
    uint64_t offset{0};
    uint64_t size{0};

    // {0, 0} represents the whole buffer.
    RENDER_API bool IsWholeResource() const;
    RENDER_API bool Contains(const RGBufferRange& other) const;
    // Whole-buffer ranges overlap every valid range; partial ranges overlap when byte intervals intersect.
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

struct RGTextureAccess {
    RenderGraphHandle     handle{};
    RGTextureRange        range{};
    Render::ETextureState state{Render::ETextureState::SHADER_RESOURCE};
    Render::EQueueType    queue{Render::EQueueType::Ignore};
};

struct RGBufferAccess {
    RenderGraphHandle    handle{};
    RGBufferRange        range{};
    Render::EBufferState state{Render::EBufferState::SHADER_RESOURCE};
    Render::EQueueType   queue{Render::EQueueType::Ignore};
};

struct RGTextureView {
    RenderGraphHandle handle{};
    RGTextureRange    range{};
};

struct RGBufferView {
    RenderGraphHandle handle{};
    RGBufferRange     range{};
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
        return RGTextureAccess{view.handle, view.range, State, queue};
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
        return RGBufferAccess{view.handle, view.range, State, queue};
    }
};

struct RGTextureArrayAccess {
    RGTextureView         view{};
    Render::ETextureState state{Render::ETextureState::UNDEFINED};

    RGTextureAccess ToAccess(Render::EQueueType queue) const {
        return RGTextureAccess{view.handle, view.range, state, queue};
    }
};

struct RGBufferArrayAccess {
    RGBufferView         view{};
    Render::EBufferState state{Render::EBufferState::UNDEFINED};

    RGBufferAccess ToAccess(Render::EQueueType queue) const {
        return RGBufferAccess{view.handle, view.range, state, queue};
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

class RGTexture {
public:
    RGTexture(StringView name, PooledTextureRef texture, bool registered);

    StringView Name() const {
        return m_name;
    }
    const RGTextureDesc& Desc() const {
        return m_desc;
    }
    const PooledTextureRef& Pooled() const {
        return m_texture;
    }
    const Render::TextureRef& RHI() const {
        return m_texture->RHI();
    }
    bool IsRegistered() const {
        return m_registered;
    }
    bool IsAllocated() const {
        return m_texture && m_texture->IsAllocated();
    }
    Render::TextureView GetView(const RGTextureRange& range = {}) const;

private:
    String           m_name{};
    RGTextureDesc    m_desc{};
    PooledTextureRef m_texture{};
    bool             m_registered{false};
};

class RGBuffer {
public:
    RGBuffer(StringView name, PooledBufferRef buffer, bool registered);

    StringView Name() const {
        return m_name;
    }
    const RGBufferDesc& Desc() const {
        return m_desc;
    }
    const PooledBufferRef& Pooled() const {
        return m_buffer;
    }
    const Render::BufferRef& RHI() const {
        return m_buffer->RHI();
    }
    bool IsRegistered() const {
        return m_registered;
    }
    bool IsAllocated() const {
        return m_buffer && m_buffer->IsAllocated();
    }
    Render::BufferView GetView(const RGBufferRange& range = {}) const;

private:
    String          m_name{};
    RGBufferDesc    m_desc{};
    PooledBufferRef m_buffer{};
    bool            m_registered{false};
};

using RGTextureRef = SharedPtr<RGTexture>;
using RGBufferRef  = SharedPtr<RGBuffer>;

struct RGResource {
    String                name{};
    ERGResourceKind       kind{ERGResourceKind::Texture};
    bool                  imported{false};
    bool                  exported{false};
    RGTransientResource   transient{};
    RGTextureDesc         texture_desc{};
    RGBufferDesc          buffer_desc{};
    RGTextureRef          texture{};
    RGBufferRef           buffer{};
    Render::EQueueType    owner_queue{Render::EQueueType::Ignore};
    Render::ETextureState final_texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  final_buffer_state{Render::EBufferState::UNDEFINED};
    RGResourceCompileInfo compile{};
};

RENDER_API bool RGTextureStateWrites(Render::ETextureState state);
RENDER_API bool RGBufferStateWrites(Render::EBufferState state);
RENDER_API bool RGTextureStateConflicts(Render::ETextureState lhs, Render::ETextureState rhs);
RENDER_API bool RGBufferStateConflicts(Render::EBufferState lhs, Render::EBufferState rhs);

} // namespace Moer::Render
