#include "rhi/RHIRecordDiagnostics.h"
#include "rhi/RHIBindlessUpdateLifecycle.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

void ExpectNear(double _actual, double _expected, const char* _message) {
    if (std::abs(_actual - _expected) > 1e-9) {
        throw std::runtime_error(_message);
    }
}

RecordTopologySummary BuildTopology(bool _swap_commands = false) {
    RecordTopologyBuilder builder;
    builder.BeginLayer(0);
    builder.AddCommand(
        _swap_commands ? Command::EType::MultiDraw : Command::EType::ShaderDispatch
    );
    builder.AddCommand(
        _swap_commands ? Command::EType::ShaderDispatch : Command::EType::MultiDraw
    );
    builder.EndLayer();
    builder.BeginLayer(1);
    builder.AddCommand(Command::EType::Scope);
    builder.EndLayer();
    return builder.Finish();
}

void CapabilityTableIsExplicitAndFailClosed() {
    constexpr std::array expected_candidates{
        Command::EType::ShaderDispatch,
        Command::EType::SetDrawState,
        Command::EType::MultiDraw,
        Command::EType::ClearResource,
    };
    constexpr std::array expected_parallel_safe{
        Command::EType::BufferToBuffer,
        Command::EType::BufferToTexture,
        Command::EType::TextureToBuffer,
        Command::EType::TextureToTexture,
        Command::EType::ShaderDispatch,
        Command::EType::SetDrawState,
        Command::EType::MultiDraw,
        Command::EType::ClearResource,
    };
    uint32_t serial_count    = 0;
    uint32_t candidate_count = 0;
    uint32_t safe_count      = 0;
    for (uint32_t value = 0; value < static_cast<uint32_t>(Command::EType::Count); ++value) {
        const auto type   = static_cast<Command::EType>(value);
        const auto traits = GetCommandRecordTraits(type);
        Expect(!traits.stable_name.empty(), "command capability entry has no stable name");
        Expect(
            traits.stable_name == Command::typenames[value],
            "command capability name differs from the canonical type name"
        );
        switch (traits.capability) {
            case RecordCapability::SerialOnly:
                ++serial_count;
                break;
            case RecordCapability::ParallelPrimarySafe:
                ++safe_count;
                break;
        }
        if (traits.measurement_candidate) {
            ++candidate_count;
        }
        const bool expected_candidate =
            std::find(expected_candidates.begin(), expected_candidates.end(), type) !=
            expected_candidates.end();
        Expect(
            traits.measurement_candidate == expected_candidate,
            "measurement-candidate command set changed unexpectedly"
        );
        const bool expected_safe =
            std::find(expected_parallel_safe.begin(), expected_parallel_safe.end(), type) !=
            expected_parallel_safe.end();
        Expect(
            (traits.capability == RecordCapability::ParallelPrimarySafe) == expected_safe,
            "parallel-primary-safe command set changed unexpectedly"
        );
    }

    Expect(serial_count > 0, "capability table has no serial fallback entries");
    Expect(candidate_count == 4, "unexpected Phase 9.0 candidate command count");
    Expect(safe_count == expected_parallel_safe.size(), "unexpected parallel-safe command count");
    Expect(
        GetCommandRecordTraits(Command::EType::Count).capability == RecordCapability::SerialOnly,
        "invalid command type did not fail closed"
    );
}

void ParallelReplayContractExcludesSideEffects() {
    constexpr std::array replay_safe{
        Command::EType::BufferToBuffer,
        Command::EType::BufferToTexture,
        Command::EType::TextureToBuffer,
        Command::EType::TextureToTexture,
        Command::EType::ShaderDispatch,
        Command::EType::SetDrawState,
        Command::EType::MultiDraw,
        Command::EType::ClearResource,
    };
    for (const Command::EType type : replay_safe) {
        Expect(IsParallelRecordReplaySafe(type), "parallel whitelist is not replay safe");
    }
    Expect(!IsParallelRecordReplaySafe(Command::EType::UploadBuffer),
           "payload-mutating upload was marked replay safe");
    Expect(!IsParallelRecordReplaySafe(Command::EType::Scope),
           "profiler scope was marked replay safe");
    Expect(!IsParallelRecordReplaySafe(Command::EType::Custom),
           "host callback command was marked replay safe");
}

