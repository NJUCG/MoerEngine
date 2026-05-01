#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"

namespace Moer {

// 标记 cache 文件属于 origin cache 还是 state cache
enum class ESceneCacheKind : uint32 {
    Origin = 0,
    State  = 1,
};

// 描述 scene cache 文件头的固定二进制布局
struct RENDER_API SceneCacheHeader {
    char   magic[16]      = {'M', 'O', 'E', 'R', '_', 'S', 'C', 'E', 'N', 'E', '_', 'C', 'A', 'C', 'H', 'E'};
    uint32 format_version = 1;
    uint32 header_size    = sizeof(SceneCacheHeader);
    uint32 kind           = static_cast<uint32>(ESceneCacheKind::Origin);
    uint32 reserved0      = 0;
    uint64 source_identity_hash        = 0;
    uint64 source_file_size            = 0;
    int64  source_file_last_write_time = 0;
    uint64 payload_size                = 0;
    uint64 reserved[8]                 = {};
};

static_assert(sizeof(SceneCacheHeader) == 128);

} // namespace Moer