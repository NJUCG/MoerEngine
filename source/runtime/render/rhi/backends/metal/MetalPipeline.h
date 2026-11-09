#pragma once

#include "misc/Types.h"
#include "rhi/backends/metal/MetalFormat.h"

#include <utility>
#include <vector>

namespace Moer::Render {
// One logical argument mapping in a single shader stage.
struct MetalShaderBinding {
    uint32         set{0};
    uint32         binding{0};
    EShaderArgType kind{SDA_Num};
    bool active{false};
    bool constant{false};
    bool texel_buffer{false};
};

// Per-stage bindless tables, scalar resources and constant upload location.
struct MetalStageBindings {
    std::vector<std::pair<NSUInteger, uint32>> bindless_sets;
    std::vector<MetalShaderBinding> scalar_bindings;
    NSUInteger constant_buffer_index{0};
    uint32                                     constant_byte_size{0};
};

class MetalPipelineState final : public PipelineState {
public:
    // Graphics and compute construction retain the corresponding native functions.
    MetalPipelineState(
        id<MTLRenderPipelineState>  pipeline,
        std::vector<MTLPixelFormat> color_formats,
        uint32                      vertex_bindings,
        RHIRasterizeInfo            rasterizer,
        MTLPixelFormat              depth_format,
        id<MTLDepthStencilState>    depth_state,
        id<MTLFunction>             vertex_function,
        id<MTLFunction>             fragment_function,
        MetalStageBindings         vertex_bindings_layout,
        MetalStageBindings         fragment_bindings_layout,
        uint64                      active_argument_bits
    ) :
        PipelineState(active_argument_bits),
        render_(pipeline),
        color_formats_(std::move(color_formats)),
        vertex_bindings_(vertex_bindings),
        rasterizer_(rasterizer),
        depth_format_(depth_format),
        depth_state_(depth_state),
        vertex_function_(vertex_function),
        fragment_function_(fragment_function),
        vertex_bindings_layout_(std::move(vertex_bindings_layout)),
        fragment_bindings_layout_(std::move(fragment_bindings_layout)) {}
    MetalPipelineState(
        id<MTLComputePipelineState> pipeline,
        id<MTLFunction>             function,
        MetalStageBindings         bindings,
        MTLSize                     local_size,
        uint64                      active_argument_bits
    ) :
        PipelineState(active_argument_bits),
        compute_(pipeline),
        compute_function_(function),
        compute_bindings_layout_(std::move(bindings)),
        local_size_(local_size) {}
    // Native execution handles.
    id<MTLRenderPipelineState> NativeRender() const noexcept { return render_; }
    id<MTLComputePipelineState> NativeCompute() const noexcept { return compute_; }

    // Compute stage argument layout and dispatch shape.
    id<MTLFunction> ComputeFunction() const noexcept { return compute_function_; }
    const std::vector<MetalShaderBinding>& ComputeBindings() const noexcept {
        return compute_bindings_layout_.scalar_bindings;
    }
    const MetalStageBindings& ComputeBindingsLayout() const noexcept {
        return compute_bindings_layout_;
    }
    const std::vector<std::pair<NSUInteger, uint32>>& ComputeBindlessSets() const noexcept {
        return compute_bindings_layout_.bindless_sets;
    }
    MTLSize LocalSize() const noexcept { return local_size_; }
    NSUInteger ConstantBufferIndex() const noexcept {
        return compute_bindings_layout_.constant_buffer_index;
    }

    // Graphics attachments, rasterization and shader-stage layouts.
    const std::vector<MTLPixelFormat>& ColorFormats() const noexcept { return color_formats_; }
    uint32                             VertexBindingCount() const noexcept {
        return vertex_bindings_;
    }
    const RHIRasterizeInfo& Rasterizer() const noexcept { return rasterizer_; }
    MTLPixelFormat DepthFormat() const noexcept { return depth_format_; }
    id<MTLDepthStencilState> DepthState() const noexcept { return depth_state_; }
    id<MTLFunction> VertexFunction() const noexcept { return vertex_function_; }
    id<MTLFunction> FragmentFunction() const noexcept { return fragment_function_; }
    const MetalStageBindings& VertexBindingsLayout() const noexcept {
        return vertex_bindings_layout_;
    }
    const MetalStageBindings& FragmentBindingsLayout() const noexcept {
        return fragment_bindings_layout_;
    }

