#include "rhi/metal/MetalDraw.h"
#include "rhi/metal/MetalBindless.h"
#include "rhi/metal/MetalResource.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Moer::Render {
namespace {
struct TessellationBuffers {
    id<MTLBuffer> control_points{nil};
    id<MTLBuffer> hull_points{nil};
    id<MTLBuffer> factors{nil};
    uint32_t patch_count{0};
};

void BindTessComputeArgs(
    id<MTLCommandBuffer> command_buffer, id<MTLComputeCommandEncoder> encoder,
    id<MTLFunction> function, const MetalRenderBindings& layout,
    const SetDrawStateCmd& draw, NSMutableArray<id<MTLBuffer>>* staging_buffers
) {
    if (!layout.bindless_sets.empty() || !draw.Args().constants.empty()) {
        Unsupported("Metal tessellation compute bindless or push constants");
    }
    for (NSUInteger set = 0; set < layout.constant_buffer_index; ++set) {
        bool needed = false;
        for (const auto& binding : layout.scalar_bindings) {
            needed |= binding.active && binding.set == set;
        }
        if (!needed) continue;
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:set];
        if (arguments == nil) Unsupported("Metal tessellation argument layout");
        id<MTLBuffer> table = [command_buffer.device
            newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        if (table == nil) throw std::runtime_error("Cannot allocate Metal tessellation arguments");
        [staging_buffers addObject:table];
        [arguments setArgumentBuffer:table offset:0];
        for (uint index = 0; index < layout.scalar_bindings.size(); ++index) {
            const auto& binding = layout.scalar_bindings[index];
            if (!binding.active || binding.set != set) continue;
            if ((binding.kind != SDA_Buffer && binding.kind != SDA_ConstantBuffer) ||
                binding.texel_buffer || index >= draw.Args().args.size()) {
                Unsupported("Metal tessellation compute resource kind");
            }
            const auto* view = std::get_if<BufferView>(&draw.Args().args[index]);
            auto* buffer = view == nullptr ? nullptr : dynamic_cast<MetalBuffer*>(view->GetBuffer());
            if (buffer == nullptr) Unsupported("Metal tessellation buffer argument");
            [arguments setBuffer:buffer->Native() offset:view->GetByteOffset()
                         atIndex:binding.binding];
            [encoder useResource:buffer->Native() usage:MTLResourceUsageRead];
        }
        [encoder setBuffer:table offset:0 atIndex:set];
    }
}

TessellationBuffers EncodeTessellation(
    id<MTLCommandBuffer> command_buffer, const SetDrawStateCmd& draw,
    const MetalPipelineState& pipeline, NSMutableArray<id<MTLBuffer>>* staging_buffers
) {
    if (draw.DrawData().size() != 1 || pipeline.PatchPoints() != 3 ||
        pipeline.PointStride() == 0) {
        Unsupported("Metal tessellation mesh or patch layout");
    }
    const MeshDrawData& mesh = draw.DrawData()[0];
    if (!mesh.vtx_views.empty() || mesh.indirect_draw_param ||
        std::holds_alternative<IndexBuffer>(mesh.idx_view) || mesh.draw_params.size() != 1) {
        Unsupported("Metal tessellation indexed or indirect draw");
    }
    const SingleDrawParam& param = mesh.draw_params[0];
    if (param.index_cnt == 0 || param.index_cnt % 3 != 0 ||
        param.instance_cnt == 0 || param.first_index != 0 ||
        param.vertex_offset != 0 || param.first_instance != 0) {
        Unsupported("Metal tessellation draw range");
    }
    const uint64_t vertex_count = uint64_t(param.index_cnt) * param.instance_cnt;
    const uint64_t patch_count = vertex_count / pipeline.PatchPoints();
    if (patch_count > UINT32_MAX ||
        vertex_count > SIZE_MAX / pipeline.PointStride() ||
        patch_count > SIZE_MAX / sizeof(MTLTriangleTessellationFactorsHalf)) {
        Unsupported("Metal tessellation draw allocation size");
    }
    const auto allocate = [&](NSUInteger bytes) {
        id<MTLBuffer> buffer = [command_buffer.device
            newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        if (buffer == nil) throw std::runtime_error("Cannot allocate Metal tessellation buffer");
        [staging_buffers addObject:buffer];
        return buffer;
    };
    TessellationBuffers buffers{
        allocate(vertex_count * pipeline.PointStride()),
        allocate(vertex_count * pipeline.PointStride()),
        allocate(patch_count * sizeof(MTLTriangleTessellationFactorsHalf)),
        uint32_t(patch_count)
    };
    const uint32_t params[2] = {pipeline.PatchPoints(), buffers.patch_count};

    id<MTLComputeCommandEncoder> vertex = [command_buffer computeCommandEncoder];
    if (vertex == nil) throw std::runtime_error("Cannot encode Metal tessellation vertex stage");
    vertex.label = @"Tessellation vertex capture";
    [vertex setComputePipelineState:pipeline.TessVertexPipeline()];
    BindTessComputeArgs(command_buffer, vertex, pipeline.TessVertexFunction(),
        pipeline.TessVertexBindingsLayout(), draw, staging_buffers);
    [vertex setBuffer:buffers.control_points offset:0 atIndex:28];
    // MSL uint3 has 16-byte alignment even though it holds only three values.
    const uint32_t grid_size[4] = {param.index_cnt, param.instance_cnt, 1, 0};
    [vertex setBytes:grid_size length:sizeof(grid_size) atIndex:30];
    [vertex dispatchThreadgroups:MTLSizeMake(param.index_cnt / 3, param.instance_cnt, 1)
      threadsPerThreadgroup:MTLSizeMake(3, 1, 1)];
    [vertex endEncoding];

    id<MTLComputeCommandEncoder> hull = [command_buffer computeCommandEncoder];
    if (hull == nil) throw std::runtime_error("Cannot encode Metal tessellation hull stage");
    hull.label = @"Tessellation hull and factors";
    [hull setComputePipelineState:pipeline.HullPipeline()];
    BindTessComputeArgs(command_buffer, hull, pipeline.HullFunction(),
        pipeline.HullBindingsLayout(), draw, staging_buffers);
    [hull setBuffer:buffers.control_points offset:0 atIndex:22];
    [hull setBuffer:buffers.hull_points offset:0 atIndex:28];
    [hull setBuffer:buffers.factors offset:0 atIndex:26];
    [hull setBytes:params length:sizeof(params) atIndex:29];
    [hull dispatchThreads:MTLSizeMake(vertex_count, 1, 1)
    threadsPerThreadgroup:MTLSizeMake(3, 1, 1)];
    [hull endEncoding];
    return buffers;
}
} // namespace

MetalPipelineState* ValidateSimpleDraw(const SetDrawStateCmd& draw) {
    auto* pipeline = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(draw.Pipeline().handle));
    const RenderPassInfo& pass = draw.RenderPassInfo();
    if (pipeline == nullptr || pipeline->NativeRender() == nil ||
        (pipeline->ColorFormats().empty() && !pass.depth_attachment.Valid()) ||
        pipeline->ColorFormats().size() != pass.color_attachments.size() ||
        pass.view_mask != 0 || pass.viewport_cnt != 1 || !pass.render_area.IsValid() ||
        draw.DrawData().empty()) {
        throw std::runtime_error(
            "Metal RHI has not implemented graphics draw layout " + draw.name +
            " (pipeline_colors=" +
            std::to_string(pipeline == nullptr ? 0 : pipeline->ColorFormats().size()) +
            ", pass_colors=" + std::to_string(pass.color_attachments.size()) +
            ", args=" + std::to_string(draw.Args().args.size()) +
            ", constants=" + std::to_string(draw.Args().constants.size()) +
            ", view_mask=" + std::to_string(pass.view_mask) +
            ", viewports=" + std::to_string(pass.viewport_cnt) +
            ", meshes=" + std::to_string(draw.DrawData().size()) + ")"
        );
    }
    MetalBindlessArray* bindless = nullptr;
    for (uint arg_index = 0; arg_index < draw.Args().args.size(); ++arg_index) {
        const TArg& argument = draw.Args().args[arg_index];
        if (const auto* array = std::get_if<BindlessArrayRef>(&argument)) {
            if (bindless != nullptr) Unsupported("multiple graphics bindless arrays");
            bindless = dynamic_cast<MetalBindlessArray*>(array->Get());
            if (bindless == nullptr) Unsupported("a foreign graphics bindless array");
        } else if (const auto* view = std::get_if<BufferView>(&argument)) {
            auto* buffer = dynamic_cast<MetalBuffer*>(view->GetBuffer());
            if (buffer == nullptr || view->GetByteSize() == 0 ||
                view->GetByteOffset() > buffer->GetByteSize() ||
                view->GetByteSize() > buffer->GetByteSize() - view->GetByteOffset()) {
                Unsupported("this graphics buffer argument view");
            }
        } else if (const auto* view = std::get_if<TextureView>(&argument)) {
            auto* texture = dynamic_cast<MetalTexture*>(view->GetTexture());
            if (texture == nullptr || view->format != texture->GetFormat() ||
                view->num_mips == 0 ||
                view->mip_level + view->num_mips > texture->GetNumMips() ||
                view->num_array == 0 ||
                view->array_layer + view->num_array > texture->GetNumArray() ||
                view->offset != uint3{0, 0, 0} ||
                view->extent != texture->GetExtent()) {
                Unsupported("this graphics texture argument view");
            }
        } else if (const auto* sampler = std::get_if<Sampler>(&argument)) {
            if (sampler->filter >= SF_Num || sampler->address_mode >= SAM_Num ||
                sampler->compare_function >= SCF_Num) {
                Unsupported("this graphics sampler argument");
            }
        } else if (!std::holds_alternative<TInvalidArg>(argument)) {
            throw std::runtime_error(
                "Metal RHI has not implemented graphics shader argument " + draw.name +
                " (index=" + std::to_string(arg_index) +
                ", variant=" + std::to_string(argument.index()) + ")");
        }
    }
    if ((!pipeline->VertexBindingsLayout().bindless_sets.empty() ||
         !pipeline->FragmentBindingsLayout().bindless_sets.empty()) && bindless == nullptr) {
        Unsupported("missing graphics bindless array");
    }
    const auto validate_texel_bindings = [&](const MetalRenderBindings& layout) {
        for (uint index = 0; index < layout.scalar_bindings.size(); ++index) {
            const auto& binding = layout.scalar_bindings[index];
            if (!binding.active || !binding.texel_buffer) continue;
            const auto* view = std::get_if<BufferView>(&draw.Args().args[index]);
            auto* buffer = view == nullptr ? nullptr :
                dynamic_cast<MetalBuffer*>(view->GetBuffer());
            if (buffer == nullptr || buffer->NativeTexelTexture() == nil ||
                view->GetByteOffset() != 0 ||
                view->GetByteSize() != buffer->GetByteSize()) {
                Unsupported("this graphics texel buffer view");
            }
        }
    };
    validate_texel_bindings(pipeline->VertexBindingsLayout());
    validate_texel_bindings(pipeline->FragmentBindingsLayout());
    if (pipeline->IsTessellation()) {
        validate_texel_bindings(pipeline->TessVertexBindingsLayout());
        validate_texel_bindings(pipeline->HullBindingsLayout());
    }
    if (draw.Args().constants.size() * sizeof(uint) > 4096) {
        Unsupported("this graphics push constant size");
    }
    const ColorAttachment* first_color = pass.color_attachments.empty() ?
        nullptr : &pass.color_attachments[0];
    const DepthAttachment& depth = pass.depth_attachment;
    auto* target = dynamic_cast<MetalTexture*>(
        first_color != nullptr ? first_color->target : depth.target
    );
    const uint reference_mip = first_color != nullptr ? first_color->mip_level : depth.mip_level;
    const uint reference_layer = first_color != nullptr ? first_color->array_layer : depth.array_layer;
    if (target == nullptr || pipeline->ColorFormats().size() > 8 ||
        reference_mip >= target->GetNumMips() ||
        reference_layer >= target->GetNumArray() ||
        pass.render_area.offset.x < 0 || pass.render_area.offset.y < 0 ||
        uint(pass.render_area.offset.x) + pass.render_area.extent.width >
            std::max(1u, target->GetWidth() >> reference_mip) ||
        uint(pass.render_area.offset.y) + pass.render_area.extent.height >
            std::max(1u, target->GetHeight() >> reference_mip)) {
        Unsupported("this graphics attachment extent");
    }
    if (first_color != nullptr &&
        (pipeline->ColorFormats()[0] != ToMetalFormat(target->GetFormat()) ||
         first_color->array_count != 1 ||
         GetStoreOp(first_color->action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE)) {
        Unsupported("this graphics color attachment");
    }
    for (uint index = 1; index < pass.color_attachments.size(); ++index) {
        const ColorAttachment& color = pass.color_attachments[index];
        auto* color_target = dynamic_cast<MetalTexture*>(color.target);
        if (color_target == nullptr ||
            pipeline->ColorFormats()[index] != ToMetalFormat(color_target->GetFormat()) ||
            color_target->GetWidth() != target->GetWidth() ||
            color_target->GetHeight() != target->GetHeight() ||
            color.mip_level != reference_mip ||
            color.array_layer != reference_layer || color.array_count != 1 ||
            color.mip_level >= color_target->GetNumMips() ||
            color.array_layer >= color_target->GetNumArray() ||
            GetStoreOp(color.action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE) {
            Unsupported("this graphics MRT attachment");
        }
    }
    if (depth.Valid()) {
        auto* depth_target = dynamic_cast<MetalTexture*>(depth.target);
        const EAttachmentAction action = GetDepthAction(depth.action);
        if (depth_target == nullptr ||
            pipeline->DepthFormat() != ToMetalFormat(depth_target->GetFormat()) ||
            depth_target->GetWidth() != target->GetWidth() ||
            depth_target->GetHeight() != target->GetHeight() ||
            depth.mip_level != reference_mip ||
            depth.array_layer != reference_layer || depth.array_count != 1 ||
            depth.mip_level >= depth_target->GetNumMips() ||
            depth.array_layer >= depth_target->GetNumArray() ||
            GetStoreOp(action) == EAttachmentStoreOp::MULTISAMPLE_RESOLVE ||
            (depth_target->GetFormat() == PF_D32_SFLOAT_S8_UINT &&
             GetStoreOp(GetStencilAction(depth.action)) ==
                 EAttachmentStoreOp::MULTISAMPLE_RESOLVE)) {
            Unsupported("this graphics depth attachment");
        }
    } else if (pipeline->DepthFormat() != MTLPixelFormatInvalid) {
        Unsupported("this graphics depth attachment");
    }
    const RHIRasterizeInfo& raster = pipeline->Rasterizer();
    if (raster.cull_mode == RCM_FRONT_AND_BACK ||
        (raster.fill_mode != FM_FILL && raster.fill_mode != FM_LINE) ||
        raster.b_depth_clamp_enable || raster.b_depth_bias) {
        Unsupported("this graphics raster state");
    }
    for (const MeshDrawData& mesh : draw.DrawData()) {
        if (mesh.vtx_views.size() != pipeline->VertexBindingCount() ||
            (mesh.indirect_draw_param.has_value() == !mesh.draw_params.empty())) {
            Unsupported("this mesh draw layout");
        }
        if (mesh.indirect_draw_param) {
            const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
            auto* buffer = dynamic_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
            const bool indexed = std::holds_alternative<IndexBuffer>(mesh.idx_view);
            const uint argument_size = indexed ?
                sizeof(MTLDrawIndexedPrimitivesIndirectArguments) :
                sizeof(MTLDrawPrimitivesIndirectArguments);
            if (buffer == nullptr || indirect.stride < argument_size ||
                indirect.stride % alignof(uint32_t) != 0 ||
                indirect.buffer.GetByteOffset() % alignof(uint32_t) != 0 ||
                indirect.buffer.GetByteOffset() > buffer->GetByteSize() ||
                indirect.buffer.GetByteSize() >
                    buffer->GetByteSize() - indirect.buffer.GetByteOffset() ||
                indirect.count > indirect.buffer.GetByteSize() / indirect.stride) {
                Unsupported("this indirect draw buffer view");
            }
            if (indirect.count_buffer) {
                const BufferView& count_view = *indirect.count_buffer;
                auto* count_buffer = dynamic_cast<MetalBuffer*>(count_view.GetBuffer());
                if (count_buffer == nullptr || count_view.GetByteSize() != sizeof(uint32_t) ||
                    count_view.GetByteOffset() % alignof(uint32_t) != 0 ||
                    count_view.GetByteOffset() > count_buffer->GetByteSize() ||
                    count_view.GetByteSize() >
                        count_buffer->GetByteSize() - count_view.GetByteOffset()) {
                    Unsupported("this indirect draw count view");
                }
            }
        }
        for (const VertexBuffer& vertex : mesh.vtx_views) {
            auto* buffer = dynamic_cast<MetalBuffer*>(vertex.buffer);
            if (buffer == nullptr || vertex.offset >= buffer->GetByteSize()) {
                Unsupported("this vertex buffer view");
            }
        }
        if (const auto* indexed = std::get_if<IndexBuffer>(&mesh.idx_view)) {
            auto* buffer = dynamic_cast<MetalBuffer*>(indexed->buffer.GetBuffer());
            if (buffer == nullptr ||
                (indexed->stride != IET_UINT16 && indexed->stride != IET_UINT32)) {
                Unsupported("this index buffer view");
            }
            const uint64 stride = indexed->stride == IET_UINT16 ? 2 : 4;
            if (indexed->buffer.GetByteOffset() > buffer->GetByteSize() ||
                indexed->buffer.GetByteSize() >
                    buffer->GetByteSize() - indexed->buffer.GetByteOffset() ||
                indexed->buffer.GetByteOffset() % stride != 0 ||
                indexed->buffer.GetByteSize() % stride != 0) {
                Unsupported("this index buffer range");
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                if (uint64(param.first_index) + param.index_cnt >
                    indexed->buffer.GetByteSize() / stride) {
                    Unsupported("this indexed draw range");
                }
            }
        }
    }
    return pipeline;
}

void EncodeSimpleDraw(
    id<MTLCommandBuffer> command_buffer, const SetDrawStateCmd& draw,
    std::span<const uint> indirect_counts,
    NSMutableArray<id<MTLBuffer>>* staging_buffers,
    NSMutableArray<id<MTLTexture>>* texture_views,
    NSMutableArray<id<MTLSamplerState>>* sampler_states
) {
    auto* pipeline = static_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(draw.Pipeline().handle));
    const RenderPassInfo& pass = draw.RenderPassInfo();
    const TessellationBuffers tess = pipeline->IsTessellation() ?
        EncodeTessellation(command_buffer, draw, *pipeline, staging_buffers) : TessellationBuffers{};
    MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    for (uint index = 0; index < pass.color_attachments.size(); ++index) {
        const ColorAttachment& attachment = pass.color_attachments[index];
        auto* target = static_cast<MetalTexture*>(attachment.target);
        auto* color = descriptor.colorAttachments[index];
        color.texture = target->Native();
        color.level = attachment.mip_level;
        color.slice = attachment.array_layer;
        switch (GetLoadOp(attachment.action)) {
            case EAttachmentLoadOp::CLEAR: color.loadAction = MTLLoadActionClear; break;
            case EAttachmentLoadOp::LOAD: color.loadAction = MTLLoadActionLoad; break;
            default: color.loadAction = MTLLoadActionDontCare; break;
        }
        color.storeAction = GetStoreOp(attachment.action) == EAttachmentStoreOp::STORE ?
            MTLStoreActionStore : MTLStoreActionDontCare;
        const float4 clear = attachment.clear_color;
        color.clearColor = MTLClearColorMake(clear.x, clear.y, clear.z, clear.w);
    }
    if (pass.depth_attachment.Valid()) {
        const DepthAttachment& depth = pass.depth_attachment;
        auto* depth_target = static_cast<MetalTexture*>(depth.target);
        auto* native_depth = descriptor.depthAttachment;
        native_depth.texture = depth_target->Native();
        native_depth.level = depth.mip_level;
        native_depth.slice = depth.array_layer;
        const EAttachmentAction action = GetDepthAction(depth.action);
        switch (GetLoadOp(action)) {
            case EAttachmentLoadOp::CLEAR: native_depth.loadAction = MTLLoadActionClear; break;
            case EAttachmentLoadOp::LOAD: native_depth.loadAction = MTLLoadActionLoad; break;
            default: native_depth.loadAction = MTLLoadActionDontCare; break;
        }
        native_depth.storeAction = GetStoreOp(action) == EAttachmentStoreOp::STORE ?
            MTLStoreActionStore : MTLStoreActionDontCare;
        native_depth.clearDepth = depth.clear_depth;
        if (depth_target->GetFormat() == PF_D32_SFLOAT_S8_UINT) {
            auto* native_stencil = descriptor.stencilAttachment;
            native_stencil.texture = depth_target->Native();
            native_stencil.level = depth.mip_level;
            native_stencil.slice = depth.array_layer;
            const EAttachmentAction stencil_action = GetStencilAction(depth.action);
            switch (GetLoadOp(stencil_action)) {
                case EAttachmentLoadOp::CLEAR: native_stencil.loadAction = MTLLoadActionClear; break;
                case EAttachmentLoadOp::LOAD: native_stencil.loadAction = MTLLoadActionLoad; break;
                default: native_stencil.loadAction = MTLLoadActionDontCare; break;
            }
            native_stencil.storeAction =
                GetStoreOp(stencil_action) == EAttachmentStoreOp::STORE ?
                    MTLStoreActionStore : MTLStoreActionDontCare;
            native_stencil.clearStencil = depth.clear_stencil;
        }
    }
    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (encoder == nil) throw std::runtime_error("Cannot encode Metal draw render pass");
    encoder.label = [NSString stringWithUTF8String:draw.name.c_str()];
    [encoder setRenderPipelineState:pipeline->NativeRender()];
    if (pipeline->DepthState() != nil) [encoder setDepthStencilState:pipeline->DepthState()];
    MetalBindlessArray* bindless = nullptr;
    for (const TArg& argument : draw.Args().args) {
        if (const auto* array = std::get_if<BindlessArrayRef>(&argument)) {
            bindless = static_cast<MetalBindlessArray*>(array->Get());
        }
    }
    if (bindless != nullptr) {
        bindless->UseRenderResources(encoder);
        for (const auto& [set, table] : pipeline->VertexBindingsLayout().bindless_sets) {
            [encoder setVertexBuffer:bindless->NativeArguments(table) offset:0 atIndex:set];
        }
        for (const auto& [set, table] : pipeline->FragmentBindingsLayout().bindless_sets) {
            [encoder setFragmentBuffer:bindless->NativeArguments(table) offset:0 atIndex:set];
        }
    }
    const auto bind_scalar = [&](const MetalRenderBindings& layout,
                                 id<MTLFunction> function, bool vertex) {
        for (NSUInteger set = 0; set < layout.constant_buffer_index; ++set) {
            bool needed = false;
            for (const auto& binding : layout.scalar_bindings) {
                needed |= binding.active && binding.set == set;
            }
            if (!needed) continue;
            id<MTLArgumentEncoder> arguments =
                [function newArgumentEncoderWithBufferIndex:set];
            if (arguments == nil) Unsupported("this Metal graphics argument buffer layout");
            id<MTLBuffer> table = [command_buffer.device
                newBufferWithLength:arguments.encodedLength
                options:MTLResourceStorageModeShared];
            if (table == nil) throw std::runtime_error("Cannot allocate Metal graphics arguments");
            [staging_buffers addObject:table];
            [arguments setArgumentBuffer:table offset:0];
            for (uint index = 0; index < layout.scalar_bindings.size(); ++index) {
                const auto& binding = layout.scalar_bindings[index];
                if (!binding.active || binding.set != set) continue;
                if (binding.kind == SDA_Buffer || binding.kind == SDA_ConstantBuffer) {
                    const BufferView& view = std::get<BufferView>(draw.Args().args[index]);
                    auto* buffer = static_cast<MetalBuffer*>(view.GetBuffer());
                    if (binding.texel_buffer) {
                        [arguments setTexture:buffer->NativeTexelTexture()
                                      atIndex:binding.binding];
                        [encoder useResource:buffer->NativeTexelTexture()
                                      usage:MTLResourceUsageRead];
                    } else {
                        [arguments setBuffer:buffer->Native() offset:view.GetByteOffset()
                                     atIndex:binding.binding];
                        [encoder useResource:buffer->Native() usage:MTLResourceUsageRead];
                    }
                } else if (binding.kind == SDA_Texture) {
                    const TextureView& view = std::get<TextureView>(draw.Args().args[index]);
                    auto* texture = static_cast<MetalTexture*>(view.GetTexture());
                    id<MTLTexture> native_view = texture->Native();
                    if (view.mip_level != 0 || view.num_mips != texture->GetNumMips() ||
                        view.array_layer != 0 || view.num_array != texture->GetNumArray()) {
                        const MTLTextureType type = view.num_array > 1 ?
                            MTLTextureType2DArray : MTLTextureType2D;
                        native_view = [texture->Native()
                            newTextureViewWithPixelFormat:ToMetalFormat(view.format)
                                              textureType:type
                                                   levels:NSMakeRange(view.mip_level, view.num_mips)
                                                   slices:NSMakeRange(view.array_layer, view.num_array)];
                        if (native_view == nil) Unsupported("this Metal graphics texture view");
                        [texture_views addObject:native_view];
                    }
                    [arguments setTexture:native_view atIndex:binding.binding];
                    [encoder useResource:native_view usage:MTLResourceUsageRead];
                } else if (binding.kind == SDA_Sampler) {
                    const Sampler sampler = std::get<Sampler>(draw.Args().args[index]);
                    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
                    const bool nearest = sampler.filter == SF_NEAREST ||
                        sampler.filter == SF_ANISOTROPIC_NEAREST;
                    descriptor.minFilter = nearest ?
                        MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
                    descriptor.magFilter = nearest ?
                        MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
                    descriptor.mipFilter = nearest ?
                        MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
                    descriptor.maxAnisotropy = sampler.filter == SF_ANISOTROPIC_NEAREST ||
                        sampler.filter == SF_ANISOTROPIC_LINEAR ? 16 : 1;
                    MTLSamplerAddressMode address = MTLSamplerAddressModeRepeat;
                    switch (sampler.address_mode) {
                        case SAM_REPEAT: break;
                        case SAM_MIRRORED_REPEAT: address = MTLSamplerAddressModeMirrorRepeat; break;
                        case SAM_CLAMP_TO_EDGE: address = MTLSamplerAddressModeClampToEdge; break;
                        case SAM_CLAMP_TO_BORDER:
                            address = MTLSamplerAddressModeClampToBorderColor; break;
                        default: Unsupported("this Metal graphics sampler address");
                    }
                    descriptor.sAddressMode = address;
                    descriptor.tAddressMode = address;
                    descriptor.rAddressMode = address;
                    descriptor.supportArgumentBuffers = YES;
                    if (sampler.compare_function != SCF_NEVER) {
                        static constexpr MTLCompareFunction kCompare[] = {
                            MTLCompareFunctionNever, MTLCompareFunctionLess,
                            MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
                            MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
                            MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways
                        };
                        descriptor.compareFunction = kCompare[sampler.compare_function];
                    }
                    id<MTLSamplerState> native_sampler =
                        [command_buffer.device newSamplerStateWithDescriptor:descriptor];
                    if (native_sampler == nil) Unsupported("this Metal graphics sampler");
                    [sampler_states addObject:native_sampler];
                    [arguments setSamplerState:native_sampler atIndex:binding.binding];
                } else {
                    Unsupported("this Metal graphics scalar argument kind");
                }
            }
            if (vertex) [encoder setVertexBuffer:table offset:0 atIndex:set];
            else [encoder setFragmentBuffer:table offset:0 atIndex:set];
        }
    };
    bind_scalar(pipeline->VertexBindingsLayout(), pipeline->VertexFunction(), true);
    bind_scalar(pipeline->FragmentBindingsLayout(), pipeline->FragmentFunction(), false);
    if (pipeline->IsTessellation()) {
        [encoder setVertexBuffer:tess.hull_points offset:0 atIndex:22];
        [encoder setVertexBuffer:tess.factors offset:0 atIndex:26];
        [encoder setTessellationFactorBuffer:tess.factors offset:0 instanceStride:0];
    }
    if (!draw.Args().constants.empty()) {
        const void* data = draw.Args().constants.data();
        const NSUInteger length = draw.Args().constants.size() * sizeof(uint);
        [encoder setVertexBytes:data length:length
                       atIndex:pipeline->VertexBindingsLayout().constant_buffer_index];
        [encoder setFragmentBytes:data length:length
                         atIndex:pipeline->FragmentBindingsLayout().constant_buffer_index];
    }
    const Rect2D& rect = pass.render_area;
    [encoder setViewport:MTLViewport{double(rect.offset.x), double(rect.offset.y),
                                     double(rect.extent.width), double(rect.extent.height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{NSUInteger(rect.offset.x), NSUInteger(rect.offset.y),
                                          rect.extent.width, rect.extent.height}];
    const RHIRasterizeInfo& raster = pipeline->Rasterizer();
    [encoder setFrontFacingWinding:raster.b_front_counter_clockwise ?
        MTLWindingCounterClockwise : MTLWindingClockwise];
    [encoder setCullMode:raster.cull_mode == RCM_FRONT ? MTLCullModeFront :
                         raster.cull_mode == RCM_BACK ? MTLCullModeBack : MTLCullModeNone];
    [encoder setTriangleFillMode:raster.fill_mode == FM_LINE ?
        MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    for (uint mesh_index = 0; mesh_index < draw.DrawData().size(); ++mesh_index) {
        const MeshDrawData& mesh = draw.DrawData()[mesh_index];
        if (pipeline->IsTessellation()) {
            [encoder drawPatches:pipeline->PatchPoints() patchStart:0
                     patchCount:tess.patch_count patchIndexBuffer:nil
         patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            continue;
        }
        for (uint index = 0; index < mesh.vtx_views.size(); ++index) {
            const VertexBuffer& vertex = mesh.vtx_views[index];
            auto* buffer = static_cast<MetalBuffer*>(vertex.buffer);
            [encoder setVertexBuffer:buffer->Native() offset:vertex.offset atIndex:16 + index];
        }
        if (const auto* indexed = std::get_if<IndexBuffer>(&mesh.idx_view)) {
            auto* buffer = static_cast<MetalBuffer*>(indexed->buffer.GetBuffer());
            const uint stride = indexed->stride == IET_UINT16 ? 2 : 4;
            const MTLIndexType type = indexed->stride == IET_UINT16 ?
                MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            if (mesh.indirect_draw_param) {
                const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
                auto* arguments = static_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
                for (uint i = 0; i < indirect_counts[mesh_index]; ++i) {
                    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:type
                        indexBuffer:buffer->Native()
                        indexBufferOffset:indexed->buffer.GetByteOffset()
                        indirectBuffer:arguments->Native()
                        indirectBufferOffset:indirect.buffer.GetByteOffset() + i * indirect.stride];
                }
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:param.index_cnt indexType:type
                                   indexBuffer:buffer->Native()
                             indexBufferOffset:indexed->buffer.GetByteOffset() + param.first_index * stride
                                instanceCount:param.instance_cnt
                                   baseVertex:param.vertex_offset baseInstance:param.first_instance];
            }
        } else {
            if (mesh.indirect_draw_param) {
                const IndirectDrawParam& indirect = *mesh.indirect_draw_param;
                auto* arguments = static_cast<MetalBuffer*>(indirect.buffer.GetBuffer());
                for (uint i = 0; i < indirect_counts[mesh_index]; ++i) {
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        indirectBuffer:arguments->Native()
                        indirectBufferOffset:indirect.buffer.GetByteOffset() + i * indirect.stride];
                }
            }
            for (const SingleDrawParam& param : mesh.draw_params) {
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:param.vertex_offset vertexCount:param.index_cnt
                         instanceCount:param.instance_cnt baseInstance:param.first_instance];
            }
        }
    }
    [encoder endEncoding];
}

MetalPipelineState* ValidateComputeDispatch(
    const DispatchCmd& dispatch, const TCachedArgArray& cached_args
) {
    auto* pipeline = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(dispatch.Pipeline().handle));
    if (pipeline == nullptr || pipeline->NativeCompute() == nil ||
        !std::holds_alternative<uint3>(dispatch.Param())) {
        Unsupported("this compute dispatch pipeline or indirect layout");
    }
    const MTLSize local = pipeline->LocalSize();
    if (local.width == 0 || local.height == 0 || local.depth == 0 ||
        local.width * local.height * local.depth >
            pipeline->NativeCompute().maxTotalThreadsPerThreadgroup) {
        Unsupported("this compute shader workgroup size");
    }
    const ArrayArguments& args = dispatch.Args(cached_args);
    const auto& bindings = pipeline->ComputeBindings();
    if (args.args.size() != bindings.size()) Unsupported("this compute argument count");
    for (uint index = 0; index < bindings.size(); ++index) {
        const MetalComputeBinding& binding = bindings[index];
        if (binding.constant) {
            if ((dispatch.Pipeline().valid_bits & (uint64(1) << index)) &&
                args.constants.empty()) {
                Unsupported("empty compute push constants");
            }
            continue;
        }
        if (!binding.active) continue;
        if (binding.set >= pipeline->ConstantBufferIndex() || binding.set >= 16 ||
            (binding.kind != SDA_Buffer && binding.kind != SDA_ConstantBuffer &&
             binding.kind != SDA_Texture)) {
            throw std::runtime_error(
                "Metal RHI has not implemented compute shader argument " + dispatch.name +
                " (index=" + std::to_string(index) +
                ", kind=" + std::to_string(static_cast<uint>(binding.kind)) +
                ", set=" + std::to_string(binding.set) +
                ", binding=" + std::to_string(binding.binding) + ")");
        }
        if (binding.kind == SDA_Texture) {
            const auto* view = std::get_if<TextureView>(&args.args[index]);
            auto* texture = view == nullptr ? nullptr :
                dynamic_cast<MetalTexture*>(view->GetTexture());
            if (texture == nullptr || view->format != texture->GetFormat() ||
                view->num_mips == 0 ||
                view->mip_level + view->num_mips > texture->GetNumMips() ||
                view->num_array == 0 ||
                view->array_layer + view->num_array > texture->GetNumArray() ||
                view->offset != uint3{0, 0, 0} ||
                view->extent != texture->GetExtent()) {
                Unsupported("this compute texture argument view");
            }
        } else {
            const auto* view = std::get_if<BufferView>(&args.args[index]);
            auto* buffer = view == nullptr ? nullptr :
                dynamic_cast<MetalBuffer*>(view->GetBuffer());
            if (buffer == nullptr || view->GetByteSize() == 0 ||
                view->GetByteOffset() > buffer->GetByteSize() ||
                view->GetByteSize() > buffer->GetByteSize() - view->GetByteOffset()) {
                Unsupported("this compute buffer argument view");
            }
            if (binding.texel_buffer &&
                (buffer->NativeTexelTexture() == nil || view->GetByteOffset() != 0 ||
                 view->GetByteSize() != buffer->GetByteSize())) {
                Unsupported("this compute texel buffer view");
            }
        }
    }
    if (!pipeline->ComputeBindlessSets().empty()) {
        bool found_bindless = false;
        for (const auto& arg : args.args) {
            const auto* array = std::get_if<BindlessArrayRef>(&arg);
            if (array == nullptr) continue;
            found_bindless = dynamic_cast<MetalBindlessArray*>(array->Get()) != nullptr;
            if (found_bindless) break;
        }
        if (!found_bindless) Unsupported("missing compute bindless array");
    }
    return pipeline;
}

