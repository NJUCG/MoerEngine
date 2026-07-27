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

inline const SchemaDescriptor& GpuScope() {
    static const SchemaDescriptor schema{
        .name           = "GpuScope",
        .event_type     = "timing.gpu_scope",
        .kind           = EventKind::Scope,
        .channel        = Channel::GpuQueue,
        .schema_version = 1,
        .fields =
            {
                {"frame_id", FieldType::UInt64},
                {"queue_domain", FieldType::UInt64},
                {"name", FieldType::String},
                {"begin_tick", FieldType::UInt64},
                {"end_tick", FieldType::UInt64},
                {"tick_period_ns", FieldType::Float64},
                {"depth", FieldType::UInt32},
            },
    };
    return schema;
}

} // namespace Moer::ProfileDump::Templates

#endif // MOER_ENGINE_PROFILE_DUMP_TEMPLATES_H