void TopologyDigestIsDeterministicAndOrderSensitive() {
    const RecordTopologySummary first  = BuildTopology();
    const RecordTopologySummary second = BuildTopology();
    const RecordTopologySummary swapped = BuildTopology(true);

    Expect(first.command_digest == second.command_digest, "command digest is not deterministic");
    Expect(first.layer_digest == second.layer_digest, "layer digest is not deterministic");
    Expect(first.topology_digest == second.topology_digest, "topology digest is not deterministic");
    Expect(first.command_digest != swapped.command_digest, "command digest ignored command order");
    Expect(first.layer_count == 2 && first.command_count == 3, "topology counts are incorrect");
    Expect(first.candidate_command_count == 2, "topology candidate count is incorrect");
}

void PredictionRejectsSingleUnitAndModelsEligibleLayer() {
    Moer::Array<RecordLayerTiming> single_unit_layers;
    single_unit_layers.push_back({10.0, {7.0}});
    const RecordPrediction single =
        PredictParallelRecordCriticalPath(single_unit_layers, 4, 1.0);
    Expect(single.parallel_layer_count == 0, "single candidate unit formed a parallel layer");
    ExpectNear(single.predicted_critical_ms, 10.0, "single-unit critical path changed");
    ExpectNear(single.predicted_net_saving_ms, 0.0, "single-unit layer predicted a saving");

    Moer::Array<RecordLayerTiming> layers;
    layers.push_back({10.0, {4.0, 3.0}});
    layers.push_back({2.0, {}});
    const RecordPrediction prediction = PredictParallelRecordCriticalPath(layers, 2, 1.0);
    Expect(prediction.parallel_layer_count == 1, "eligible layer was not recognized");
    ExpectNear(prediction.serial_record_wall_ms, 12.0, "serial record wall is incorrect");
    ExpectNear(prediction.eligible_record_ms, 7.0, "eligible record time is incorrect");
    ExpectNear(prediction.predicted_critical_ms, 9.0, "predicted critical path is incorrect");
    ExpectNear(prediction.dispatch_join_estimate_ms, 1.0, "dispatch/join estimate is incorrect");
    ExpectNear(prediction.predicted_net_saving_ms, 2.0, "predicted net saving is incorrect");

    const RecordPrediction zero_workers =
        PredictParallelRecordCriticalPath(layers, 0, 1.0);
    const RecordPrediction one_worker =
        PredictParallelRecordCriticalPath(layers, 1, 1.0);
    Expect(zero_workers.model_workers == 1, "zero workers did not clamp to one");
    Expect(zero_workers.parallel_layer_count == 0, "zero workers enabled a parallel layer");
    ExpectNear(
        zero_workers.predicted_critical_ms,
        one_worker.predicted_critical_ms,
        "zero- and one-worker predictions diverged"
    );

    Moer::Array<RecordLayerTiming> three_units;
    three_units.push_back({10.0, {4.0, 3.0, 2.0}});
    const RecordPrediction greedy =
        PredictParallelRecordCriticalPath(three_units, 2, 0.5);
    ExpectNear(greedy.eligible_record_ms, 9.0, "three-unit eligible sum is incorrect");
    ExpectNear(greedy.predicted_critical_ms, 6.0, "greedy worker allocation is incorrect");
    ExpectNear(greedy.predicted_net_saving_ms, 3.5, "greedy net saving is incorrect");

    Moer::Array<RecordLayerTiming> overhead_dominated;
    overhead_dominated.push_back({2.0, {1.0, 1.0}});
    const RecordPrediction negative =
        PredictParallelRecordCriticalPath(overhead_dominated, 2, 3.0);
    ExpectNear(
        negative.predicted_net_saving_ms,
        -2.0,
        "dispatch-dominated negative saving was clamped or miscomputed"
    );
}

const void* FakePointer(uintptr_t _value) {
    return reinterpret_cast<const void*>(_value);
}

struct AliasedTokens {
    SubmissionTokenTable table;
    StableSubmissionToken first;
    StableSubmissionToken second;
};

