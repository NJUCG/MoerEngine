#ifndef MOER_ENGINE_PROFILE_DUMP_TEMPLATES_H
#define MOER_ENGINE_PROFILE_DUMP_TEMPLATES_H

#include "profile/ProfileDump.h"

#include <array>
#include <span>

namespace Moer::ProfileDump::Templates {

inline const SchemaDescriptor& CpuScope() {
    static const SchemaDescriptor schema{
        .name           = "CpuScope",
        .event_type     = "timing.cpu_scope",
        .kind           = EventKind::Scope,
        .channel        = Channel::CpuThread,
        .schema_version = 1,
        .fields =
            {
                {"thread_id", FieldType::UInt64},
                {"name", FieldType::String},
                {"begin_ns", FieldType::UInt64},
                {"end_ns", FieldType::UInt64},
                {"depth", FieldType::UInt32},
            },
    };
    return schema;
}

inline const SchemaDescriptor& GpuFrame() {
    static const SchemaDescriptor schema{
        .name           = "GpuFrame",
        .event_type     = "timing.gpu_frame",
        .kind           = EventKind::Instant,
        .channel        = Channel::GpuQueue,
        .schema_version = 1,
        .fields =
            {
                {"frame_id", FieldType::UInt64},
                {"capture_status", FieldType::UInt32},
                {"valid", FieldType::Bool},
                {"admitted_scope_count", FieldType::UInt64},
                {"dropped_scope_count", FieldType::UInt64},
                {"error_scope_count", FieldType::UInt64},
                {"reason", FieldType::String},
            },
    };
    return schema;
}

inline const SchemaDescriptor& GpuScope() {
    static const SchemaDescriptor schema{
        .name           = "GpuScope",
        .event_type     = "timing.gpu_scope",
        .kind           = EventKind::Scope,
        .channel        = Channel::GpuQueue,
        .schema_version = 2,
        .fields =
            {
                {"frame_id", FieldType::UInt64},
                {"scope_id", FieldType::UInt64},
                {"parent_scope_id", FieldType::UInt64},
                {"source_order", FieldType::UInt64},
                {"local_order", FieldType::UInt64},
                {"logical_queue", FieldType::UInt32},
                {"native_queue_id", FieldType::UInt32},
                {"family_id", FieldType::UInt32},
                {"name", FieldType::String},
                {"status", FieldType::UInt32},
                {"begin_tick", FieldType::UInt64},
                {"end_tick", FieldType::UInt64},
                {"valid_bits", FieldType::UInt32},
                {"tick_period_ns", FieldType::Float64},
                {"total_duration_ns", FieldType::Float64},
                {"exclusive_duration_ns", FieldType::Float64},
                {"depth", FieldType::UInt32},
                {"error_reason", FieldType::String},
            },
    };
    return schema;
}

} // namespace Moer::ProfileDump::Templates

#endif // MOER_ENGINE_PROFILE_DUMP_TEMPLATES_H
