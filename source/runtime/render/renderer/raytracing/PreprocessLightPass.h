#ifndef MOER_PREPARE_LIGHTS_PASS_H
#define MOER_PREPARE_LIGHTS_PASS_H

#include "RaytracingGraphResources.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

namespace Moer {
class Scene;
}

namespace Moer::Render::Raytracing {

class PrepareLightShaderPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PrepareLightShaderPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(PrepareLightsParams, param);
    DEFINE_SHADER_BUFFER(light_data);
    DEFINE_SHADER_BUFFER(light_index_mapping);
    DEFINE_SHADER_TEX(local_light_pdf);
    DEFINE_SHADER_BUFFER(prim_lights);
    DEFINE_SHADER_BUFFER(tasks);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(param, light_data, light_index_mapping, local_light_pdf, prim_lights, tasks, bdls);
};
class PrepareLightPass {
public:
    struct PreparedCommand {
        Array<uint>                  primitive_to_light;
        Array<PrepareLightsTask>     tasks;
        Array<PolymorphicLightInfo>  prim_light_infos;
        PrepareLightsParams          params{};
        uint                         dispatch_light_count = 0;
    };

    struct RecordResources {
        BufferRef          primitive_to_light_buf;
        BufferRef          task_buf;
        BufferRef          prim_light_buf;
        BufferRef          light_mapping_buf;
        BufferRef          light_data_buf;
        TextureRef         local_light_pdf_tex;
        Array<TextureView> local_light_pdf_mips;
        BindlessArrayRef   bindless_array;
        ShaderUtils*       shader_utils = nullptr;
    };

    PrepareLightPass(class RenderDevice& _device, class ShaderManager& _manager, Scene& _scene);
    PreparedCommand Prepare(RTContext& _rt_ctx);
    void AddPasses(
        RenderGraph& _graph,
        const RTGraphFrameResources& _rg,
        RTContext& _rt_ctx,
        PreparedCommand&& _command
    );

private:
    struct EmissivePrimitiveEntry {
        uint primitive_id       = UINT_MAX;
        uint light_offset       = 0;
        uint num_triangles      = 0;
        uint index_start_idx    = 0;
        uint first_instance_idx = UINT_MAX;
    };

    static RecordResources CaptureResources(RTContext& _rt_ctx);
    void RebuildEmissivePrimitiveCache();
    void RecordUploads(CommandList& _cmd_list, const PreparedCommand& _command, const RecordResources& _resources);
    void RecordClears(CommandList& _cmd_list, const RecordResources& _resources);
    void RecordPrepareLights(CommandList& _cmd_list, const PreparedCommand& _command, const RecordResources& _resources);
    void RecordGenerateMips(CommandList& _cmd_list, RecordResources& _resources);

    class RenderDevice&  device;
    class ShaderManager& manager;
    Scene&               scene;

    UnorderedMap<uint64, uint> instance_light_buffer_offsets;
    UnorderedMap<uint64, uint> primitive_light_buffer_offsets;
    Array<EmissivePrimitiveEntry> cached_emissive_primitives;
    Array<uint>                   cached_primitive_to_light;
    uint                          cached_primitive_count              = UINT_MAX;
    uint                          cached_emissive_triangle_count      = 0;
    bool                          cached_primitive_to_light_dirty     = true;
    Buffer*                       cached_primitive_to_light_upload_target = nullptr;

    BufferRef geom_instance_to_light_buf;

    PrepareLightShaderPipeline prepare_light_pipeline;

    bool b_odd_frame = false;
};

} // namespace Moer::Render::Raytracing

#endif