AliasedTokens MakeAliasedTokens(
    uintptr_t _pointer_base,
    bool      _share_alias = true,
    uint64_t  _second_offset = 64
) {
    AliasedTokens tokens;
    const void* first_identity  = FakePointer(_pointer_base + 1);
    const void* second_identity = FakePointer(_pointer_base + 2);
    const void* first_alias     = FakePointer(_pointer_base + 100);
    const void* second_alias =
        _share_alias ? first_alias : FakePointer(_pointer_base + 200);
    tokens.first = tokens.table.Register(
        StableObjectKind::BufferView, first_identity, first_alias, 0, 64
    );
    tokens.second = tokens.table.Register(
        StableObjectKind::BufferView, second_identity, second_alias, _second_offset, 64
    );
    return tokens;
}

SerialCommandLayerSection BuildAliasedCommandSection(
    uintptr_t _pointer_base,
    bool      _share_alias = true,
    uint64_t  _second_offset = 64,
    bool      _reverse_order = false
) {
    AliasedTokens tokens = MakeAliasedTokens(_pointer_base, _share_alias, _second_offset);
    SerialCommandLayerSectionBuilder builder;
    builder.BeginLayer(0);
    const std::array first_resource{
        _reverse_order ? tokens.second : tokens.first,
    };
    const std::array second_resource{
        _reverse_order ? tokens.first : tokens.second,
    };
    builder.AddCommand(Command::EType::ShaderDispatch, first_resource, 17);
    builder.AddCommand(Command::EType::ShaderDispatch, second_resource, 17);
    builder.EndLayer();
    return builder.Finish();
}

void StableTokensIgnorePointersAndCaptureAliasTopology() {
    const SerialCommandLayerSection baseline = BuildAliasedCommandSection(0x1000);
    const SerialCommandLayerSection relocated = BuildAliasedCommandSection(0x9000);
    const SerialCommandLayerSection offset_changed =
        BuildAliasedCommandSection(0x1000, true, 96);
    const SerialCommandLayerSection alias_changed =
        BuildAliasedCommandSection(0x1000, false, 64);
    const SerialCommandLayerSection order_changed =
        BuildAliasedCommandSection(0x1000, true, 64, true);

    Expect(baseline.complete && relocated.complete, "stable token topology is incomplete");
    Expect(
        baseline.command_digest == relocated.command_digest,
        "pointer relocation changed the command digest"
    );
    Expect(
        baseline.layer_digest == relocated.layer_digest,
        "pointer relocation changed the layer digest"
    );
    Expect(
        baseline.command_digest != offset_changed.command_digest,
        "alias offset was omitted from the command digest"
    );
    Expect(
        baseline.command_digest != alias_changed.command_digest,
        "alias topology was omitted from the command digest"
    );
    Expect(
        baseline.command_digest != order_changed.command_digest,
        "command resource order was omitted from the digest"
    );
}

SerialBarrierItem MakeBarrier(
    const StableSubmissionToken& _resource,
    uint64_t                     _old_state,
    uint64_t                     _new_state,
    uint64_t                     _group_ordinal = 0
) {
    SerialBarrierItem item;
    item.group_ordinal  = _group_ordinal;
    item.resource        = _resource;
    item.src_stage_mask  = 1;
    item.dst_stage_mask  = 2;
    item.src_access_mask = 4;
    item.dst_access_mask = 8;
    item.old_state       = _old_state;
    item.new_state       = _new_state;
    item.range_size      = 64;
    return item;
}

