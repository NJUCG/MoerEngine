#ifndef MOER_RENDER_RHI_RECORD_DIAGNOSTICS_H
#define MOER_RENDER_RHI_RECORD_DIAGNOSTICS_H

#include "RHICommand.h"
#include "misc/STL.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

namespace Moer::Render {

enum class RecordCapability : uint8_t {
    SerialOnly,
    ParallelPrimarySafe,
};

enum class RecordConstraint : uint32_t {
    None                    = 0,
    MutatesCommandPayload   = 1u << 0,
    SharedDescriptorBinder  = 1u << 1,
    GlobalResourceState     = 1u << 2,
    AllocatorSideEffect     = 1u << 3,
    ProfilerOrScope         = 1u << 4,
    QueueOrHostSideEffect   = 1u << 5,
    UnsupportedBackendPath  = 1u << 6,
};

constexpr RecordConstraint operator|(RecordConstraint _lhs, RecordConstraint _rhs) {
    return static_cast<RecordConstraint>(
        static_cast<uint32_t>(_lhs) | static_cast<uint32_t>(_rhs)
    );
}

struct CommandRecordTraits {
    std::string_view stable_name;
    RecordCapability capability{RecordCapability::SerialOnly};
    bool             measurement_candidate{false};
    RecordConstraint constraints{RecordConstraint::UnsupportedBackendPath};
};

constexpr CommandRecordTraits GetCommandRecordTraits(Command::EType _type) {
    using Type       = Command::EType;
    using Capability = RecordCapability;
    using Constraint = RecordConstraint;

    switch (_type) {
        case Type::UploadBuffer:
            return {"UploadBuffer", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::GlobalResourceState};
        case Type::CopyBackBuffer:
            return {"CopyBackBuffer", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::QueueOrHostSideEffect};
        case Type::BufferToBuffer:
            return {"BufferToBuffer", Capability::SerialOnly, false, Constraint::GlobalResourceState};
        case Type::BufferToTexture:
            return {"BufferToTexture", Capability::SerialOnly, false, Constraint::GlobalResourceState};
        case Type::TextureToBuffer:
            return {"TextureToBuffer", Capability::SerialOnly, false, Constraint::GlobalResourceState};
        case Type::UploadTexture:
            return {"UploadTexture", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::GlobalResourceState};
        case Type::TextureToTexture:
            return {"TextureToTexture", Capability::SerialOnly, false, Constraint::GlobalResourceState};
        case Type::CopyBackTexture:
            return {"CopyBackTexture", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::QueueOrHostSideEffect};
        case Type::ShaderDispatch:
            return {"ShaderDispatch", Capability::SerialOnly, true,
                    Constraint::SharedDescriptorBinder | Constraint::GlobalResourceState};
        case Type::BuildAccel:
            return {"BuildAccel", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::GlobalResourceState};
        case Type::BuildTLAS:
            return {"BuildTLAS", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::AllocatorSideEffect |
                        Constraint::GlobalResourceState};
        case Type::TraceRay:
            return {"TraceRay", Capability::SerialOnly, false,
                    Constraint::SharedDescriptorBinder | Constraint::UnsupportedBackendPath};
        case Type::Barrier:
            return {"Barrier", Capability::SerialOnly, false, Constraint::GlobalResourceState};
        case Type::QueueTransfer:
            return {"QueueTransfer", Capability::SerialOnly, false,
                    Constraint::MutatesCommandPayload | Constraint::QueueOrHostSideEffect |
                        Constraint::GlobalResourceState};
        case Type::SetDrawState:
            return {"SetDrawState", Capability::SerialOnly, true,
                    Constraint::SharedDescriptorBinder | Constraint::GlobalResourceState};
        case Type::SetGeometryPassDrawState:
            return {"SetGeometryPassDrawState", Capability::SerialOnly, false,
                    Constraint::UnsupportedBackendPath};
        case Type::MultiDraw:
            return {"MultiDraw", Capability::SerialOnly, true,
                    Constraint::SharedDescriptorBinder | Constraint::GlobalResourceState};
        case Type::UpdateBindlessArray:
            return {"UpdateBindlessArray", Capability::SerialOnly, false,
                    Constraint::QueueOrHostSideEffect | Constraint::GlobalResourceState};
        case Type::ClearResource:
            return {"ClearResource", Capability::SerialOnly, true,
                    Constraint::GlobalResourceState};
        case Type::Scope:
            return {"Scope", Capability::SerialOnly, false, Constraint::ProfilerOrScope};
        case Type::Custom:
            return {"Custom", Capability::SerialOnly, false,
                    Constraint::QueueOrHostSideEffect | Constraint::UnsupportedBackendPath};
        case Type::Count:
            break;
    }
    return {"Invalid", Capability::SerialOnly, false, Constraint::UnsupportedBackendPath};
}

constexpr bool IsParallelRecordCandidate(Command::EType _type) {
    return GetCommandRecordTraits(_type).measurement_candidate;
}

class StableRecordHash {
public:
    static constexpr uint64_t kOffset = 14695981039346656037ull;
    static constexpr uint64_t kPrime  = 1099511628211ull;

