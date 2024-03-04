#ifndef MOER_PASS_COMMON_H
#define MOER_PASS_COMMON_H
#include "rhi/RHICommand.h"
namespace Moer {
    class RenderResources {};
    struct PassInput {
        RHIGraphicsCommandList* cmd_list;
        RenderResources*        render_resources;
        uint64_t                frame_index;
        uint32_t                frame_offset;
    };
}// namespace Moer

#endif