void BarrierDigestIsOrderIndependentButTokenSensitive() {
    AliasedTokens tokens = MakeAliasedTokens(0x2000);
    const SerialBarrierItem first  = MakeBarrier(tokens.first, 3, 5);
    const SerialBarrierItem second = MakeBarrier(tokens.second, 5, 7);

    SerialBarrierSectionBuilder forward_builder;
    forward_builder.Add(first);
    forward_builder.Add(second);
    const SerialGoldenSection forward = forward_builder.Finish();

    SerialBarrierSectionBuilder reverse_builder;
    reverse_builder.Add(second);
    reverse_builder.Add(first);
    const SerialGoldenSection reverse = reverse_builder.Finish();

    SerialBarrierSectionBuilder changed_builder;
    changed_builder.Add(MakeBarrier(tokens.second, 3, 5));
    changed_builder.Add(second);
    const SerialGoldenSection changed = changed_builder.Finish();

    SerialBarrierSectionBuilder regrouped_builder;
    regrouped_builder.Add(MakeBarrier(tokens.first, 3, 5, 1));
    regrouped_builder.Add(MakeBarrier(tokens.second, 5, 7, 0));
    const SerialGoldenSection regrouped = regrouped_builder.Finish();

    Expect(forward.complete, "canonical barrier section is incomplete");
    Expect(forward.digest == reverse.digest, "barrier input order changed the digest");
    Expect(forward.digest != changed.digest, "barrier resource token did not affect the digest");
    Expect(forward.digest != regrouped.digest, "barrier dispatch group order was omitted");
}

void QueueFamilyRolesAreStableCompleteAndDirectional() {
    constexpr uint32_t ignored = std::numeric_limits<uint32_t>::max();
    const SerialQueueFamilyMap distinct{
        .graphics_family = 3,
        .compute_family  = 5,
        .copy_family     = 7,
        .ignored_family  = ignored,
    };
    Expect(
        ResolveSerialQueueRole(3, distinct) ==
            static_cast<uint32_t>(SerialQueueRole::Graphics),
        "graphics queue family did not map to its stable role"
    );
    Expect(
        ResolveSerialQueueRole(5, distinct) ==
            static_cast<uint32_t>(SerialQueueRole::Compute),
        "compute queue family did not map to its stable role"
    );
    Expect(
        ResolveSerialQueueRole(7, distinct) ==
            static_cast<uint32_t>(SerialQueueRole::Copy),
        "copy queue family did not map to its stable role"
    );
    Expect(
        ResolveSerialQueueRole(ignored, distinct) ==
            static_cast<uint32_t>(SerialQueueRole::Ignored),
        "ignored queue family did not remain ignored"
    );

    SerialQueueFamilyMap aliased = distinct;
    aliased.compute_family = aliased.graphics_family;
    const uint32_t aliased_role = ResolveSerialQueueRole(aliased.graphics_family, aliased);
    Expect(
        aliased_role == (static_cast<uint32_t>(SerialQueueRole::Graphics) |
                         static_cast<uint32_t>(SerialQueueRole::Compute)),
        "aliased device queue family lost one of its stable roles"
    );
    const uint32_t unknown_role = ResolveSerialQueueRole(99, distinct);
    Expect(
        unknown_role == static_cast<uint32_t>(SerialQueueRole::Unknown) &&
            !IsCompleteSerialQueueRole(unknown_role),
        "unknown queue family did not fail closed"
    );

    AliasedTokens tokens = MakeAliasedTokens(0x2800);
    SerialBarrierItem forward = MakeBarrier(tokens.first, 3, 5);
    forward.src_queue_role = ResolveSerialQueueRole(3, distinct);
    forward.dst_queue_role = ResolveSerialQueueRole(5, distinct);
    SerialBarrierItem reverse = forward;
    std::swap(reverse.src_queue_role, reverse.dst_queue_role);

    SerialBarrierSectionBuilder forward_builder;
    forward_builder.Add(forward);
    const SerialGoldenSection forward_section = forward_builder.Finish();
    SerialBarrierSectionBuilder reverse_builder;
    reverse_builder.Add(reverse);
    const SerialGoldenSection reverse_section = reverse_builder.Finish();
    Expect(
        forward_section.digest != reverse_section.digest,
        "barrier queue-transfer direction was omitted from the digest"
    );

    SerialBarrierItem incomplete = forward;
    incomplete.src_queue_role      = unknown_role;
    incomplete.queue_roles_complete = false;
    SerialBarrierSectionBuilder incomplete_builder;
    incomplete_builder.Add(incomplete);
    Expect(
        !incomplete_builder.Finish().complete,
        "unknown barrier queue role did not fail the golden closed"
    );
}

