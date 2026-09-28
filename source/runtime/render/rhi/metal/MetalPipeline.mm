#include "rhi/metal/MetalPipeline.h"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace Moer::Render {
namespace {
id<MTLFunction> CompileMetalFunction(id<MTLDevice> device, const SingleShaderInfo& shader) {
    if (shader.shader_data.empty() || shader.entry_point.empty()) {
        throw std::runtime_error("Metal pipeline shader source or entry point is empty");
    }
    NSString* source = [[NSString alloc]
        initWithBytes:shader.shader_data.data()
               length:shader.shader_data.size()
             encoding:NSUTF8StringEncoding];
    if (source == nil) throw std::runtime_error("Metal pipeline shader source is not UTF-8 MSL");
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (library == nil) {
        throw std::runtime_error("Metal shader compilation failed: " +
            std::string(error.localizedDescription.UTF8String ?: "unknown error"));
    }
    NSString* entry = [[NSString alloc] initWithBytes:shader.entry_point.data()
                                              length:shader.entry_point.size()
                                            encoding:NSUTF8StringEncoding];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    if (function == nil) {
        throw std::runtime_error("Metal shader entry point was not found: " +
            std::string(shader.entry_point));
    }
    return function;
}

PipelineHandle MetalPipelineMetadata(
    const PipelineShaderInfo& shader_info,
    std::initializer_list<const SingleShaderInfo*> stages
) {
    if (shader_info.layout_hash.size() != shader_info.arg_cpp_info.size() ||
        shader_info.layout_hash.size() > 64) {
        Unsupported("this pipeline argument layout");
    }
    PipelineHandle handle{};
    handle.binding_infos.resize(shader_info.layout_hash.size());
    for (uint index = 0; index < shader_info.layout_hash.size(); ++index) {
        handle.hash_2_info_index[GetHash(shader_info.layout_hash[index])] = index;
        for (const SingleShaderInfo* shader : stages) {
            if (shader->shader_param_map == nullptr) continue;
            const auto& reflection = shader->shader_param_map->reflect_map;
            const bool bindless = shader_info.arg_cpp_info[index].type == SDA_BindlessArray;
            const auto found = reflection.find(std::string(
                bindless ? ReflectParamInfo::bdls_name : shader_info.layout_hash[index]
            ));
            if (found == reflection.end()) continue;
            bool active = false;
            if (bindless) {
                const auto& resources = found->second.spirv.bindless;
                active = (resources.array && resources.array->custom_flag.active) ||
                         (resources.buffer && resources.buffer->custom_flag.active) ||
                         (resources.image && resources.image->custom_flag.active) ||
                         (resources.sampler && resources.sampler->custom_flag.active);
            } else if (const auto* resource = std::get_if<ReflectParamInfo::Resource>(
                           &found->second.spirv.resources.data)) {
                active = resource->custom_flag.active;
            } else if (const auto* constant = std::get_if<ReflectParamInfo::Constant>(
                           &found->second.spirv.resources.data)) {
                active = constant->custom_flag.active;
            }
            if (active) handle.valid_bits |= uint64(1) << index;
        }
        if (shader_info.arg_cpp_info[index].type == SDA_Constant &&
            (handle.valid_bits & (uint64(1) << index))) {
            handle.constant_idx = index;
        }
    }
    return handle;
}

MetalRenderBindings MetalRenderStageBindings(
    const PipelineShaderInfo& shader_info, const SingleShaderInfo& shader
) {
    MetalRenderBindings layout;
    if (shader.shader_param_map == nullptr) return layout;
    const auto& reflection = shader.shader_param_map->reflect_map;
    const auto found = reflection.find(std::string(ReflectParamInfo::bdls_name));
    if (found != reflection.end()) {
        const auto& bindless = found->second.spirv.bindless;
        const auto add = [&](const std::optional<ReflectParamInfo::Bindless>& resource, uint table) {
            if (!resource || !resource->custom_flag.active) return;
            for (const auto& [set, existing_table] : layout.bindless_sets) {
                if (set == resource->set) {
                    if (existing_table != table) Unsupported("overlapping Metal bindless sets");
                    return;
                }
            }
            layout.bindless_sets.emplace_back(resource->set, table);
            layout.constant_buffer_index = std::max(
                layout.constant_buffer_index, NSUInteger(resource->set) + 1);
        };
        add(bindless.array, 1);
        add(bindless.buffer, 1);
        add(bindless.image, 2);
        add(bindless.sampler, 3);
        if (bindless.acceleration_structure &&
            bindless.acceleration_structure->custom_flag.active) {
            Unsupported("Metal bindless acceleration structures");
        }
    }
    layout.scalar_bindings.resize(shader_info.layout_hash.size());
    for (uint index = 0; index < shader_info.layout_hash.size(); ++index) {
        const EShaderArgType kind = shader_info.arg_cpp_info[index].type;
        if (kind == SDA_Constant) {
            layout.scalar_bindings[index].constant = true;
            continue;
        }
        if (kind == SDA_BindlessArray) continue;
        const auto resource_it = reflection.find(std::string(shader_info.layout_hash[index]));
        if (resource_it == reflection.end()) continue;
        const auto* resource = std::get_if<ReflectParamInfo::Resource>(
            &resource_it->second.spirv.resources.data);
        if (resource == nullptr || !resource->custom_flag.active) continue;
        layout.scalar_bindings[index] = MetalComputeBinding{
            resource->set, resource->binding, kind, true, false,
            resource->desc_type == VDT_UNIFORM_TEXEL_BUFFER ||
                resource->desc_type == VDT_STORAGE_TEXEL_BUFFER};
        layout.constant_buffer_index = std::max(
            layout.constant_buffer_index, NSUInteger(resource->set) + 1);
    }
    return layout;
}

} // namespace

