#ifndef MOER_RHI_UI_IMPLEMENT_H
#define MOER_RHI_UI_IMPLEMENT_H

#include "rhi/RHICommandList.h"
#include "rhi/RHIResource.h"

struct GuiFrameRenderBuffers {

    RHIBufferRef vertex_buffer;
    RHIBufferRef index_buffer;
};
struct GuiWindowRenderBuffers {
    uint32_t               index;
    uint32_t               count;
    GuiFrameRenderBuffers* frame_render_buffers;
};
struct GuiBackendData {
    size_t buffer_memory_alignment;

    RHIGraphicsPipelineStateRef pipeline;
    RHIShaderRef                shader_module_vert;
    RHIShaderRef                shader_module_frag;

    // Font data
    RHISamplerRef            font_sampler;
    RHITextureRef            font_texture;
    RHIShaderResourceViewRef font_view;
    RHIBufferRef             upload_buffer;

    // Render buffers for main window
    GuiWindowRenderBuffers main_window_render_buffers;
    uint32_t               num_frames_in_flight;

    GuiBackendData() {
        memset((void*)this, 0, sizeof(*this));
        buffer_memory_alignment = 256;
    }
};

struct GuiFrameContext {
    RHIResource* attachment;
};

struct GuiViewportData {
    int width;
    int height;

    RHIGraphicsCommandList* comand_list;
    RHIFence*               fence;

    uint32_t         num_frames_in_flight;
    GuiFrameContext* frames;

    uint32_t               frame_index;   // Current frame being rendered to (0 <= FrameIndex < FrameInFlightCount)
    GuiFrameRenderBuffers* render_buffers;// Used by all viewports

    GuiViewportData(uint32_t _num_frames_in_flight) : num_frames_in_flight(_num_frames_in_flight) {
        memset(&render_buffers, 0, sizeof(render_buffers));
    }
    ~GuiViewportData() {}
};
#endif