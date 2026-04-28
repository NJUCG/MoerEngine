#pragma once

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "shaderheaders/shared/raster/culling/ShaderParameters.h"

#include <string>
#include <string_view>

namespace Moer::Render::Raster {

// Groups the per-pass buffers used by GPU frustum culling and counted indirect draws.
struct GpuCullingBuffers {
    // Stores the visibility remap, indirect draw commands, and counters for one raster view.
    struct VisibilitySet {
        BufferRef        draw_cmd_buf;
        BufferWithHandle visible_instance_id_buf;
        BufferRef        counter_buf;

        uint max_draw_count     = 0;
        uint max_instance_count = 0;

        // Returns the uint counter view consumed by counted indirect draw calls.
        BufferView GetDrawCountView() const {
            return counter_buf->GetView(0, sizeof(uint));
        }

        // Ensures the visibility buffers are large enough and rebound after reallocations.
        void EnsureCapacity(
            RenderDevice&     device,
            BindlessArrayRef& bdls,
            CommandList&      cmd_list,
            std::string_view  debug_name_prefix,
            uint              draw_count,
            uint              instance_count
        ) {
            const uint target_draw_count     = draw_count == 0 ? 1u : draw_count;
            const uint target_instance_count = instance_count == 0 ? 1u : instance_count;

            bool need_bindless_update = false;

            if (draw_cmd_buf == nullptr || max_draw_count < target_draw_count) {
                draw_cmd_buf = device.CreateBuffer<DrawIndexedCmdData>(
                    std::string(debug_name_prefix) + "::DrawCommands",
                    target_draw_count,
                    EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
                );
                max_draw_count = target_draw_count;
            }

            if (visible_instance_id_buf.buf == nullptr || max_instance_count < target_instance_count) {
                if (visible_instance_id_buf.hdl != 0) {
                    bdls->UnbindBuffer(visible_instance_id_buf.hdl);
                    visible_instance_id_buf.hdl = 0;
                }

                visible_instance_id_buf.buf = device.CreateBuffer<uint>(
                    std::string(debug_name_prefix) + "::VisibleInstanceIds",
                    target_instance_count,
                    EBufferUsageFlags::UNORDERED_ACCESS
                );
                visible_instance_id_buf.hdl = bdls->AllocateBuffer(visible_instance_id_buf.buf->GetView());
                max_instance_count          = target_instance_count;
                need_bindless_update        = true;
            }

            if (counter_buf == nullptr) {
                counter_buf = device.CreateBuffer<GpuCullingCounterData>(
                    std::string(debug_name_prefix) + "::Counters",
                    1,
                    EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER
                );
            }

            if (need_bindless_update) {
                cmd_list.UpdateBindlessArray(bdls);
            }
        }
    };

    VisibilitySet geometry;
    VisibilitySet shadow;
};

} // namespace Moer::Render::Raster