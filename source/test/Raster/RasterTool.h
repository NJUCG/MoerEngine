#pragma once

#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"

namespace Moer::Render {

    class RasterTool {
    public:
        static Array<SingleDrawParam> GetFullScreenDrawDatas() {
            Array<SingleDrawParam> full_screen_draw_datas;
            full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
            return full_screen_draw_datas;
        }
    };

} // namespace Moer::Render