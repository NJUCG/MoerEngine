#pragma once

#include "RenderGraph.h"
#include "misc/STL.h"

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Moer::Render {

namespace Detail {

template<RenderGraph::AccessMode Mode>
inline constexpr bool IsRGDeclarableAccessMode =
    Mode == RenderGraph::AccessMode::Read ||
    Mode == RenderGraph::AccessMode::Write ||
    Mode == RenderGraph::AccessMode::ReadWrite;

} // namespace Detail

/** A required typed texture access. Invalid handles deliberately fail closed. */
template<
    RenderGraph::AccessMode   Mode,
    RenderGraph::TextureState State = RenderGraph::TextureState::Automatic>
struct RGTextureAccess {
    static_assert(
        Detail::IsRGDeclarableAccessMode<Mode>,
        "RG texture access mode must be Read, Write, or ReadWrite"
    );

    RenderGraph::TextureHandle resource{};
    RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole();

    RGTextureAccess() = default;
    RGTextureAccess(
        RenderGraph::TextureHandle handle,
        RenderGraph::TextureRange  access_range = RenderGraph::TextureRange::Whole()
    ) : resource(handle), range(access_range) {}

    RGTextureAccess& operator=(RenderGraph::TextureHandle handle) {
        resource = handle;
        return *this;
    }
};

/** A required typed buffer access. Invalid handles deliberately fail closed. */
template<
    RenderGraph::AccessMode  Mode,
    RenderGraph::BufferState State = RenderGraph::BufferState::Automatic>
struct RGBufferAccess {
    static_assert(
        Detail::IsRGDeclarableAccessMode<Mode>,
        "RG buffer access mode must be Read, Write, or ReadWrite"
    );

    RenderGraph::BufferHandle resource{};
    RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole();

    RGBufferAccess() = default;
    RGBufferAccess(
        RenderGraph::BufferHandle handle,
        RenderGraph::BufferRange  access_range = RenderGraph::BufferRange::Whole()
    ) : resource(handle), range(access_range) {}

    RGBufferAccess& operator=(RenderGraph::BufferHandle handle) {
        resource = handle;
        return *this;
    }
};

/** A required typed token access. */
template<RenderGraph::AccessMode Mode>
struct RGTokenAccess {
    static_assert(
        Detail::IsRGDeclarableAccessMode<Mode>,
        "RG token access mode must be Read, Write, or ReadWrite"
    );

    RenderGraph::TokenHandle resource{};

    RGTokenAccess() = default;
    RGTokenAccess(RenderGraph::TokenHandle handle) : resource(handle) {}

    RGTokenAccess& operator=(RenderGraph::TokenHandle handle) {
        resource = handle;
        return *this;
    }
};

/** Explicitly optional access. A disengaged value is the only skipped access. */
template<typename Access>
using RGOptionalAccess = std::optional<Access>;

/** Owning dynamic access list; no caller-owned pointer lifetime is retained. */
template<typename Access>
using RGAccessArray = std::vector<Access>;

/** One heterogeneous texture-array element with explicit mode and state. */
class RGTextureAccessElement {
public:
    [[nodiscard]] static RGTextureAccessElement Read(
        RenderGraph::TextureHandle handle,
        RenderGraph::TextureState  state = RenderGraph::TextureState::Automatic,
        RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::Read,
            state,
            range
        );
    }

    [[nodiscard]] static RGTextureAccessElement Write(
        RenderGraph::TextureHandle handle,
        RenderGraph::TextureState  state = RenderGraph::TextureState::Automatic,
        RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::Write,
            state,
            range
        );
    }

    [[nodiscard]] static RGTextureAccessElement ReadWrite(
        RenderGraph::TextureHandle handle,
        RenderGraph::TextureState  state = RenderGraph::TextureState::Automatic,
        RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::ReadWrite,
            state,
            range
        );
    }

    [[nodiscard]] RenderGraph::TextureHandle Resource() const {
        return resource;
    }
    [[nodiscard]] RenderGraph::AccessMode Mode() const {
        return mode;
    }
    [[nodiscard]] RenderGraph::TextureState State() const {
        return state;
    }
    [[nodiscard]] RenderGraph::TextureRange Range() const {
        return range;
    }

private:
    RGTextureAccessElement(
        RenderGraph::TextureHandle handle,
        RenderGraph::AccessMode    mode,
        RenderGraph::TextureState  state,
        RenderGraph::TextureRange  range
    ) : resource(handle), mode(mode), state(state), range(range) {}

    RenderGraph::TextureHandle resource{};
    RenderGraph::AccessMode    mode = RenderGraph::AccessMode::None;
    RenderGraph::TextureState  state = RenderGraph::TextureState::Automatic;
    RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole();
};

