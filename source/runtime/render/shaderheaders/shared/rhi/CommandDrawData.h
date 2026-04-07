/**
 * 请统一Include如下文件，不要Include当前文件
 * CPP:
 *     #include "shaderheaders/shared/rhi/CommandDrawData.h"
 * HLSL:
 *     #include "shared/rhi/CommandDrawData.h"
 */
#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

// 定义供 C++ 与 HLSL 共用的带索引间接绘制命令布局。
struct DrawIndexedCmdData {
    uint index_cnt;
    uint instance_cnt;
    uint first_index;
    uint vertex_offset;
    uint first_instance;
};

#ifdef __cplusplus
}
#else
}
#endif