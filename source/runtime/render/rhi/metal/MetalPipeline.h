#pragma once

#include "rhi/metal/MetalFormat.h"

#include <utility>
#include <vector>

namespace Moer::Render {
struct MetalComputeBinding {
    uint set{0};
    uint binding{0};
    EShaderArgType kind{SDA_Num};
    bool active{false};
    bool constant{false};
    bool texel_buffer{false};
};

struct MetalRenderBindings {
    std::vector<std::pair<NSUInteger, uint>> bindless_sets;
    std::vector<MetalComputeBinding> scalar_bindings;
    NSUInteger constant_buffer_index{0};
};

class MetalPipelineState final : public PipelineState {
public:
    MetalPipelineState(
        id<MTLRenderPipelineState> pipeline,
        std::vector<MTLPixelFormat> color_formats,
        uint vertex_bindings,
        RHIRasterizeInfo rasterizer,
        MTLPixelFormat depth_format,
        id<MTLDepthStencilState> depth_state,
        id<MTLFunction> vertex_function,
        id<MTLFunction> fragment_function,
        MetalRenderBindings vertex_bindings_layout,
        MetalRenderBindings fragment_bindings_layout
    ) : render_(pipeline), color_formats_(std::move(color_formats)),
        vertex_bindings_(vertex_bindings), rasterizer_(rasterizer),
        depth_format_(depth_format), depth_state_(depth_state),
        vertex_function_(vertex_function), fragment_function_(fragment_function),
        vertex_bindings_layout_(std::move(vertex_bindings_layout)),
        fragment_bindings_layout_(std::move(fragment_bindings_layout)) {}
    MetalPipelineState(
        id<MTLComputePipelineState> pipeline, id<MTLFunction> function,
        MetalRenderBindings bindings, MTLSize local_size
    ) : compute_(pipeline), compute_function_(function),
        compute_bindings_layout_(std::move(bindings)), local_size_(local_size) {}
    id<MTLRenderPipelineState> NativeRender() const noexcept { return render_; }
    id<MTLComputePipelineState> NativeCompute() const noexcept { return compute_; }
    id<MTLFunction> ComputeFunction() const noexcept { return compute_function_; }
    const std::vector<MetalComputeBinding>& ComputeBindings() const noexcept {
        return compute_bindings_layout_.scalar_bindings;
    }
    const std::vector<std::pair<NSUInteger, uint>>& ComputeBindlessSets() const noexcept {
        return compute_bindings_layout_.bindless_sets;
    }
    MTLSize LocalSize() const noexcept { return local_size_; }
    NSUInteger ConstantBufferIndex() const noexcept {
        return compute_bindings_layout_.constant_buffer_index;
    }
    const std::vector<MTLPixelFormat>& ColorFormats() const noexcept { return color_formats_; }
    uint VertexBindingCount() const noexcept { return vertex_bindings_; }
    const RHIRasterizeInfo& Rasterizer() const noexcept { return rasterizer_; }
    MTLPixelFormat DepthFormat() const noexcept { return depth_format_; }
    id<MTLDepthStencilState> DepthState() const noexcept { return depth_state_; }
    id<MTLFunction> VertexFunction() const noexcept { return vertex_function_; }
    id<MTLFunction> FragmentFunction() const noexcept { return fragment_function_; }
    const MetalRenderBindings& VertexBindingsLayout() const noexcept {
        return vertex_bindings_layout_;
    }
    const MetalRenderBindings& FragmentBindingsLayout() const noexcept {
        return fragment_bindings_layout_;
    }

private:
    id<MTLRenderPipelineState> render_{nil};
    id<MTLComputePipelineState> compute_{nil};
    id<MTLFunction> compute_function_{nil};
    MetalRenderBindings compute_bindings_layout_;
    MTLSize local_size_{MTLSizeMake(0, 0, 0)};
    std::vector<MTLPixelFormat> color_formats_;
    uint vertex_bindings_{0};
    RHIRasterizeInfo rasterizer_{};
    MTLPixelFormat depth_format_{MTLPixelFormatInvalid};
    id<MTLDepthStencilState> depth_state_{nil};
    id<MTLFunction> vertex_function_{nil};
    id<MTLFunction> fragment_function_{nil};
    MetalRenderBindings vertex_bindings_layout_{};
    MetalRenderBindings fragment_bindings_layout_{};
};

PipelineHandle CreateMetalGraphicsPipeline(id<MTLDevice> device,
    GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info);
PipelineHandle CreateMetalComputePipeline(id<MTLDevice> device,
    PipelineShaderInfo&& shader_info);
} // namespace Moer::Render
