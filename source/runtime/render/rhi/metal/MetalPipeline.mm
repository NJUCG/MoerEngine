#include "rhi/metal/MetalPipeline.h"
#include "rhi/ShaderStageUtils.h"
#include "rhi/ShaderConstantLayout.h"

#include <algorithm>
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

PipelineHandle MetalPipelineMetadata(const PipelineShaderInfo& shader_info) {
    if (shader_info.arguments.size() > 64) {
        Unsupported("this pipeline argument layout");
    }
    PipelineHandle handle{};
    handle.binding_infos.resize(shader_info.arguments.size());
    for (uint index = 0; index < shader_info.arguments.size(); ++index) {
        handle.hash_2_info_index[GetHash(shader_info.arguments[index].name)] = index;
        for (const auto& shader : shader_info.shaders) {
            if (shader.shader_param_map == nullptr) continue;
            const auto& reflection = shader.shader_param_map->reflect_map;
            const bool  bindless   = shader_info.arguments[index].cpp_info.type == SDA_BindlessArray;
            const auto  found      = reflection.find(
                std::string(bindless ? ReflectParamInfo::bdls_name : shader_info.arguments[index].name)
            );
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
    }
    return handle;
}

MetalRenderBindings MetalRenderStageBindings(
    const PipelineShaderInfo& shader_info, const SingleShaderInfo& shader
) {
    MetalRenderBindings layout;
    if (shader.shader_param_map == nullptr) return layout;
    const auto& reflection = shader.shader_param_map->reflect_map;
    for (const auto& argument : shader_info.constant_layout.arguments) {
        const auto parameter =
            reflection.find(std::string(shader_info.arguments[argument.argument_index].name));
        if (parameter == reflection.end()) continue;
        const auto* constant = std::get_if<ReflectParamInfo::Constant>(&parameter->second.spirv.resources.data);
        if (constant != nullptr && constant->custom_flag.active) {
            layout.constant_byte_size = shader_info.constant_layout.byte_size;
        }
    }
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
    layout.scalar_bindings.resize(shader_info.arguments.size());
    for (uint index = 0; index < shader_info.arguments.size(); ++index) {
        const EShaderArgType kind = shader_info.arguments[index].cpp_info.type;
        if (kind == SDA_Constant) {
            layout.scalar_bindings[index].constant = true;
            continue;
        }
        if (kind == SDA_BindlessArray) continue;
        const auto resource_it = reflection.find(std::string(shader_info.arguments[index].name));
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
    ValidatePipelineConstants(shader_info, 4096);
    if (const auto error = ValidateGraphicsShaderStages(shader_info.shaders); !error.empty()) {
        throw std::runtime_error("Cannot create Metal graphics pipeline: " + std::string(error));
    }
    const auto* vs_info = FindShaderStage(shader_info.shaders, ST_VERTEX);
    const auto* hs_info = FindShaderStage(shader_info.shaders, ST_HULL);
    const auto* ds_info = FindShaderStage(shader_info.shaders, ST_DOMAIN);
    const bool tessellation = hs_info != nullptr;
    if (FindShaderStage(shader_info.shaders, ST_GEOMETRY) != nullptr ||
        FindShaderStage(shader_info.shaders, ST_MESH) != nullptr ||
        create_info.primitive_topology !=
            (tessellation ? EPrimitiveTopology::PATCH_LIST : EPrimitiveTopology::TRIANGLE_LIST) ||
        (tessellation &&
         (create_info.patch_control_points != 3 || create_info.patch_control_point_stride == 0 ||
          !create_info.vertex_stream.bindings.empty())) ||
        create_info.view_mask != 0 || create_info.multi_view_count != 1 ||
        create_info.multisample_info.sample_count != 1 || create_info.color_attachment_count > 8 ||
        shader_info.arguments.size() > 64 || create_info.depth_stencil_info.b_enable_front_face_stencil ||
        create_info.depth_stencil_info.b_enable_back_face_stencil) {
        Unsupported("this graphics pipeline layout");
    }
    const SingleShaderInfo& vs = *vs_info;
    const SingleShaderInfo& ps = *FindShaderStage(shader_info.shaders, ST_FRAGMENT);
    @autoreleasepool {
        id<MTLFunction> vertex = CompileMetalFunction(device, vs);
        id<MTLFunction> fragment = CompileMetalFunction(device, ps);
        id<MTLFunction> hull = nil;
        id<MTLFunction> domain = nil;
        id<MTLComputePipelineState> tess_vertex_pipeline = nil;
        id<MTLComputePipelineState> hull_pipeline = nil;
        NSError* error = nil;
        if (tessellation) {
            hull = CompileMetalFunction(device, *hs_info);
            domain = CompileMetalFunction(device, *ds_info);
            tess_vertex_pipeline = [device newComputePipelineStateWithFunction:vertex error:&error];
            if (tess_vertex_pipeline == nil) {
                throw std::runtime_error("Metal tessellation vertex kernel creation failed: " +
                    std::string(error.localizedDescription.UTF8String ?: "unknown error"));
            }
            error = nil;
            hull_pipeline = [device newComputePipelineStateWithFunction:hull error:&error];
            if (hull_pipeline == nil) {
                throw std::runtime_error("Metal hull kernel creation failed: " +
                    std::string(error.localizedDescription.UTF8String ?: "unknown error"));
            }
        }
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = tessellation ? domain : vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.rasterSampleCount = 1;
        descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        if (tessellation) {
            descriptor.maxTessellationFactor = 64;
            descriptor.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
            descriptor.tessellationPartitionMode = MTLTessellationPartitionModeFractionalEven;
            descriptor.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
            descriptor.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
            descriptor.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        }

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
                stride += GetVertexFormatByteSize(element.format);
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
        error = nil;
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
        PipelineHandle handle = MetalPipelineMetadata(shader_info);
        std::vector<MTLPixelFormat> color_formats;
        color_formats.reserve(create_info.color_attachment_count);
        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            color_formats.push_back(ToMetalFormat(create_info.color_attachments_info[index].pixel_format));
        }
        auto* pipeline = MoerNew(MetalPipelineState)(
            native_pipeline, std::move(color_formats),
            uint(create_info.vertex_stream.bindings.size()), create_info.rasterizer_info,
            descriptor.depthAttachmentPixelFormat, depth_state,
            tessellation ? domain : vertex, fragment,
            MetalRenderStageBindings(shader_info, tessellation ? *ds_info : vs),
            MetalRenderStageBindings(shader_info, ps)
        );
        if (tessellation) {
            pipeline->SetTessellation(
                tess_vertex_pipeline, vertex, hull_pipeline, hull,
                MetalRenderStageBindings(shader_info, vs),
                MetalRenderStageBindings(shader_info, *hs_info),
                create_info.patch_control_points, create_info.patch_control_point_stride);
        }
        handle.handle = reinterpret_cast<uint64>(pipeline);
        return handle;
    }
}
PipelineHandle CreateMetalComputePipeline(id<MTLDevice> device, PipelineShaderInfo&& shader_info) {
    ValidatePipelineConstants(shader_info, 4096);
    if (const auto error = ValidateComputeShaderStages(shader_info.shaders); !error.empty()) {
        throw std::runtime_error("Cannot create Metal compute pipeline: " + std::string(error));
    }
    const SingleShaderInfo& shader = shader_info.shaders.front();
    @autoreleasepool {
        id<MTLFunction> function = CompileMetalFunction(device, shader);
        NSError* error = nil;
        id<MTLComputePipelineState> native_pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal compute pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        PipelineHandle handle = MetalPipelineMetadata(shader_info);
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
