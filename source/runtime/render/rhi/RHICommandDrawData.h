#pragma once

#include "misc/Traits.h"
#include "misc/Types.h"
#include "shaderheaders/shared/rhi/CommandDrawData.h"

namespace Moer::Render {

struct SingleDrawParam {
    uint32 index_cnt;
    uint32 instance_cnt;
    uint32 first_index;
    uint32 vertex_offset;
    uint32 first_instance;
};

struct DrawCmdData {
    uint32 vertex_cnt;
    uint32 instance_cnt;
    uint32 first_vtx;
    uint32 first_instance;
};

} // namespace Moer::Render