void EncodeComputeDispatch(
    id<MTLCommandBuffer> command_buffer, NSMutableArray<id<MTLBuffer>>* staging_buffers,
    NSMutableArray<id<MTLTexture>>* texture_views,
    const DispatchCmd& dispatch, const TCachedArgArray& cached_args
) {
    auto* pipeline = static_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(dispatch.Pipeline().handle));
    const uint3 groups = std::get<uint3>(dispatch.Param());
    if (groups.x == 0 || groups.y == 0 || groups.z == 0) return;
    const ArrayArguments& args = dispatch.Args(cached_args);
    const auto& bindings = pipeline->ComputeBindings();
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) throw std::runtime_error("Cannot encode Metal compute dispatch");
    encoder.label = [NSString stringWithUTF8String:dispatch.name.c_str()];
    [encoder setComputePipelineState:pipeline->NativeCompute()];
    if (!pipeline->ComputeBindlessSets().empty()) {
        MetalBindlessArray* bindless = nullptr;
        for (const auto& arg : args.args) {
            if (const auto* array = std::get_if<BindlessArrayRef>(&arg)) {
                bindless = static_cast<MetalBindlessArray*>(array->Get());
                break;
            }
        }
        bindless->UseComputeResources(encoder);
        for (const auto& [set, table] : pipeline->ComputeBindlessSets()) {
            [encoder setBuffer:bindless->NativeArguments(table) offset:0 atIndex:set];
        }
    }
    for (NSUInteger set = 0; set < pipeline->ConstantBufferIndex(); ++set) {
        bool needed = false;
        for (const auto& binding : bindings) {
            needed |= binding.active && binding.set == set;
        }
        if (!needed) continue;
        id<MTLArgumentEncoder> arguments = [pipeline->ComputeFunction()
            newArgumentEncoderWithBufferIndex:set];
        if (arguments == nil) Unsupported("this Metal compute argument buffer layout");
        id<MTLBuffer> table = [command_buffer.device
            newBufferWithLength:arguments.encodedLength
            options:MTLResourceStorageModeShared];
        if (table == nil) throw std::runtime_error("Cannot allocate Metal compute arguments");
        [staging_buffers addObject:table];
        [arguments setArgumentBuffer:table offset:0];
        for (uint index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            if (!binding.active || binding.set != set) continue;
            if (binding.kind == SDA_Texture) {
                const TextureView& view = std::get<TextureView>(args.args[index]);
                auto* texture = static_cast<MetalTexture*>(view.GetTexture());
                id<MTLTexture> native_view = texture->Native();
                if (view.mip_level != 0 || view.num_mips != texture->GetNumMips() ||
                    view.array_layer != 0 || view.num_array != texture->GetNumArray()) {
                    MTLTextureType type = view.num_array > 1 ?
                        MTLTextureType2DArray : MTLTextureType2D;
                    native_view = [texture->Native()
                        newTextureViewWithPixelFormat:ToMetalFormat(view.format)
                                          textureType:type
                                               levels:NSMakeRange(view.mip_level, view.num_mips)
                                               slices:NSMakeRange(view.array_layer, view.num_array)];
                    if (native_view == nil) Unsupported("this Metal compute texture view");
                    [texture_views addObject:native_view];
                }
                [arguments setTexture:native_view atIndex:binding.binding];
                [encoder useResource:native_view
                            usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            } else {
                const BufferView& view = std::get<BufferView>(args.args[index]);
                auto* buffer = static_cast<MetalBuffer*>(view.GetBuffer());
                if (binding.texel_buffer) {
                    [arguments setTexture:buffer->NativeTexelTexture()
                                  atIndex:binding.binding];
                    [encoder useResource:buffer->NativeTexelTexture()
                                usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                } else {
                    [arguments setBuffer:buffer->Native() offset:view.GetByteOffset()
                                 atIndex:binding.binding];
                    [encoder useResource:buffer->Native()
                                usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                }
            }
        }
        [encoder setBuffer:table offset:0 atIndex:set];
    }
    if (!args.constants.empty()) {
        [encoder setBytes:args.constants.data()
                  length:args.constants.size() * sizeof(uint)
                 atIndex:pipeline->ConstantBufferIndex()];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(groups.x, groups.y, groups.z)
            threadsPerThreadgroup:pipeline->LocalSize()];
    [encoder endEncoding];
}

} // namespace Moer::Render