PipelineHandle CreateMetalGraphicsPipeline(id<MTLDevice> device, GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info) {
    if (!std::holds_alternative<ShaderVsPs>(shader_info.shader_group) ||
        create_info.primitive_topology != EPrimitiveTopology::TRIANGLE_LIST ||
        create_info.view_mask != 0 || create_info.multi_view_count != 1 ||
        create_info.multisample_info.sample_count != 1 ||
        create_info.color_attachment_count > 8 ||
        shader_info.layout_hash.size() != shader_info.arg_cpp_info.size() ||
        shader_info.layout_hash.size() > 64 ||
        create_info.depth_stencil_info.b_enable_front_face_stencil ||
        create_info.depth_stencil_info.b_enable_back_face_stencil) {
        Unsupported("this graphics pipeline layout");
    }
    const auto& shaders = std::get<ShaderVsPs>(shader_info.shader_group);
    @autoreleasepool {
        id<MTLFunction> vertex = CompileMetalFunction(device, shaders.vs);
        id<MTLFunction> fragment = CompileMetalFunction(device, shaders.ps);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.rasterSampleCount = 1;
        descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;

        MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
        uint attribute_index = 0;
        for (uint binding_index = 0; binding_index < create_info.vertex_stream.bindings.size(); ++binding_index) {
            const VertexBinding& binding = create_info.vertex_stream.bindings[binding_index];
            const uint metal_buffer_index = 16 + binding_index; // Reserve low slots for MSL resource sets.
            if (metal_buffer_index >= 31) Unsupported("too many vertex streams");
            NSUInteger stride = 0;
            for (const VertexElement& element : binding.vertex_elements) {
                if (attribute_index >= 31) Unsupported("too many vertex attributes");
                auto* attribute = vertex_descriptor.attributes[attribute_index++];
                attribute.format = ToMetalVertexFormat(element.format);
                attribute.offset = stride;
                attribute.bufferIndex = metal_buffer_index;
                stride += GetByteFromPixelFormat(element.format);
            }
            auto* layout = vertex_descriptor.layouts[metal_buffer_index];
            layout.stride = stride;
            layout.stepFunction = binding.input_rate == VIR_INSTANCE ?
                MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            layout.stepRate = 1;
        }
        descriptor.vertexDescriptor = vertex_descriptor;

        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            auto* attachment = descriptor.colorAttachments[index];
            const RHIColorAttachmentInfo& info = create_info.color_attachments_info[index];
            attachment.pixelFormat = ToMetalFormat(info.pixel_format);
            const RHIBlendAttachmentInfo& blend = info.blend_state_info;
            attachment.blendingEnabled =
                blend.color_blend_op != BO_ADD || blend.color_src_blend_factor != BF_ONE ||
                blend.color_dst_blend_factor != BF_ZERO || blend.alpha_blend_op != BO_ADD ||
                blend.alpha_src_blend_factor != BF_ONE || blend.alpha_dst_blend_factor != BF_ZERO;
            attachment.rgbBlendOperation = ToMetalBlendOperation(blend.color_blend_op);
            attachment.alphaBlendOperation = ToMetalBlendOperation(blend.alpha_blend_op);
            attachment.sourceRGBBlendFactor = ToMetalBlendFactor(blend.color_src_blend_factor);
            attachment.destinationRGBBlendFactor = ToMetalBlendFactor(blend.color_dst_blend_factor);
            attachment.sourceAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_src_blend_factor);
            attachment.destinationAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_dst_blend_factor);
            MTLColorWriteMask write_mask = MTLColorWriteMaskNone;
            if (blend.color_write_mask & CW_RED) write_mask |= MTLColorWriteMaskRed;
            if (blend.color_write_mask & CW_GREEN) write_mask |= MTLColorWriteMaskGreen;
            if (blend.color_write_mask & CW_BLUE) write_mask |= MTLColorWriteMaskBlue;
            if (blend.color_write_mask & CW_ALPHA) write_mask |= MTLColorWriteMaskAlpha;
            attachment.writeMask = write_mask;
        }
        if (create_info.depth_stencil_format != PF_UNDEFINED) {
            descriptor.depthAttachmentPixelFormat = ToMetalFormat(create_info.depth_stencil_format);
            if (create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT) {
                descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> native_pipeline =
            [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal render pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        id<MTLDepthStencilState> depth_state = nil;
        if (create_info.depth_stencil_format != PF_UNDEFINED) {
            MTLDepthStencilDescriptor* depth_descriptor = [MTLDepthStencilDescriptor new];
            depth_descriptor.depthCompareFunction =
                ToMetalCompare(create_info.depth_stencil_info.depth_test_op);
            depth_descriptor.depthWriteEnabled = create_info.depth_stencil_info.b_enable_depth_write;
            depth_state = [device newDepthStencilStateWithDescriptor:depth_descriptor];
            if (depth_state == nil) throw std::runtime_error("Cannot create Metal depth state");
        }
        PipelineHandle handle = MetalPipelineMetadata(shader_info, {&shaders.vs, &shaders.ps});
        std::vector<MTLPixelFormat> color_formats;
        color_formats.reserve(create_info.color_attachment_count);
        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            color_formats.push_back(ToMetalFormat(create_info.color_attachments_info[index].pixel_format));
        }
        handle.handle = reinterpret_cast<uint64>(MoerNew(MetalPipelineState)(
            native_pipeline, std::move(color_formats),
            uint(create_info.vertex_stream.bindings.size()), create_info.rasterizer_info,
            descriptor.depthAttachmentPixelFormat, depth_state, vertex, fragment,
            MetalRenderStageBindings(shader_info, shaders.vs),
            MetalRenderStageBindings(shader_info, shaders.ps)
        ));
        return handle;
    }
}
PipelineHandle CreateMetalComputePipeline(id<MTLDevice> device, PipelineShaderInfo&& shader_info) {
    if (!std::holds_alternative<ShaderCs>(shader_info.shader_group)) {
        Unsupported("this compute pipeline shader group");
    }
    const SingleShaderInfo& shader = std::get<ShaderCs>(shader_info.shader_group).cs;
    @autoreleasepool {
        id<MTLFunction> function = CompileMetalFunction(device, shader);
        NSError* error = nil;
        id<MTLComputePipelineState> native_pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal compute pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        PipelineHandle handle = MetalPipelineMetadata(shader_info, {&shader});
        auto bindings = MetalRenderStageBindings(shader_info, shader);
        const uint3 group = shader.compute_local_size;
        handle.handle = reinterpret_cast<uint64>(MoerNew(MetalPipelineState)(
            native_pipeline, function, std::move(bindings),
            MTLSizeMake(group.x, group.y, group.z)
        ));
        return handle;
    }
}

void* GetMetalNativeComputePipeline(PipelineHandle pipeline) noexcept {
    auto* metal = dynamic_cast<MetalPipelineState*>(
        reinterpret_cast<PipelineState*>(pipeline.handle));
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeCompute();
}

} // namespace Moer::Render
