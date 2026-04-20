#pragma once

#include "misc/Traits.h"
#include "shaderheaders/shared/rhi/CommandDrawData.h"


namespace Moer::Render {

struct SingleDrawParam {
    uint index_cnt;
    uint instance_cnt;
    uint first_index;
    uint vertex_offset;
    uint first_instance;
};

struct DrawCmdData {
    uint vertex_cnt;
    uint instance_cnt;
    uint first_vtx;
    uint first_instance;
};

} // namespace Moer::Render