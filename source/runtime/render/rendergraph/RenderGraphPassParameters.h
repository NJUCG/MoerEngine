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
    RenderGraph::AccessMode        Mode,
    RenderGraph::TextureAccessType Type = RenderGraph::TextureAccessType::Unspecified>
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
    RenderGraph::AccessMode       Mode,
    RenderGraph::BufferAccessType Type = RenderGraph::BufferAccessType::Unspecified>
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

/** One heterogeneous texture-array element with explicit mode and type. */
class RGTextureAccessElement {
public:
    [[nodiscard]] static RGTextureAccessElement Read(
        RenderGraph::TextureHandle     handle,
        RenderGraph::TextureAccessType type  = RenderGraph::TextureAccessType::Unspecified,
        RenderGraph::TextureRange      range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::Read,
            type,
            range
        );
    }

    [[nodiscard]] static RGTextureAccessElement Write(
        RenderGraph::TextureHandle     handle,
        RenderGraph::TextureAccessType type  = RenderGraph::TextureAccessType::Unspecified,
        RenderGraph::TextureRange      range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::Write,
            type,
            range
        );
    }

    [[nodiscard]] static RGTextureAccessElement ReadWrite(
        RenderGraph::TextureHandle     handle,
        RenderGraph::TextureAccessType type  = RenderGraph::TextureAccessType::Unspecified,
        RenderGraph::TextureRange      range = RenderGraph::TextureRange::Whole()
    ) {
        return RGTextureAccessElement(
            handle,
            RenderGraph::AccessMode::ReadWrite,
            type,
            range
        );
    }

    [[nodiscard]] RenderGraph::TextureHandle Resource() const {
        return resource;
    }
    [[nodiscard]] RenderGraph::AccessMode Mode() const {
        return mode;
    }
    [[nodiscard]] RenderGraph::TextureAccessType Type() const {
        return type;
    }
    [[nodiscard]] RenderGraph::TextureRange Range() const {
        return range;
    }

private:
    RGTextureAccessElement(
        RenderGraph::TextureHandle     handle,
        RenderGraph::AccessMode        mode,
        RenderGraph::TextureAccessType type,
        RenderGraph::TextureRange      range
    ) :
        resource(handle),
        mode(mode),
        type(type),
        range(range) {}

    RenderGraph::TextureHandle resource{};
    RenderGraph::AccessMode    mode = RenderGraph::AccessMode::None;
    RenderGraph::TextureAccessType type  = RenderGraph::TextureAccessType::Unspecified;
    RenderGraph::TextureRange  range = RenderGraph::TextureRange::Whole();
};

/** One heterogeneous buffer-array element with explicit mode and type. */
class RGBufferAccessElement {
public:
    [[nodiscard]] static RGBufferAccessElement Read(
        RenderGraph::BufferHandle     handle,
        RenderGraph::BufferAccessType type  = RenderGraph::BufferAccessType::Unspecified,
        RenderGraph::BufferRange      range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::Read,
            type,
            range
        );
    }

    [[nodiscard]] static RGBufferAccessElement Write(
        RenderGraph::BufferHandle     handle,
        RenderGraph::BufferAccessType type  = RenderGraph::BufferAccessType::Unspecified,
        RenderGraph::BufferRange      range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::Write,
            type,
            range
        );
    }

    [[nodiscard]] static RGBufferAccessElement ReadWrite(
        RenderGraph::BufferHandle     handle,
        RenderGraph::BufferAccessType type  = RenderGraph::BufferAccessType::Unspecified,
        RenderGraph::BufferRange      range = RenderGraph::BufferRange::Whole()
    ) {
        return RGBufferAccessElement(
            handle,
            RenderGraph::AccessMode::ReadWrite,
            type,
            range
        );
    }

    [[nodiscard]] RenderGraph::BufferHandle Resource() const {
        return resource;
    }
    [[nodiscard]] RenderGraph::AccessMode Mode() const {
        return mode;
    }
    [[nodiscard]] RenderGraph::BufferAccessType Type() const {
        return type;
    }
    [[nodiscard]] RenderGraph::BufferRange Range() const {
        return range;
    }

private:
    RGBufferAccessElement(
        RenderGraph::BufferHandle     handle,
        RenderGraph::AccessMode       mode,
        RenderGraph::BufferAccessType type,
        RenderGraph::BufferRange      range
    ) :
        resource(handle),
        mode(mode),
        type(type),
        range(range) {}

    RenderGraph::BufferHandle resource{};
    RenderGraph::AccessMode   mode = RenderGraph::AccessMode::None;
    RenderGraph::BufferAccessType type  = RenderGraph::BufferAccessType::Unspecified;
    RenderGraph::BufferRange  range = RenderGraph::BufferRange::Whole();
};

using RGTextureAccessArray = RGAccessArray<RGTextureAccessElement>;
using RGBufferAccessArray  = RGAccessArray<RGBufferAccessElement>;