SerialDescriptorItem MakeDescriptorItem(
    const StableSubmissionToken& _resource,
    uint32_t                     _binding = 2
) {
    SerialDescriptorItem item;
    item.bind_ordinal              = 3;
    item.bind_point                = 1;
    item.descriptor_set            = 0;
    item.binding                   = _binding;
    item.array_element             = 0;
    item.descriptor_type           = 6;
    item.param_idx                 = 7;
    item.declared_descriptor_count = 4;
    item.resource                  = _resource;
    item.resource_offset           = 16;
    item.resource_range            = 32;
    item.semantic_bytes            = {0x10, 0x20, 0x30, 0x40};
    return item;
}

SerialGoldenSection BuildDescriptorSection(const SerialDescriptorItem& _item) {
    SerialDescriptorSectionBuilder builder;
    builder.Add(_item);
    return builder.Finish();
}

void DescriptorDigestIncludesLayoutResourceAndSemanticBytes() {
    AliasedTokens tokens = MakeAliasedTokens(0x3000);
    const SerialDescriptorItem baseline_item = MakeDescriptorItem(tokens.first);
    const SerialGoldenSection baseline = BuildDescriptorSection(baseline_item);

    SerialDescriptorItem binding_changed_item = baseline_item;
    binding_changed_item.binding = 3;
    const SerialGoldenSection binding_changed = BuildDescriptorSection(binding_changed_item);
    SerialDescriptorItem resource_changed_item = baseline_item;
    resource_changed_item.resource = tokens.second;
    const SerialGoldenSection resource_changed = BuildDescriptorSection(resource_changed_item);
    SerialDescriptorItem bytes_changed_item = baseline_item;
    bytes_changed_item.semantic_bytes[2] ^= 0xff;
    const SerialGoldenSection bytes_changed = BuildDescriptorSection(bytes_changed_item);
    SerialDescriptorItem bind_point_changed_item = baseline_item;
    bind_point_changed_item.bind_point = 2;
    const SerialGoldenSection bind_point_changed =
        BuildDescriptorSection(bind_point_changed_item);
    SerialDescriptorItem param_changed_item = baseline_item;
    param_changed_item.param_idx = 8;
    const SerialGoldenSection param_changed = BuildDescriptorSection(param_changed_item);
    SerialDescriptorItem count_changed_item = baseline_item;
    count_changed_item.declared_descriptor_count = 8;
    const SerialGoldenSection count_changed = BuildDescriptorSection(count_changed_item);

    Expect(baseline.complete, "descriptor section is incomplete");
    Expect(
        baseline.digest != binding_changed.digest,
        "descriptor binding was omitted from the digest"
    );
    Expect(
        baseline.digest != resource_changed.digest,
        "descriptor resource was omitted from the digest"
    );
    Expect(
        baseline.digest != bytes_changed.digest,
        "descriptor semantic bytes were omitted from the digest"
    );
    Expect(
        baseline.digest != bind_point_changed.digest,
        "descriptor bind point was omitted from the digest"
    );
    Expect(
        baseline.digest != param_changed.digest,
        "descriptor binder parameter index was omitted from the digest"
    );
    Expect(
        baseline.digest != count_changed.digest,
        "declared descriptor count was omitted from the digest"
    );

    SerialDescriptorItem second_item = baseline_item;
    second_item.binding = 1;
    second_item.array_element = 1;
    second_item.semantic_bytes = {0xaa, 0xbb};
    SerialDescriptorSectionBuilder forward_builder;
    forward_builder.Add(baseline_item);
    forward_builder.Add(second_item);
    const SerialGoldenSection forward = forward_builder.Finish();
    SerialDescriptorSectionBuilder reverse_builder;
    reverse_builder.Add(second_item);
    reverse_builder.Add(baseline_item);
    const SerialGoldenSection reverse = reverse_builder.Finish();
    Expect(
        forward.digest == reverse.digest,
        "descriptor item input order changed canonical same-bind semantics"
    );
}

