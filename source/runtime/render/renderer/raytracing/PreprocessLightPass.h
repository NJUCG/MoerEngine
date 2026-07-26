#ifndef MOER_PREPARE_LIGHTS_PASS_H
#define MOER_PREPARE_LIGHTS_PASS_H

#include "RTResource.h"
#include "RaytracingSceneFrameSnapshot.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

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
    struct EmissiveLightHistory {
        uint light_offset       = 0;
        uint num_triangles      = 0;
        uint index_start_idx    = 0;
        uint first_instance_idx = 0;
    };

    struct RecordResources {
        BufferRef primitive_to_light_buf;
        BufferRef task_buf;
        BufferRef prim_light_buf;
        BufferRef light_mapping_buf;
        BufferRef light_data_buf;

        BufferRef primitive_buf;
        BufferRef instance_buf;
        BufferRef material_buf;
        BufferRef position_buf;
        BufferRef index_buf;
        BufferRef texcoord0_buf;

        TextureRef         local_light_pdf_tex;
        Array<TextureView> local_light_pdf_mips;
        Array<TextureRef>  material_textures;
        BindlessArrayRef   bindless_array;
        ShaderUtils*       shader_utils = nullptr;
    };

    struct MipDispatch {
        uint source_mip      = 0;
        uint bound_mip_count = 0;
    };

    struct RecordPayload {
        Array<uint>                 primitive_to_light;
        Array<PrepareLightsTask>    tasks;
        Array<PolymorphicLightInfo> primitive_light_infos;
        PrepareLightsParams         params{};
        DI::LightBufferParams       light_buffer_params{};
        uint                        dispatch_light_count           = 0;
        bool                        reads_scene_geometry           = false;
        bool                        reset_light_history            = false;
        bool                        primitive_to_light_initialized = false;
        bool                        tasks_initialized              = false;
        bool                        primitive_lights_initialized   = false;
        bool                        light_mapping_initialized      = false;
        bool                        light_data_initialized         = false;
        bool                        local_light_pdf_initialized    = false;
        Array<MipDispatch>          mip_dispatches;
        RecordResources             resources{};
    };

    struct PreparedCommand {
        PreparedCommand()                                  = default;
        PreparedCommand(const PreparedCommand&)            = delete;
        PreparedCommand& operator=(const PreparedCommand&) = delete;
        PreparedCommand(PreparedCommand&&)                 = default;
        PreparedCommand& operator=(PreparedCommand&&)      = default;

        const DI::LightBufferParams& GetLightBufferParams() const {
            return record->light_buffer_params;
        }

        bool ReadsSceneGeometry() const {
            return record && record->reads_scene_geometry;
        }

        SharedPtr<const RecordPayload> record{};

        UnorderedMap<uint64, EmissiveLightHistory> next_instance_light_buffer_offsets;
        UnorderedMap<uint64, uint>                 next_primitive_light_buffer_offsets;
        bool                                       next_odd_frame                   = false;
        const Buffer*                              next_primitive_to_light_identity = nullptr;
        const Buffer*                              next_tasks_identity              = nullptr;
        const Buffer*                              next_primitive_lights_identity   = nullptr;
        const Buffer*                              next_light_mapping_identity      = nullptr;
        const Buffer*                              next_light_data_identity         = nullptr;
        const Texture*                             next_local_light_pdf_identity    = nullptr;
        uint64                                     next_scene_revision              = 0;
    };

    PrepareLightPass(class ShaderManager& manager, BindlessArrayRef bindless_array);
    PreparedCommand
    Prepare(RTContext& rt_ctx, const RaytracingSceneFrameSnapshot& scene_snapshot, uint64 scene_revision);
    void Process(class CommandList& cmd_list, const PreparedCommand& command);
    void RecordLightingInputTransitions(CommandList& cmd_list, const PreparedCommand& command);
    void RecordAcceptedBoundary(CommandList& cmd_list, const PreparedCommand& command);
    bool AddPasses(RenderGraph& graph, const PreparedCommand& command);
    void CommitAcceptedFrame(RTContext& rt_ctx, PreparedCommand&& command) noexcept;

private:
    RecordResources CaptureResources(const RTContext& rt_ctx) const;
    void            RecordUploads(CommandList& cmd_list, SharedPtr<const RecordPayload> payload);
    void            RecordClears(CommandList& cmd_list, SharedPtr<const RecordPayload> payload);
    void RecordSceneInputTransitions(CommandList& cmd_list, SharedPtr<const RecordPayload> payload);
    void RecordDispatch(CommandList& cmd_list, SharedPtr<const RecordPayload> payload);
    void
    RecordGenerateMips(CommandList& cmd_list, SharedPtr<const RecordPayload> payload, MipDispatch dispatch);

    BindlessArrayRef bindless_array;

    UnorderedMap<uint64, EmissiveLightHistory> instance_light_buffer_offsets;
    UnorderedMap<uint64, uint>                 primitive_light_buffer_offsets;
    const Buffer*                              accepted_primitive_to_light_identity = nullptr;
    const Buffer*                              accepted_tasks_identity              = nullptr;
    const Buffer*                              accepted_primitive_lights_identity   = nullptr;
    const Buffer*                              accepted_light_mapping_identity      = nullptr;
    const Buffer*                              accepted_light_data_identity         = nullptr;
    const Texture*                             accepted_local_light_pdf_identity    = nullptr;
    uint64                                     accepted_scene_revision              = ~uint64{0};

    PrepareLightShaderPipeline prepare_light_pipeline;

    bool b_odd_frame = false;
};

} // namespace Moer::Render::Raytracing

#endif