/** One heterogeneous buffer-array element with explicit mode and state. */
class RGBufferAccessElement {
public:
    [[nodiscard]] static RGBufferAccessElement Read(
        RenderGraph::BufferHandle handle,
        RenderGraph::BufferState  state = RenderGraph::BufferState::Automatic,
        RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::Read,
            state,
            range
        );
    }

    [[nodiscard]] static RGBufferAccessElement Write(
        RenderGraph::BufferHandle handle,
        RenderGraph::BufferState  state = RenderGraph::BufferState::Automatic,
        RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::Write,
            state,
            range
        );
    }

    [[nodiscard]] static RGBufferAccessElement ReadWrite(
        RenderGraph::BufferHandle handle,
        RenderGraph::BufferState  state = RenderGraph::BufferState::Automatic,
        RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::ReadWrite,
            state,
            range
        );
    }

    [[nodiscard]] RenderGraph::BufferHandle Resource() const {
        return resource;
    }
    [[nodiscard]] RenderGraph::AccessMode Mode() const {
        return mode;
    }
    [[nodiscard]] RenderGraph::BufferState State() const {
        return state;
    }
    [[nodiscard]] RenderGraph::BufferRange Range() const {
        return range;
    }

private:
    RGBufferAccessElement(
        RenderGraph::BufferHandle handle,
        RenderGraph::AccessMode   mode,
        RenderGraph::BufferState  state,
        RenderGraph::BufferRange  range
    ) : resource(handle), mode(mode), state(state), range(range) {}

    RenderGraph::BufferHandle resource{};
    RenderGraph::AccessMode   mode = RenderGraph::AccessMode::None;
    RenderGraph::BufferState  state = RenderGraph::BufferState::Automatic;
    RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole();
};

using RGTextureAccessArray = RGAccessArray<RGTextureAccessElement>;
using RGBufferAccessArray  = RGAccessArray<RGBufferAccessElement>;

/**
 * Adapts parameter declarations to the existing typed PassBuilder API. It
 * intentionally owns no compiler state and performs no access inference.
 */
class RGParameterAccessCollector {
public:
    explicit RGParameterAccessCollector(RenderGraph::PassBuilder& builder) :
        builder(builder) {}

    template<RenderGraph::AccessMode Mode, RenderGraph::TextureState State>
    void Add(const RGTextureAccess<Mode, State>& access) {
        AddTexture(access.resource, Mode, State, access.range);
    }

    template<RenderGraph::AccessMode Mode, RenderGraph::BufferState State>
    void Add(const RGBufferAccess<Mode, State>& access) {
        AddBuffer(access.resource, Mode, State, access.range);
    }

    void Add(const RGTextureAccessElement& access) {
        AddTexture(
            access.Resource(),
            access.Mode(),
            access.State(),
            access.Range()
        );
    }

    void Add(const RGBufferAccessElement& access) {
        AddBuffer(
            access.Resource(),
            access.Mode(),
            access.State(),
            access.Range()
        );
    }

    template<RenderGraph::AccessMode Mode>
    void Add(const RGTokenAccess<Mode>& access) {
        if constexpr (Mode == RenderGraph::AccessMode::Read) {
            builder.Read(access.resource);
        } else if constexpr (Mode == RenderGraph::AccessMode::Write) {
            builder.Write(access.resource);
        } else {
            builder.ReadWrite(access.resource);
        }
    }

    template<RGParameterAccessProvider Parameters>
    void Add(const Parameters& parameters) {
        parameters.DeclareRGAccess(*this);
    }

    template<typename Access>
    void Add(const std::optional<Access>& access) {
        if (access.has_value()) {
            Add(*access);
        }
    }