SerialGoldenSection BuildQuerySection(
    uint64_t         _frame,
    uint64_t         _absolute_slot,
    bool             _reverse = false,
    std::string_view _end_name = "GpuWork"
) {
    SerialQuerySectionBuilder builder;
    if (_reverse) {
        builder.AddEvent(SerialQueryEvent::End, _end_name, 1, 2, _frame, _absolute_slot + 1);
        builder.AddEvent(SerialQueryEvent::Begin, "GpuWork", 0, 1, _frame, _absolute_slot);
    } else {
        builder.AddEvent(SerialQueryEvent::Begin, "GpuWork", 0, 1, _frame, _absolute_slot);
        builder.AddEvent(SerialQueryEvent::End, _end_name, 1, 2, _frame, _absolute_slot + 1);
    }
    return builder.Finish();
}

void QueryDigestIgnoresRingPlacementButPreservesEvents() {
    const SerialGoldenSection baseline = BuildQuerySection(1, 64);
    const SerialGoldenSection next_frame = BuildQuerySection(99, 4096);
    const SerialGoldenSection reordered = BuildQuerySection(1, 64, true);
    const SerialGoldenSection renamed = BuildQuerySection(1, 64, false, "OtherWork");

    SerialQuerySectionBuilder different_command_builder;
    different_command_builder.AddEvent(SerialQueryEvent::Begin, "GpuWork", 0, 1, 1, 64, true, 7);
    different_command_builder.AddEvent(SerialQueryEvent::End, "GpuWork", 1, 2, 1, 65, true, 7);
    const SerialGoldenSection different_command = different_command_builder.Finish();

    SerialQuerySectionBuilder different_stage_builder;
    different_stage_builder.AddEvent(SerialQueryEvent::Begin, "GpuWork", 0, 4, 1, 64);
    different_stage_builder.AddEvent(SerialQueryEvent::End, "GpuWork", 1, 2, 1, 65);
    const SerialGoldenSection different_stage = different_stage_builder.Finish();

    Expect(
        baseline.digest == next_frame.digest,
        "query frame or absolute ring slot leaked into the digest"
    );
    Expect(baseline.digest != reordered.digest, "query event order did not affect the digest");
    Expect(baseline.digest != renamed.digest, "query event name did not affect the digest");
    Expect(baseline.digest != different_command.digest, "query command ordinal was omitted");
    Expect(baseline.digest != different_stage.digest, "query pipeline stage was omitted");
}

void CombinedDigestKeepsSectionIdentity() {
    const SerialCommandLayerSection commands = BuildAliasedCommandSection(0x4000);
    AliasedTokens tokens = MakeAliasedTokens(0x5000);

    SerialBarrierSectionBuilder barrier_builder;
    barrier_builder.Add(MakeBarrier(tokens.first, 1, 2));
    const SerialGoldenSection barriers = barrier_builder.Finish();
    const SerialGoldenSection descriptors =
        BuildDescriptorSection(MakeDescriptorItem(tokens.first, 0));
    const SerialGoldenSection queries = BuildQuerySection(0, 0);

    const SerialGoldenSummary baseline =
        MakeSerialGoldenSummary(commands, barriers, descriptors, queries);
    SerialGoldenSummary recombined = baseline;
    std::swap(recombined.barrier_digest, recombined.descriptor_digest);
    recombined.combined_digest = CombineSerialGoldenDigests(recombined);

    Expect(baseline.complete, "complete serial golden was marked incomplete");
    Expect(
        baseline.combined_digest != recombined.combined_digest,
        "cross-section digest recombination lost section identity"
    );
}