    // Optional tessellation capture and hull stages.
    bool IsTessellation() const noexcept { return hull_pipeline_ != nil; }
    void SetTessellation(
        id<MTLComputePipelineState> vertex_pipeline,
        id<MTLFunction>             vertex_function,
        id<MTLComputePipelineState> hull_pipeline,
        id<MTLFunction>             hull_function,
        MetalStageBindings         tess_vertex_bindings,
        MetalStageBindings         hull_bindings,
        uint32                      patch_points,
        uint32                      point_stride
    ) {
        tess_vertex_pipeline_ = vertex_pipeline;
        tess_vertex_function_ = vertex_function;
        hull_pipeline_ = hull_pipeline;
        hull_function_ = hull_function;
        tess_vertex_bindings_layout_ = std::move(tess_vertex_bindings);
        hull_bindings_layout_ = std::move(hull_bindings);
        patch_points_ = patch_points;
        point_stride_ = point_stride;
    }
    id<MTLComputePipelineState> TessVertexPipeline() const noexcept { return tess_vertex_pipeline_; }
    id<MTLFunction> TessVertexFunction() const noexcept { return tess_vertex_function_; }
    id<MTLComputePipelineState> HullPipeline() const noexcept { return hull_pipeline_; }
    id<MTLFunction> HullFunction() const noexcept { return hull_function_; }
    const MetalStageBindings& HullBindingsLayout() const noexcept { return hull_bindings_layout_; }
    const MetalStageBindings& TessVertexBindingsLayout() const noexcept { return tess_vertex_bindings_layout_; }
    uint32                      PatchPoints() const noexcept {
        return patch_points_;
    }
    uint32 PointStride() const noexcept {
        return point_stride_;
    }

private:
    // Native execution handles.
    id<MTLRenderPipelineState> render_{nil};
    id<MTLComputePipelineState> compute_{nil};

    // Compute reflection and dispatch metadata.
    id<MTLFunction> compute_function_{nil};
    MetalStageBindings compute_bindings_layout_;
    MTLSize local_size_{MTLSizeMake(0, 0, 0)};

    // Graphics render-target compatibility and fixed state.
    std::vector<MTLPixelFormat> color_formats_;
    uint32                      vertex_bindings_{0};
    RHIRasterizeInfo rasterizer_{};
    MTLPixelFormat depth_format_{MTLPixelFormatInvalid};
    id<MTLDepthStencilState> depth_state_{nil};

    // Graphics stage functions and their binding metadata.
    id<MTLFunction> vertex_function_{nil};
    id<MTLFunction> fragment_function_{nil};
    MetalStageBindings vertex_bindings_layout_{};
    MetalStageBindings fragment_bindings_layout_{};

    // Tessellation intermediate stage functions and patch layout.
    id<MTLComputePipelineState> tess_vertex_pipeline_{nil};
    id<MTLFunction> tess_vertex_function_{nil};
    id<MTLComputePipelineState> hull_pipeline_{nil};
    id<MTLFunction> hull_function_{nil};
    MetalStageBindings tess_vertex_bindings_layout_{};
    MetalStageBindings hull_bindings_layout_{};
    uint32                      patch_points_{0};
    uint32                      point_stride_{0};
};

PipelineHandle CreateMetalGraphicsPipeline(id<MTLDevice> device,
    GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info);
PipelineHandle CreateMetalComputePipeline(id<MTLDevice> device,
    PipelineShaderInfo&& shader_info);
} // namespace Moer::Render