    template<typename Access, typename Allocator>
    void Add(const std::vector<Access, Allocator>& accesses) {
        for (const Access& access : accesses) {
            Add(access);
        }
    }

private:
    void AddTexture(
        RenderGraph::TextureHandle handle,
        RenderGraph::AccessMode    mode,
        RenderGraph::TextureState  state,
        RenderGraph::TextureRange  range
    ) {
        switch (mode) {
            case RenderGraph::AccessMode::Read:
                if (state == RenderGraph::TextureState::Automatic) {
                    builder.Read(handle, range);
                } else {
                    builder.Read(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::Write:
                if (state == RenderGraph::TextureState::Automatic) {
                    builder.Write(handle, range);
                } else {
                    builder.Write(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::ReadWrite:
                if (state == RenderGraph::TextureState::Automatic) {
                    builder.ReadWrite(handle, range);
                } else {
                    builder.ReadWrite(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::Unknown:
            case RenderGraph::AccessMode::None:
                builder.Read(RenderGraph::TextureHandle{}, range);
                return;
        }
    }

    void AddBuffer(
        RenderGraph::BufferHandle handle,
        RenderGraph::AccessMode   mode,
        RenderGraph::BufferState  state,
        RenderGraph::BufferRange  range
    ) {
        switch (mode) {
            case RenderGraph::AccessMode::Read:
                if (state == RenderGraph::BufferState::Automatic) {
                    builder.Read(handle, range);
                } else {
                    builder.Read(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::Write:
                if (state == RenderGraph::BufferState::Automatic) {
                    builder.Write(handle, range);
                } else {
                    builder.Write(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::ReadWrite:
                if (state == RenderGraph::BufferState::Automatic) {
                    builder.ReadWrite(handle, range);
                } else {
                    builder.ReadWrite(handle, state, range);
                }
                return;
            case RenderGraph::AccessMode::Unknown:
            case RenderGraph::AccessMode::None:
                builder.Read(RenderGraph::BufferHandle{}, range);
                return;
        }
    }

    RenderGraph::PassBuilder& builder;
};

template<typename... Accesses>
void CollectRGParameterAccess(
    RGParameterAccessCollector& collector,
    const Accesses&...           accesses
) {
    (collector.Add(accesses), ...);
}

template<RGParameterAccessProvider Parameters, typename Record>
    requires std::invocable<
        Record&,
        CommandList&,
        const std::remove_cvref_t<Parameters>&>
RenderGraph::PassHandle RenderGraph::AddRecordPass(
    std::string_view name,
    Parameters&&     parameters,
    SetupCallback    policy_setup,
    Record&&         record,
    PassExecutionClass execution,
    uint32_t             workload
) {
    using ParameterType = std::remove_cvref_t<Parameters>;
    using RecordType    = std::decay_t<Record>;

    if constexpr (std::is_pointer_v<RecordType>) {
        if (record == nullptr) {
            return AddRecordPass(
                name,
                policy_setup,
                RecordCallback{},
                execution,
                workload
            );
        }
    } else if constexpr (requires(const RecordType& candidate) {
                             { candidate.operator bool() } -> std::same_as<bool>;
                         }) {
        if (!record.operator bool()) {
            return AddRecordPass(
                name,
                policy_setup,
                RecordCallback{},
                execution,
                workload
            );
        }
    }

    SharedPtr<const ParameterType> parameter_owner =
        MakeShared<ParameterType>(std::forward<Parameters>(parameters));
    auto record_owner = MakeShared<RecordType>(std::forward<Record>(record));

    return AddRecordPass(
        name,
        [parameter_owner, policy_setup = std::move(policy_setup)](
            PassBuilder& builder
        ) {
            if (policy_setup) {
                policy_setup(builder);
            }
            RGParameterAccessCollector collector(builder);
            parameter_owner->DeclareRGAccess(collector);
        },
        [parameter_owner, record_owner](CommandList& command_list) {
            std::invoke(*record_owner, command_list, *parameter_owner);
        },
        execution,
        workload
    );
}

} // namespace Moer::Render

#define DEFINE_RG_TEXTURE_ACCESS(name, mode, state) \
    ::Moer::Render::RGTextureAccess<mode, state> name {}

#define DEFINE_RG_BUFFER_ACCESS(name, mode, state) \
    ::Moer::Render::RGBufferAccess<mode, state> name {}

#define DEFINE_RG_TOKEN_ACCESS(name, mode) \
    ::Moer::Render::RGTokenAccess<mode> name {}

#define DEFINE_RG_NESTED_PARAMETER(type, name) type name {}

#define DEFINE_RG_TEXTURE_ACCESS_ARRAY(name) \
    ::Moer::Render::RGTextureAccessArray name {}

#define DEFINE_RG_BUFFER_ACCESS_ARRAY(name) \
    ::Moer::Render::RGBufferAccessArray name {}

#define DEFINE_RG_OPTIONAL_TEXTURE_ACCESS(name, mode, state) \
    ::Moer::Render::RGOptionalAccess<                      \
        ::Moer::Render::RGTextureAccess<mode, state>> name {}

#define DEFINE_RG_OPTIONAL_BUFFER_ACCESS(name, mode, state) \
    ::Moer::Render::RGOptionalAccess<                     \
        ::Moer::Render::RGBufferAccess<mode, state>> name {}

#define DEFINE_RG_OPTIONAL_TOKEN_ACCESS(name, mode) \
    ::Moer::Render::RGOptionalAccess<               \
        ::Moer::Render::RGTokenAccess<mode>> name {}

#define DEFINE_RG_PARAMETER_ACCESS(...)                                      \
    void DeclareRGAccess(                                                     \
        ::Moer::Render::RGParameterAccessCollector& collector                 \
    ) const {                                                                 \
        ::Moer::Render::CollectRGParameterAccess(                             \
            collector __VA_OPT__(, ) __VA_ARGS__                              \
        );                                                                    \
    }
