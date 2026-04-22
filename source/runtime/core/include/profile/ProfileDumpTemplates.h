#pragma once

#include "profile/ProfileDump.h"

namespace Moer::ProfileDump::Templates {

BEGIN_TIMING_EVENT_TEMPLATE(CpuScopeTemplate, "timing.cpu_scope", EChannel::CPUThread, 1)
    PROFILE_DUMP_FIELD(uint64_t, thread_id)
    PROFILE_DUMP_FIELD(std::string_view, name)
    PROFILE_DUMP_FIELD(int64_t, start_us)
    PROFILE_DUMP_FIELD(int64_t, duration_us)
    PROFILE_DUMP_FIELD(uint32_t, depth)
END_TIMING_EVENT_TEMPLATE()

BEGIN_TIMING_EVENT_TEMPLATE(GpuScopeTemplate, "timing.gpu_scope", EChannel::GPUQueue, 1)
    PROFILE_DUMP_FIELD(uint64_t, frame_index)
    PROFILE_DUMP_FIELD(std::string_view, queue_name)
    PROFILE_DUMP_FIELD(std::string_view, name)
    PROFILE_DUMP_FIELD(uint64_t, start_ns)
    PROFILE_DUMP_FIELD(uint64_t, end_ns)
    PROFILE_DUMP_FIELD(uint32_t, depth)
    PROFILE_DUMP_FIELD(uint64_t, total_busy_ns)
    PROFILE_DUMP_FIELD(uint64_t, exclusive_ns)
END_TIMING_EVENT_TEMPLATE()

} // namespace Moer::ProfileDump::Templates