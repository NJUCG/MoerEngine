#ifndef MOER_RENDER_PLATFORM_H
#define MOER_RENDER_PLATFORM_H
#if VULKAN

#elif DX12

#endif
#include "API_Macro.h"
#include "RenderAPI.h"
#include <cstdint>
#include <string>
#include <type_traits>
namespace Moer {
RENDER_API extern const char* RHI_VULKAN_NAME;
RENDER_API extern const char* RHI_D3D12_NAME;
RENDER_API extern const char* RHI_OPENGL_NAME;
RENDER_API extern const char* RHI_METAL_NAME;
namespace RHI {

class RENDER_API GenericRenderPlatformInfo {
    std::string rhi_name;

    uint32_t b_is_pc : 1;
    uint32_t b_is_console : 1;
    uint32_t b_support_debug_view_shaders : 1;

    uint32_t b_support_array_texture_compression : 1;
    uint32_t b_support_distance_fields : 1;
    uint32_t b_support_rgb_color_buffer : 1;
    uint32_t b_support_capsule_shadows : 1;
    uint32_t b_support_percentage_closer_shadows : 1;
    uint32_t b_support_volumetric_fog : 1; // also used for FVVoxelization
    uint32_t b_support_index_buffer_uavs : 1;
    uint32_t b_support_instanced_stereo : 1;
    uint32_t b_support_multiview : 1;
    uint32_t b_support_msaa : 1;
    uint32_t b_support_4component_uav_readwrite : 1;
    uint32_t b_support_colorattachment_writemask : 1;
    uint32_t b_support_raytracing : 1;
    uint32_t b_support_raytracing_procedural_primitive : 1;
    uint32_t
        b_support_raytracing_indirect_instance_data : 1; // Whether instance transforms can be copied from the GPU to the TLAS instances buffer
    uint32_t
        b_support_highend_raytracing_reflections : 1; // Whether fully-featured RT reflections can be used on the platform (with multi-bounce, translucency, etc.)
    uint32_t
        b_support_pathtracing : 1; // Whether real-time path tracer is supported on this platform (avoids compiling unnecessary shaders)
    uint32_t b_support_gpu_skin_cache : 1;
    uint32_t b_support_gpu_scene : 1;
    uint32_t b_support_bytebuffer_computeshaders : 1;
    uint32_t b_support_primitive_shaders : 1;
    uint32_t b_support_uint64_image_atomics : 1;
    uint32_t b_sequire_vendor_extensions_for_atomics : 1;
    uint32_t b_support_temporal_history_upscale : 1;
    uint32_t b_support_rt_index_from_vs : 1;
    uint32_t b_support_wave_operations : 1; // Whether HLSL SM6 shader wave intrinsics are supported
    uint32_t b_support_intrinsic_wave_once : 1;
    uint32_t b_support_conservative_rasterization : 1;
    uint32_t b_sequire_explicit128bit_rt : 1;
    uint32_t b_support_gen5_temporal_aa : 1;
    uint32_t b_targets_tiled_gpu : 1;
    uint32_t b_needs_offline_compiler : 1;
    uint32_t b_support_compute_framework : 1;
    uint32_t b_support_anisotropic_materials : 1;
    uint32_t b_support_dual_source_blending : 1;
    uint32_t b_require_generate_prev_transform_buffer : 1;
    uint32_t b_require_colorattachment_during_raster : 1;
    uint32_t b_require_disable_forward_local_lights : 1;
    uint32_t b_compile_signal_processing_pipeline : 1;
    uint32_t b_support_mesh_shaders_tier0 : 1;
    uint32_t b_support_mesh_shaders_tier1 : 1;
    uint32_t max_mesh_shader_thread_group_size : 10;
    uint32_t b_support_per_fragment_buffer_mask : 1;
    uint32_t b_is_hlsl_cc : 1;
    uint32_t b_support_dxc : 1; // Whether DirectXShaderCompiler (DXC) is supported
    uint32_t b_support_variable_rate_shading : 1;
    uint32_t number_of_compute_threads : 10;
    uint32_t b_water_uses_simple_forward_shading : 1;
    uint32_t b_support_water_indirect_draw : 1;
    uint32_t b_support_hair_strand_geometry : 1;
    uint32_t b_support_dof_hybrid_scattering : 1;
    uint32_t b_support_hzb_occlusion : 1;

    uint32_t b_support_async_pipeline_compilation : 1;
    uint32_t b_support_manual_vertex_fetch : 1;
    uint32_t b_override_fmaterial_needs_gbuffer_enabled : 1;
    uint32_t b_support_FFTBloom : 1;
    uint32_t b_support_inline_ray_tracing : 1;
    uint32_t b_support_raytracing_shaders : 1;
    uint32_t b_support_vertex_shader_layer : 1;
    uint32_t b_support_3d_texture_atomics : 1;

    GenericRenderPlatformInfo() {}

private:
    void InitDefaultValues();

public:
    void Initialize();
    void ParseValuesFromConfiguration(GenericRenderPlatformInfo& target_platform_info);
};
} // namespace RHI
} // namespace Moer
#endif // !MOER_PLATFORM