void UnresolvedAndOpaqueObjectsFailClosed() {
    SubmissionTokenTable table;
    const StableSubmissionToken unresolved =
        table.Resolve(StableObjectKind::Texture, FakePointer(0x6001));
    Expect(!unresolved.complete && !table.Complete(), "unresolved token table did not fail closed");
    table.Reset();
    const StableSubmissionToken opaque =
        table.Register(StableObjectKind::Opaque, FakePointer(0x6002));
    Expect(!opaque.complete && !table.Complete(), "opaque token table did not fail closed");

    SerialCommandLayerSectionBuilder command_builder;
    command_builder.BeginLayer(0);
    const std::array resources{unresolved};
    command_builder.AddCommand(Command::EType::ShaderDispatch, resources);
    command_builder.EndLayer();
    const SerialCommandLayerSection commands = command_builder.Finish();

    SerialBarrierSectionBuilder barrier_builder;
    const SerialGoldenSection barriers = barrier_builder.Finish();
    SerialDescriptorSectionBuilder descriptor_builder;
    SerialDescriptorItem descriptor = MakeDescriptorItem(opaque, 0);
    descriptor.semantic_bytes.clear();
    descriptor_builder.Add(descriptor);
    const SerialGoldenSection descriptors = descriptor_builder.Finish();
    SerialQuerySectionBuilder query_builder;
    const SerialGoldenSection queries = query_builder.Finish();
    const SerialGoldenSummary summary =
        MakeSerialGoldenSummary(commands, barriers, descriptors, queries);

    Expect(!commands.complete, "unresolved command resource did not fail closed");
    Expect(!descriptors.complete, "opaque descriptor resource did not fail closed");
    Expect(!summary.complete, "incomplete section produced a complete serial golden");
}

class MockBindlessArray final : public BindlessArray {
public:
    explicit MockBindlessArray(uint32_t* _discard_count) : discard_count(_discard_count) {}

    Moer::uint AllocateTexture(const TextureView&, Sampler) override { return 0; }
    Moer::uint AllocateBuffer(BufferView) override { return 0; }
    void UnbindTexture(Moer::uint) override {}
    void UnbindBuffer(Moer::uint) override {}
    Moer::uint64 ArrayHandle() const override { return 0; }

protected:
    Moer::UniquePtr<Command> CreateUpdateCommand() override { return {}; }
    void DiscardUpdateCommand(const Moer::Array<UpdateCmd>&) override { ++*discard_count; }
    void Destroy() override { delete this; }

private:
    uint32_t* discard_count;
};

std::unique_ptr<UpdateBindlessArrayCmd> MakeEmptyBindlessUpdate(BindlessArrayRef _array) {
    return std::make_unique<UpdateBindlessArrayCmd>(
        std::move(_array),
        Moer::Array<BindlessArray::UpdateCmd>{},
        Moer::Array<Moer::byte>{},
        Moer::Array<std::pair<Moer::uint, Moer::uint>>{},
        Moer::Array<Moer::byte>{},
        Moer::Array<std::pair<Moer::uint, Moer::uint>>{},
        Moer::Array<Moer::byte>{},
        Moer::Array<std::pair<Moer::uint, Moer::uint>>{}
    );
}

void BindlessUpdateLifecycleIsOneShotAndDiscardSafe() {
    uint32_t discard_count = 0;
    BindlessArrayRef array(new MockBindlessArray(&discard_count));

    {
        auto dropped = MakeEmptyBindlessUpdate(array);
    }
    Expect(discard_count == 1, "dropped bindless update did not roll back once");

    std::shared_ptr<std::atomic_bool> finalization_token;
    {
        auto handed_off = MakeEmptyBindlessUpdate(array);
        Expect(handed_off->HandOffUpdates(), "first bindless handoff was rejected");
        Expect(!handed_off->HandOffUpdates(), "bindless update allowed a second handoff");
        finalization_token = handed_off->UpdateFinalizationToken();
    }
    Expect(discard_count == 1, "handed-off bindless update was rolled back on destruction");
    Expect(
        !finalization_token->exchange(true),
        "bindless finalization token was consumed before retirement"
    );
    Expect(finalization_token->exchange(true), "bindless finalization token was not one-shot");
}