    void Add(uint64_t _value) {
        for (uint32_t byte_index = 0; byte_index < 8; ++byte_index) {
            state ^= (_value >> (byte_index * 8u)) & 0xffu;
            state *= kPrime;
        }
    }

    void AddBytes(std::span<const std::byte> _bytes) {
        Add(static_cast<uint64_t>(_bytes.size()));
        for (const std::byte value : _bytes) {
            state ^= std::to_integer<uint8_t>(value);
            state *= kPrime;
        }
    }

    void AddBytes(std::span<const uint8_t> _bytes) {
        AddBytes(std::as_bytes(_bytes));
    }

    void AddBytes(const void* _data, size_t _size) {
        AddBytes(std::span(static_cast<const std::byte*>(_data), _size));
    }

    void AddString(std::string_view _value) {
        Add(0x535452494e475f31ull);
        AddBytes(std::as_bytes(std::span(_value.data(), _value.size())));
    }

    uint64_t Value() const {
        return state;
    }

    static uint64_t Mix(uint64_t _value) {
        _value += 0x9e3779b97f4a7c15ull;
        _value = (_value ^ (_value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        _value = (_value ^ (_value >> 27u)) * 0x94d049bb133111ebull;
        return _value ^ (_value >> 31u);
    }

private:
    uint64_t state{kOffset};
};

// Pointer values are lookup keys only. The emitted token is submission-local and
// derives from encounter order plus alias topology, so it is stable across runs.
enum class StableObjectKind : uint8_t {
    None,
    Buffer,
    Texture,
    BufferView,
    TextureView,
    Sampler,
    Pipeline,
    AccelerationStructure,
    Descriptor,
    Query,
    Opaque,
};

struct StableSubmissionToken {
    StableObjectKind kind{StableObjectKind::Opaque};
    uint64_t         object_token{0};
    uint64_t         alias_token{0};
    uint64_t         alias_offset{0};
    uint64_t         extent{0};
    bool             complete{false};

    static constexpr StableSubmissionToken Null() {
        return {StableObjectKind::None, 0, 0, 0, 0, true};
    }
};

inline void AddStableSubmissionToken(StableRecordHash& _hash, const StableSubmissionToken& _token) {
    _hash.Add(static_cast<uint64_t>(_token.kind));
    _hash.Add(_token.object_token);
    _hash.Add(_token.alias_token);
    _hash.Add(_token.alias_offset);
    _hash.Add(_token.extent);
    _hash.Add(_token.complete ? 1u : 0u);
}

class SubmissionTokenTable {
public:
    StableSubmissionToken Register(
        StableObjectKind _kind,
        const void*      _identity,
        const void*      _alias_identity = nullptr,
        uint64_t         _alias_offset   = 0,
        uint64_t         _extent         = 0
    ) {
        if (_kind == StableObjectKind::None) {
            return StableSubmissionToken::Null();
        }
        if (_kind == StableObjectKind::Opaque || _identity == nullptr) {
            complete = false;
            return {_kind, 0, 0, 0, 0, false};
        }

        const LookupKey key{_kind, _identity};
        if (const auto found = objects.find(key); found != objects.end()) {
            const StableSubmissionToken& token = found->second;
            const void* effective_alias = _alias_identity != nullptr ? _alias_identity : _identity;
            const auto  alias_found     = aliases.find(effective_alias);
            if (alias_found == aliases.end() || token.alias_token != alias_found->second ||
                token.alias_offset != _alias_offset || token.extent != _extent) {
                complete = false;
                StableSubmissionToken conflict = token;
                conflict.complete               = false;
                return conflict;
            }
            return token;
        }

        const void* effective_alias = _alias_identity != nullptr ? _alias_identity : _identity;
        auto [alias_it, inserted]   = aliases.try_emplace(effective_alias, next_alias_token);
        if (inserted) {
            ++next_alias_token;
        }

        StableSubmissionToken token{
            _kind,
            next_object_token++,
            alias_it->second,
            _alias_offset,
            _extent,
            true,
        };
        objects.emplace(key, token);
        return token;
    }

    StableSubmissionToken Resolve(StableObjectKind _kind, const void* _identity) const {
        if (_kind == StableObjectKind::None) {
            return StableSubmissionToken::Null();
        }
        const auto found = objects.find({_kind, _identity});
        if (found == objects.end()) {
            complete = false;
            return {_kind, 0, 0, 0, 0, false};
        }
        return found->second;
    }

    bool Complete() const {
        return complete;
    }

    void Reset() {
        objects.clear();
        aliases.clear();
        next_object_token = 1;
        next_alias_token  = 1;
        complete          = true;
    }

private:
    struct LookupKey {
        StableObjectKind kind;
        const void*      identity;

        bool operator==(const LookupKey&) const = default;
    };

    struct LookupHash {
        size_t operator()(const LookupKey& _key) const {
            const size_t pointer_hash = std::hash<const void*>{}(_key.identity);
            return pointer_hash ^ (static_cast<size_t>(_key.kind) << 1u);
        }
    };

    UnorderedMap<LookupKey, StableSubmissionToken, LookupHash> objects;
    UnorderedMap<const void*, uint64_t>                         aliases;
    uint64_t                                                    next_object_token{1};
    uint64_t                                                    next_alias_token{1};
    mutable bool                                                complete{true};
};

struct SerialCommandLayerSection {
    uint64_t command_digest{StableRecordHash::kOffset};
    uint64_t layer_digest{StableRecordHash::kOffset};
    uint32_t command_count{0};
    uint32_t layer_count{0};
    bool     complete{true};
};

class SerialCommandLayerSectionBuilder {
public:
    SerialCommandLayerSectionBuilder() {
        command_hash.Add(0x434f4d4d414e4453ull);
        layer_hash.Add(0x4c41594552535f31ull);
    }

    void BeginLayer(uint32_t _layer_index) {
        if (layer_open) {
            section.complete = false;
            EndLayer();
        }
        layer_open            = true;
        current_layer_index   = _layer_index;
        current_command_count = 0;
        command_hash.Add(0x4c415945525f4247ull);
        command_hash.Add(_layer_index);
    }

    void AddCommand(
        Command::EType                         _type,
        std::span<const StableSubmissionToken> _resources       = {},
        uint64_t                               _semantic_payload = 0
    ) {
        if (!layer_open) {
            section.complete = false;
            BeginLayer(section.layer_count);
        }
        const auto traits = GetCommandRecordTraits(_type);
        command_hash.Add(0x434f4d4d414e445full);
        command_hash.Add(static_cast<uint64_t>(_type));
        command_hash.AddString(traits.stable_name);
        command_hash.Add(_semantic_payload);
        command_hash.Add(static_cast<uint64_t>(_resources.size()));
        for (const StableSubmissionToken& resource : _resources) {
            AddStableSubmissionToken(command_hash, resource);
            section.complete &= resource.complete && resource.kind != StableObjectKind::Opaque;
        }
        ++current_command_count;
        ++section.command_count;
    }

    void EndLayer() {
        if (!layer_open) {
            section.complete = false;
            return;
        }
        command_hash.Add(0x4c415945525f454eull);
        layer_hash.Add(current_layer_index);
        layer_hash.Add(current_command_count);
        ++section.layer_count;
        layer_open = false;
    }

    SerialCommandLayerSection Finish() {
        if (layer_open) {
            section.complete = false;
            EndLayer();
        }
        section.command_digest = command_hash.Value();
        section.layer_digest   = layer_hash.Value();
        return section;
    }

private:
    SerialCommandLayerSection section;
    StableRecordHash          command_hash;
    StableRecordHash          layer_hash;
    uint32_t                  current_layer_index{0};
    uint32_t                  current_command_count{0};
    bool                      layer_open{false};
};

struct SerialGoldenSection {
    uint64_t digest{StableRecordHash::kOffset};
    uint32_t item_count{0};
    bool     complete{true};
};

// Queue-family indices are device-specific and must never enter a serial
// golden.  Encode the semantic roles served by a family instead.  A family
// can legitimately serve more than one role, so the stable value is a mask.
enum class SerialQueueRole : uint32_t {
    Ignored  = 0,
    Graphics = 1u << 0,
    Compute  = 1u << 1,
    Copy     = 1u << 2,
    Unknown  = 1u << 31,
};

struct SerialQueueFamilyMap {
    uint32_t graphics_family{std::numeric_limits<uint32_t>::max()};
    uint32_t compute_family{std::numeric_limits<uint32_t>::max()};
    uint32_t copy_family{std::numeric_limits<uint32_t>::max()};
    uint32_t ignored_family{std::numeric_limits<uint32_t>::max()};
};

constexpr uint32_t ResolveSerialQueueRole(
    uint32_t                    _queue_family,
    const SerialQueueFamilyMap& _map
) {
    if (_queue_family == _map.ignored_family) {
        return static_cast<uint32_t>(SerialQueueRole::Ignored);
    }

    uint32_t roles = 0;
    if (_queue_family == _map.graphics_family) {
        roles |= static_cast<uint32_t>(SerialQueueRole::Graphics);
    }
    if (_queue_family == _map.compute_family) {
        roles |= static_cast<uint32_t>(SerialQueueRole::Compute);
    }
    if (_queue_family == _map.copy_family) {
        roles |= static_cast<uint32_t>(SerialQueueRole::Copy);
    }
    return roles == 0 ? static_cast<uint32_t>(SerialQueueRole::Unknown) : roles;
}

constexpr bool IsCompleteSerialQueueRole(uint32_t _role) {
    return _role != static_cast<uint32_t>(SerialQueueRole::Unknown);
}

struct SerialBarrierItem {
    // Barrier order inside one vkCmdPipelineBarrier2 call is not semantic, but
    // the order of barrier calls is.  Keep the dispatch group in the key and
    // canonicalize only within that group.
    uint64_t              group_ordinal{0};
    StableSubmissionToken resource;
    uint64_t              src_stage_mask{0};
    uint64_t              dst_stage_mask{0};
    uint64_t              src_access_mask{0};
    uint64_t              dst_access_mask{0};
    uint64_t              old_state{0};
    uint64_t              new_state{0};
    uint32_t              src_queue_role{0};
    uint32_t              dst_queue_role{0};
    bool                  queue_roles_complete{true};
    uint64_t              range_offset{0};
    uint64_t              range_size{0};
    uint64_t              aspect_mask{0};
    uint32_t              base_mip_level{0};
    uint32_t              level_count{0};
    uint32_t              base_array_layer{0};
    uint32_t              layer_count{0};
};

class SerialBarrierSectionBuilder {
public:
    void Add(const SerialBarrierItem& _item) {
        items.push_back(_item);
    }

    SerialGoldenSection Finish() {
        std::sort(items.begin(), items.end(), [](const SerialBarrierItem& _lhs, const SerialBarrierItem& _rhs) {
            return Key(_lhs) < Key(_rhs);
        });

        StableRecordHash hash;
        hash.Add(0x4241525249455253ull);
        hash.Add(static_cast<uint64_t>(items.size()));
        bool complete = true;
        for (const SerialBarrierItem& item : items) {
            AddItem(hash, item);
            complete &= item.resource.complete && item.resource.kind != StableObjectKind::Opaque &&
                        item.queue_roles_complete;
        }
        return {hash.Value(), static_cast<uint32_t>(items.size()), complete};
    }

private:
    static auto Key(const SerialBarrierItem& _item) {
        return std::tuple{
            _item.group_ordinal,
            static_cast<uint64_t>(_item.resource.kind),
            _item.resource.object_token,
            _item.resource.alias_token,
            _item.resource.alias_offset,
            _item.resource.extent,
            _item.resource.complete,
            _item.src_stage_mask,
            _item.dst_stage_mask,
            _item.src_access_mask,
            _item.dst_access_mask,
            _item.old_state,
            _item.new_state,
            _item.src_queue_role,
            _item.dst_queue_role,
            _item.queue_roles_complete,
            _item.range_offset,
            _item.range_size,
            _item.aspect_mask,
            _item.base_mip_level,
            _item.level_count,
            _item.base_array_layer,
            _item.layer_count,
        };
    }

    static void AddItem(StableRecordHash& _hash, const SerialBarrierItem& _item) {
        _hash.Add(0x424152524945525full);
        _hash.Add(_item.group_ordinal);
        AddStableSubmissionToken(_hash, _item.resource);
        _hash.Add(_item.src_stage_mask);
        _hash.Add(_item.dst_stage_mask);
        _hash.Add(_item.src_access_mask);
        _hash.Add(_item.dst_access_mask);
        _hash.Add(_item.old_state);
        _hash.Add(_item.new_state);
        _hash.Add(_item.src_queue_role);
        _hash.Add(_item.dst_queue_role);
        _hash.Add(_item.queue_roles_complete ? 1u : 0u);
        _hash.Add(_item.range_offset);
        _hash.Add(_item.range_size);
        _hash.Add(_item.aspect_mask);
        _hash.Add(_item.base_mip_level);
        _hash.Add(_item.level_count);
        _hash.Add(_item.base_array_layer);
        _hash.Add(_item.layer_count);
    }

    Array<SerialBarrierItem> items;
};

struct SerialDescriptorItem {
    uint32_t              bind_ordinal{0};
    uint32_t              bind_point{0};
    uint32_t              descriptor_set{0};
    uint32_t              binding{0};
    uint32_t              array_element{0};
    uint32_t              descriptor_type{0};
    uint32_t              param_idx{std::numeric_limits<uint32_t>::max()};
    uint32_t              declared_descriptor_count{0};
    StableSubmissionToken resource{StableSubmissionToken::Null()};
    uint64_t              resource_offset{0};
    uint64_t              resource_range{0};
    Array<uint8_t>        semantic_bytes;
    bool                  complete{true};
};

class SerialDescriptorSectionBuilder {
public:
    void Add(const SerialDescriptorItem& _item) {
        items.push_back(_item);
    }

    void Add(
        uint32_t                       _bind_ordinal,
        uint32_t                       _bind_point,
        uint32_t                       _descriptor_set,
        uint32_t                       _binding,
        uint32_t                       _array_element,
        uint32_t                       _descriptor_type,
        uint32_t                       _param_idx,
        uint32_t                       _declared_descriptor_count,
        const StableSubmissionToken&   _resource,
        uint64_t                       _resource_offset,
        uint64_t                       _resource_range,
        std::span<const uint8_t>        _semantic_bytes,
        bool                           _complete = true
    ) {
        SerialDescriptorItem item;
        item.bind_ordinal   = _bind_ordinal;
        item.bind_point     = _bind_point;
        item.descriptor_set  = _descriptor_set;
        item.binding         = _binding;
        item.array_element   = _array_element;
        item.descriptor_type = _descriptor_type;
        item.param_idx       = _param_idx;
        item.declared_descriptor_count = _declared_descriptor_count;
        item.resource        = _resource;
        item.resource_offset = _resource_offset;
        item.resource_range  = _resource_range;
        item.semantic_bytes.assign(_semantic_bytes.begin(), _semantic_bytes.end());
        item.complete = _complete;
        Add(item);
    }

    SerialGoldenSection Finish() {
        std::sort(items.begin(), items.end(), [](const SerialDescriptorItem& _lhs, const SerialDescriptorItem& _rhs) {
            return Less(_lhs, _rhs);
        });

        StableRecordHash hash;
        hash.Add(0x4445534352495054ull);
        hash.Add(static_cast<uint64_t>(items.size()));
        bool complete = true;
        for (const SerialDescriptorItem& item : items) {
            hash.Add(0x444553435249505full);
            hash.Add(item.bind_ordinal);
            hash.Add(item.bind_point);
            hash.Add(item.descriptor_set);
            hash.Add(item.binding);
            hash.Add(item.array_element);
            hash.Add(item.descriptor_type);
            hash.Add(item.param_idx);
            hash.Add(item.declared_descriptor_count);
            AddStableSubmissionToken(hash, item.resource);
            hash.Add(item.resource_offset);
            hash.Add(item.resource_range);
            hash.AddBytes(std::span<const uint8_t>(item.semantic_bytes));
            hash.Add(item.complete ? 1u : 0u);
            complete &= item.complete && item.resource.complete &&
                        item.resource.kind != StableObjectKind::Opaque;
        }
        return {hash.Value(), static_cast<uint32_t>(items.size()), complete};
    }

private:
    static bool Less(const SerialDescriptorItem& _lhs, const SerialDescriptorItem& _rhs) {
        const auto lhs_key = std::tuple{
            _lhs.bind_ordinal,
            _lhs.bind_point,
            _lhs.descriptor_set,
            _lhs.binding,
            _lhs.array_element,
            _lhs.descriptor_type,
            _lhs.param_idx,
            _lhs.declared_descriptor_count,
            static_cast<uint64_t>(_lhs.resource.kind),
            _lhs.resource.object_token,
            _lhs.resource.alias_token,
            _lhs.resource.alias_offset,
            _lhs.resource.extent,
            _lhs.resource.complete,
            _lhs.resource_offset,
            _lhs.resource_range,
            _lhs.complete,
        };
        const auto rhs_key = std::tuple{
            _rhs.bind_ordinal,
            _rhs.bind_point,
            _rhs.descriptor_set,
            _rhs.binding,
            _rhs.array_element,
            _rhs.descriptor_type,
            _rhs.param_idx,
            _rhs.declared_descriptor_count,
            static_cast<uint64_t>(_rhs.resource.kind),
            _rhs.resource.object_token,
            _rhs.resource.alias_token,
            _rhs.resource.alias_offset,
            _rhs.resource.extent,
            _rhs.resource.complete,
            _rhs.resource_offset,
            _rhs.resource_range,
            _rhs.complete,
        };
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return std::lexicographical_compare(
            _lhs.semantic_bytes.begin(), _lhs.semantic_bytes.end(),
            _rhs.semantic_bytes.begin(), _rhs.semantic_bytes.end()
        );
    }

    Array<SerialDescriptorItem> items;
};

enum class SerialQueryEvent : uint8_t {
    Begin,
    End,
    Timestamp,
    Reset,
};

class SerialQuerySectionBuilder {
public:
    SerialQuerySectionBuilder() {
        hash.Add(0x5155455259455654ull);
    }

    void AddEvent(
        SerialQueryEvent _event,
        std::string_view _name,
        uint32_t         _relative_slot,
        uint64_t         _pipeline_stage_mask,
        uint64_t         _frame_index        = 0,
        uint64_t         _absolute_query_slot = 0,
        bool             _complete           = true,
        uint32_t         _command_ordinal     = std::numeric_limits<uint32_t>::max()
    ) {
        (void)_frame_index;
        (void)_absolute_query_slot;
        hash.Add(0x51554552595f4954ull);
        hash.Add(static_cast<uint64_t>(_event));
        hash.AddString(_name);
        hash.Add(_pipeline_stage_mask);
        hash.Add(_command_ordinal);
        hash.Add(_relative_slot);
        hash.Add(_complete ? 1u : 0u);
        complete &= _complete;
        ++item_count;
    }

    SerialGoldenSection Finish() const {
        return {hash.Value(), item_count, complete};
    }

private:
    StableRecordHash hash;
    uint32_t         item_count{0};
    bool             complete{true};
};

struct SerialGoldenSummary {
    uint64_t command_digest{StableRecordHash::kOffset};
    uint64_t layer_digest{StableRecordHash::kOffset};
    uint64_t barrier_digest{StableRecordHash::kOffset};
    uint64_t descriptor_digest{StableRecordHash::kOffset};
    uint64_t query_digest{StableRecordHash::kOffset};
    uint64_t combined_digest{StableRecordHash::kOffset};
    uint32_t command_count{0};
    uint32_t layer_count{0};
    uint32_t barrier_count{0};
    uint32_t descriptor_count{0};
    uint32_t query_count{0};
    bool     complete{true};
};

inline uint64_t CombineSerialGoldenDigests(const SerialGoldenSummary& _summary) {
    StableRecordHash hash;
    hash.Add(0x53455249414c4731ull);
    hash.Add(0x434f4d4d414e4453ull);
    hash.Add(_summary.command_digest);
    hash.Add(_summary.command_count);
    hash.Add(0x4c41594552535f31ull);
    hash.Add(_summary.layer_digest);
    hash.Add(_summary.layer_count);
    hash.Add(0x4241525249455253ull);
    hash.Add(_summary.barrier_digest);
    hash.Add(_summary.barrier_count);
    hash.Add(0x4445534352495054ull);
    hash.Add(_summary.descriptor_digest);
    hash.Add(_summary.descriptor_count);
    hash.Add(0x5155455259455654ull);
    hash.Add(_summary.query_digest);
    hash.Add(_summary.query_count);
    hash.Add(_summary.complete ? 1u : 0u);
    return hash.Value();
}

inline SerialGoldenSummary MakeSerialGoldenSummary(
    const SerialCommandLayerSection& _commands,
    const SerialGoldenSection&       _barriers,
    const SerialGoldenSection&       _descriptors,
    const SerialGoldenSection&       _queries
) {
    SerialGoldenSummary summary;
    summary.command_digest    = _commands.command_digest;
    summary.layer_digest      = _commands.layer_digest;
    summary.barrier_digest    = _barriers.digest;
    summary.descriptor_digest = _descriptors.digest;
    summary.query_digest      = _queries.digest;
    summary.command_count     = _commands.command_count;
    summary.layer_count       = _commands.layer_count;
    summary.barrier_count     = _barriers.item_count;
    summary.descriptor_count  = _descriptors.item_count;
    summary.query_count       = _queries.item_count;
    summary.complete          = _commands.complete && _barriers.complete &&
                                _descriptors.complete && _queries.complete;
    summary.combined_digest = CombineSerialGoldenDigests(summary);
    return summary;
}

struct RecordTopologySummary {
    uint64_t command_digest{StableRecordHash::kOffset};
    uint64_t layer_digest{StableRecordHash::kOffset};
    uint64_t topology_digest{StableRecordHash::kOffset};
    uint32_t layer_count{0};
    uint32_t command_count{0};
    uint32_t candidate_command_count{0};
    uint32_t safe_command_count{0};
    Array<uint32_t> layer_command_counts;
    Array<uint32_t> layer_candidate_counts;
};

class RecordTopologyBuilder {
public:
    void BeginLayer(uint32_t _layer_index) {
        current_layer_index      = _layer_index;
        current_command_count    = 0;
        current_candidate_count  = 0;
        command_hash.Add(0x4c415945525f4247ull);
        command_hash.Add(_layer_index);
    }

    void AddCommand(Command::EType _type) {
        const CommandRecordTraits traits = GetCommandRecordTraits(_type);
        command_hash.Add(static_cast<uint64_t>(_type));
        command_hash.Add(static_cast<uint64_t>(traits.capability));
        command_hash.Add(traits.measurement_candidate ? 1u : 0u);
        ++summary.command_count;
        ++current_command_count;
        if (traits.capability == RecordCapability::ParallelPrimarySafe) {
            ++summary.safe_command_count;
        }
        if (IsParallelRecordCandidate(_type)) {
            ++summary.candidate_command_count;
            ++current_candidate_count;
        }
    }

    void EndLayer() {
        layer_hash.Add(current_layer_index);
        layer_hash.Add(current_command_count);
        layer_hash.Add(current_candidate_count);
        summary.layer_command_counts.push_back(current_command_count);
        summary.layer_candidate_counts.push_back(current_candidate_count);
        ++summary.layer_count;
    }

    RecordTopologySummary Finish() {
        summary.command_digest = command_hash.Value();
        summary.layer_digest   = layer_hash.Value();
        StableRecordHash topology_hash;
        topology_hash.Add(summary.command_digest);
        topology_hash.Add(summary.layer_digest);
        topology_hash.Add(summary.layer_count);
        topology_hash.Add(summary.command_count);
        topology_hash.Add(summary.candidate_command_count);
        summary.topology_digest = topology_hash.Value();
        return std::move(summary);
    }

private:
    RecordTopologySummary summary;
    StableRecordHash      command_hash;
    StableRecordHash      layer_hash;
    uint32_t              current_layer_index{0};
    uint32_t              current_command_count{0};
    uint32_t              current_candidate_count{0};
};

struct RecordLayerTiming {
    double        wall_ms{0.0};
    Array<double> candidate_units_ms;
};

struct RecordPrediction {
    uint32_t model_workers{1};
    uint32_t parallel_layer_count{0};
    double   serial_record_wall_ms{0.0};
    double   eligible_record_ms{0.0};
    double   predicted_critical_ms{0.0};
    double   dispatch_join_estimate_ms{0.0};
    double   predicted_net_saving_ms{0.0};
};

inline RecordPrediction PredictParallelRecordCriticalPath(
    std::span<const RecordLayerTiming> _layers,
    uint32_t                           _model_workers,
    double                             _dispatch_join_tail_ms
) {
    RecordPrediction prediction;
    prediction.model_workers = std::max(1u, _model_workers);

    for (const RecordLayerTiming& layer : _layers) {
        prediction.serial_record_wall_ms += layer.wall_ms;
        if (prediction.model_workers < 2 || layer.candidate_units_ms.size() < 2) {
            prediction.predicted_critical_ms += layer.wall_ms;
            continue;
        }

        ++prediction.parallel_layer_count;
        const uint32_t worker_count = std::min<uint32_t>(
            prediction.model_workers, static_cast<uint32_t>(layer.candidate_units_ms.size())
        );
        Array<double> worker_loads(worker_count, 0.0);
        double        candidate_sum = 0.0;
        for (const double unit_ms : layer.candidate_units_ms) {
            candidate_sum += unit_ms;
            auto least_loaded = std::min_element(worker_loads.begin(), worker_loads.end());
            *least_loaded += unit_ms;
        }

        prediction.eligible_record_ms += candidate_sum;
        const double serial_remainder = std::max(0.0, layer.wall_ms - candidate_sum);
        prediction.predicted_critical_ms +=
            serial_remainder + *std::max_element(worker_loads.begin(), worker_loads.end());
    }

    prediction.dispatch_join_estimate_ms =
        double(prediction.parallel_layer_count) * std::max(0.0, _dispatch_join_tail_ms);
    prediction.predicted_net_saving_ms = prediction.serial_record_wall_ms -
                                         prediction.predicted_critical_ms -
                                         prediction.dispatch_join_estimate_ms;
    return prediction;
}

} // namespace Moer::Render

#endif
