#ifndef MOER_RHI_UI_IMPLEMENT_H
#define MOER_RHI_UI_IMPLEMENT_H

#include "PixelFormat.h"
#include "rhi/RHICommandList.h"
#include "rhi/RHICommandQueue.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"

struct GuiFrameRenderBuffers {

    RHIBufferRef vertex_buffer;
    RHIBufferRef index_buffer;
};
struct GuiBackendData {
    size_t buffer_memory_alignment;

    RHIGraphicsPipelineStateRef pipeline;
    RHIVertexShaderRef          shader_module_vert;
    RHIFragmentShaderRef        shader_module_frag;

    // Font data
    RHISamplerRef            font_sampler;
    RHITextureRef            font_texture;
    RHIShaderResourceViewRef font_view;
    RHIBufferRef             upload_buffer;

    // Render buffers for main window
    GuiFrameRenderBuffers* main_viewport_render_buffers;
    RHIViewport*           main_viewport;

    EPixelFormat attachment_format;
    uint32_t     num_frames_in_flight;

    GuiBackendData() {
        memset((void*)this, 0, sizeof(*this));
        buffer_memory_alignment = 256;
    }
    ~GuiBackendData();
};

struct GuiViewportData {

    RHICommandQueue*        command_queue;
    RHIGraphicsCommandList* comand_list;

    RHIGraphicsCommandList* upload_command_list;

    RHIFenceRef present_fence;

    RHIViewportRef viewport;

    GuiFrameRenderBuffers* render_buffers;// Used by all viewports

    uint64_t        frame_index;
    uint32_t        viewport_index;
    static uint32_t viewport_count;

    GuiViewportData(uint32_t _frame_in_flight) {
        memset((void*)this, 0, sizeof(*this));
        render_buffers = new GuiFrameRenderBuffers[_frame_in_flight];
        for (uint32_t i = 0; i < _frame_in_flight; ++i) {
            render_buffers[i].vertex_buffer = nullptr;
            render_buffers[i].index_buffer  = nullptr;
        }
        viewport_index = viewport_count;
        viewport_count++;
    }
    ~GuiViewportData() {
        delete[] render_buffers;
        viewport_count--;
    }
};

#endif