/**
 * Adapts parameter declarations to the existing typed PassBuilder API. It
 * intentionally owns no compiler type and performs no access inference.
 */
class RGParameterAccessCollector {
public:
    explicit RGParameterAccessCollector(RenderGraph::PassBuilder& builder) :
        builder(builder) {}

    template<RenderGraph::AccessMode Mode, RenderGraph::TextureAccessType Type>
    void Add(const RGTextureAccess<Mode, Type>& access) {
        AddTexture(access.resource, Mode, Type, access.range);
    }

    template<RenderGraph::AccessMode Mode, RenderGraph::BufferAccessType Type>
    void Add(const RGBufferAccess<Mode, Type>& access) {
        AddBuffer(access.resource, Mode, Type, access.range);
    }

    void Add(const RGTextureAccessElement& access) {
        AddTexture(
            access.Resource(),
            access.Mode(),
            access.Type(),
            access.Range()
        );
    }

    void Add(const RGBufferAccessElement& access) {
        AddBuffer(
            access.Resource(),
            access.Mode(),
            access.Type(),
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
        RenderGraph::TextureHandle     handle,
        RenderGraph::AccessMode        mode,
        RenderGraph::TextureAccessType type,
        RenderGraph::TextureRange      range
    ) {
        switch (mode) {
            case RenderGraph::AccessMode::Read:
                if (type == RenderGraph::TextureAccessType::Unspecified) {
                    builder.Read(handle, range);
                } else {
                    builder.Read(handle, type, range);
                }
                return;
            case RenderGraph::AccessMode::Write:
                if (type == RenderGraph::TextureAccessType::Unspecified) {
                    builder.Write(handle, range);
                } else {
                    builder.Write(handle, type, range);
                }
                return;
            case RenderGraph::AccessMode::ReadWrite:
                if (type == RenderGraph::TextureAccessType::Unspecified) {
                    builder.ReadWrite(handle, range);
                } else {
                    builder.ReadWrite(handle, type, range);
                }
                return;
            case RenderGraph::AccessMode::Unknown:
            case RenderGraph::AccessMode::None:
                builder.Read(RenderGraph::TextureHandle{}, range);
                return;
        }
    }

    void AddBuffer(
        RenderGraph::BufferHandle     handle,
        RenderGraph::AccessMode       mode,
        RenderGraph::BufferAccessType type,
        RenderGraph::BufferRange      range
    ) {
        switch (mode) {
            case RenderGraph::AccessMode::Read:
                if (type == RenderGraph::BufferAccessType::Unspecified) {
                    builder.Read(handle, range);
                } else {
                    builder.Read(handle, type, range);
                }
                return;
            case RenderGraph::AccessMode::Write:
                if (type == RenderGraph::BufferAccessType::Unspecified) {
                    builder.Write(handle, range);
                } else {
                    builder.Write(handle, type, range);
                }
                return;
            case RenderGraph::AccessMode::ReadWrite:
                if (type == RenderGraph::BufferAccessType::Unspecified) {
                    builder.ReadWrite(handle, range);
                } else {
                    builder.ReadWrite(handle, type, range);
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
    PassRecordPolicy policy
) {
    using ParameterType = std::remove_cvref_t<Parameters>;
    using RecordType    = std::decay_t<Record>;

    if constexpr (std::is_pointer_v<RecordType>) {
        if (record == nullptr) {
            return AddRecordPass(
                name,
                policy_setup,
                RecordCallback{},
                policy
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
                policy
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
        policy
    );
}

} // namespace Moer::Render

#define DEFINE_RG_TEXTURE_ACCESS(name, mode, type) \
    ::Moer::Render::RGTextureAccess<mode, type> name {}

#define DEFINE_RG_BUFFER_ACCESS(name, mode, type) \
    ::Moer::Render::RGBufferAccess<mode, type> name {}

#define DEFINE_RG_TOKEN_ACCESS(name, mode) \
    ::Moer::Render::RGTokenAccess<mode> name {}

#define DEFINE_RG_NESTED_PARAMETER(type, name) type name {}

#define DEFINE_RG_TEXTURE_ACCESS_ARRAY(name) \
    ::Moer::Render::RGTextureAccessArray name {}

#define DEFINE_RG_BUFFER_ACCESS_ARRAY(name) \
    ::Moer::Render::RGBufferAccessArray name {}

#define DEFINE_RG_OPTIONAL_TEXTURE_ACCESS(name, mode, type) \
    ::Moer::Render::RGOptionalAccess<                       \
        ::Moer::Render::RGTextureAccess<mode, type>>        \
        name {}

#define DEFINE_RG_OPTIONAL_BUFFER_ACCESS(name, mode, type) \
    ::Moer::Render::RGOptionalAccess<                      \
        ::Moer::Render::RGBufferAccess<mode, type>>        \
        name {}

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
