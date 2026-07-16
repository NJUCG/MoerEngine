#pragma once

#include "rhi/RHIRecordDiagnostics.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace Moer::Render {

struct ArrayArguments;
struct PipelineHandle;
struct VulkanPipelineParamBinder;

// Profile-only recorder for the Phase 9.0 serial baseline. Native handles are
// accepted only as lookup keys and never enter a digest.
class VulkanSerialGoldenTrace {
public:
    VulkanSerialGoldenTrace();
    ~VulkanSerialGoldenTrace();

    VulkanSerialGoldenTrace(const VulkanSerialGoldenTrace&)            = delete;
    VulkanSerialGoldenTrace& operator=(const VulkanSerialGoldenTrace&) = delete;

    void BeginLayer(uint32_t _layer_ordinal);
    void PrimeCommandResources(
        const Command*         _command,
        uint32_t               _original_ordinal,
        const TCachedArgArray& _cached_args
    );
    void RecordCommand(
        const Command*        _command,
        uint32_t              _original_ordinal,
        const TCachedArgArray& _cached_args
    );
    void RegisterDerivedResources(const Command* _command);
    void EndLayer();

    void SetCurrentCommand(uint32_t _original_ordinal);

    StableSubmissionToken ResolveNativeBuffer(uint64_t _native_handle);
    StableSubmissionToken ResolveNativeImage(uint64_t _native_handle);
    void AddBarrier(const SerialBarrierItem& _item);
    void RecordUnresolvedBufferBarrier(const SerialBarrierItem& _item);

    void RecordDescriptorBind(
        const PipelineHandle&             _pipeline,
        const ArrayArguments&             _args,
        const VulkanPipelineParamBinder&  _binder
    );
    // Called only from ExecuteNow's profile-on branch, after VisitCmd returned.
    // Replays the descriptor calls that the successful command just emitted.
    void RecordDescriptorsForCommand(
        const Command*         _command,
        const TCachedArgArray& _cached_args,
        uint64_t               _relative_descriptor_begin,
        uint64_t               _actual_descriptor_bytes
    );
    void RecordQueryEvent(
        SerialQueryEvent _event,
        std::string_view _name,
        uint64_t         _pipeline_stage_mask
    );

    void MarkOpaque();
    void MarkUnresolved();
    uint32_t OpaqueCount() const;
    uint32_t UnresolvedCount() const;
    uint64_t OpaqueCommandMask() const;
    uint64_t UnresolvedCommandMask() const;
    uint32_t UnresolvedNativeBufferCount() const;
    uint32_t UnresolvedNativeImageCount() const;
    const SerialBarrierItem& FirstUnresolvedBufferBarrier() const;
    bool HasUnresolvedBufferBarrier() const;

    SerialGoldenSummary Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Moer::Render