void BindlessGenerationClaimsRejectStaleAndSplitOwnership() {
    constexpr uint64_t token = 17;
    constexpr uint64_t generation = 41;
    using State = BindlessUpdateSlotState;

    Expect(
        ClassifyBindlessUpdateSlot(
            generation, generation, 0, 0, generation, generation, token
        ) == State::Original,
        "unclaimed bindless generation was not recognized as current"
    );
    Expect(
        ClassifyBindlessUpdateSlot(
            generation + 1,
            generation + 1,
            token,
            token,
            generation,
            generation,
            token
        ) == State::ClaimedByCommand,
        "matching bindless lifecycle claim was not recognized"
    );
    Expect(
        ClassifyBindlessUpdateSlot(
            generation + 1,
            generation + 1,
            token + 1,
            token + 1,
            generation,
            generation,
            token
        ) == State::Stale,
        "foreign bindless lifecycle claim was accepted"
    );
    Expect(
        ClassifyBindlessUpdateSlot(
            generation + 1,
            generation,
            token,
            0,
            generation,
            generation,
            token
        ) == State::Stale,
        "split global/typed bindless ownership was accepted"
    );
    Expect(
        ClassifyBindlessUpdateSlot(
            generation + 2,
            generation + 2,
            0,
            0,
            generation,
            generation,
            token
        ) == State::Stale,
        "reused bindless generation was accepted by an old command"
    );
    Expect(
        ClassifyBindlessUpdateSlot(
            generation, generation, 0, 0, generation, generation, 0
        ) == State::Stale,
        "tokenless bindless update was accepted"
    );
    constexpr uint64_t exhausted = std::numeric_limits<uint64_t>::max();
    Expect(
        ClassifyBindlessUpdateSlot(
            exhausted, exhausted, token, token, exhausted, exhausted, token
        ) == State::Stale,
        "exhausted bindless generation wrapped into a valid claim"
    );

    using Action = BindlessUpdateFinalizationAction;
    Expect(
        GetBindlessUpdateFinalizationAction(State::Original, false, true) == Action::Ignore,
        "successful bindless allocation was retired"
    );
    Expect(
        GetBindlessUpdateFinalizationAction(State::ClaimedByCommand, true, true) ==
            Action::Retire,
        "successful bindless free was not retired at GPU completion"
    );
    Expect(
        GetBindlessUpdateFinalizationAction(State::Original, false, false) == Action::Retire,
        "rejected bindless allocation was not cancelled"
    );
    Expect(
        GetBindlessUpdateFinalizationAction(State::ClaimedByCommand, true, false) ==
            Action::Restore,
        "rejected bindless free was retired without GPU ordering"
    );
    Expect(
        GetBindlessUpdateFinalizationAction(State::Stale, true, false) == Action::Ignore,
        "stale bindless update was allowed to mutate current ownership"
    );

    uint64_t array_generation = generation + 1;
    uint64_t typed_generation = generation + 1;
    uint64_t array_claim      = token;
    uint64_t typed_claim      = token;
    Expect(
        RollbackBindlessUpdateSlotClaim(
            array_generation,
            typed_generation,
            array_claim,
            typed_claim,
            generation,
            generation,
            token
        ),
        "owned bindless lifecycle claim could not be rolled back"
    );
    Expect(
        array_generation == generation && typed_generation == generation &&
            array_claim == 0 && typed_claim == 0,
        "bindless lifecycle rollback did not restore the original generation"
    );
    array_generation = generation + 1;
    typed_generation = generation + 1;
    array_claim      = token + 1;
    typed_claim      = token + 1;
    Expect(
        !RollbackBindlessUpdateSlotClaim(
            array_generation,
            typed_generation,
            array_claim,
            typed_claim,
            generation,
            generation,
            token
        ),
        "foreign bindless lifecycle claim was rolled back"
    );
}

} // namespace

int main() {
    try {
        CapabilityTableIsExplicitAndFailClosed();
        ParallelReplayContractExcludesSideEffects();
        TopologyDigestIsDeterministicAndOrderSensitive();
        PredictionRejectsSingleUnitAndModelsEligibleLayer();
        StableTokensIgnorePointersAndCaptureAliasTopology();
        BarrierDigestIsOrderIndependentButTokenSensitive();
        QueueFamilyRolesAreStableCompleteAndDirectional();
        DescriptorDigestIncludesLayoutResourceAndSemanticBytes();
        QueryDigestIgnoresRingPlacementButPreservesEvents();
        CombinedDigestKeepsSectionIdentity();
        UnresolvedAndOpaqueObjectsFailClosed();
        BindlessUpdateLifecycleIsOneShotAndDiscardSafe();
        BindlessGenerationClaimsRejectStaleAndSplitOwnership();
        std::cout << "RHI record diagnostic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI record diagnostic test failed: " << error.what() << '\n';
        return 1;
    }
}
