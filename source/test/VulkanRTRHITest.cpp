//
// Created by 74535 on 2023/10/10.
//

#include <GLFW/glfw3.h>

#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"

// global shader

#include "rhi/RHICommand.h"

class TestRayGenShader : public Shader {
    DEFINE_SHADER_TYPE(TestRayGenShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_SRV(AccelerationStructure, rs)
    DEFINE_SHADER_PARAM_UAV(RWTexture2D, image)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
IMPLEMENT_SHADER_TYPE(TestRayGenShader, "raytracingbasic/raygen.rgen", "main", EShaderType::ST_RAY_GEN);

class TestRayMissShader : public Shader {
    DEFINE_SHADER_TYPE(TestRayMissShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_CBV(ConstantBuffer, clearValue)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
IMPLEMENT_SHADER_TYPE(TestRayMissShader, "raytracingbasic/miss.rmiss", "main", EShaderType::ST_RAY_MISS);

class TestRayClosestHitShader : public Shader {
    DEFINE_SHADER_TYPE(TestRayClosestHitShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
IMPLEMENT_SHADER_TYPE(
    TestRayClosestHitShader,
    "raytracingbasic/closesthit.rchit",
    "main",
    EShaderType::ST_RAY_CLOSESTHIT
);

void Init(int argc, char** argv) {
    g_rhi                           = new VulkanRHIImpl();
    std::filesystem::path workspace = argv[0];
    Moer::ConfigManager::GetInstance().Init(workspace.parent_path());
    Moer::SurfaceInitInfo info{};
    info.title = "vulkan raytracing rt rhi test";
    Moer::WindowContext::Init(info);

    ShaderCompiler::Init();

    Moer::TaskSystem::Init();
    Moer::LogSystem::Init();

    EShaderPlatform platform = GetShaderPlatformByRHIType(g_rhi->GetType());
    ShaderResourceManager::Init(platform);
    ShaderResourceManager::GetInstance().PrepareGlobalShaderResources();

    const auto& config_data = Moer::ConfigManager::GetInstance().GetConfig();

    g_rhi->Initialize({config_data.engine.rhi.max_frame_in_flight, false});
    g_rhi->PostInit();
}

RHIBufferRef CreateBufferFromData(uint64_t size, EBufferUsageFlags usage, void* data) {
    RHIBufferRef buffer = g_rhi->RHICreateBuffer<std::byte>(size, usage);
    void*        dst    = g_rhi->RHIMapBuffer(buffer, 0, size);
    std::memcpy(dst, data, size);
    g_rhi->RHIUnmapBuffer(buffer);
    return buffer;
}

void Test() {
    uint32_t       index_data[]  = {0, 1, 2};
    Moer::Vector3f vertex_data[] = {
        {0, -0.5, 1},
        {-0.5, 0.5, 1},
        {0.5, 0.5, 1},

    };
    RHIBufferRef index_buffer = g_rhi->RHICreateBuffer<uint32_t>(
        sizeof(index_data),
        EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT
    );
    void* index_dst = g_rhi->RHIMapBuffer(index_buffer, 0, sizeof(index_data));
    memcpy(index_dst, index_data, sizeof(index_data));
    g_rhi->RHIUnmapBuffer(index_buffer);

    RHIBufferRef vertex_buffer = g_rhi->RHICreateBuffer<float>(
        sizeof(vertex_data),
        EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT
    );
    void* vertex_dst = g_rhi->RHIMapBuffer(vertex_buffer, 0, sizeof(vertex_data));
    memcpy(vertex_dst, vertex_data, sizeof(vertex_data));
    g_rhi->RHIUnmapBuffer(vertex_buffer);

    Moer::Vector3f clear_value    = {0, 0, 0.2};
    RHIBufferRef   uniform_buffer = CreateBufferFromData(
        sizeof(clear_value), EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::CONSTANT_BUFFER, &clear_value
    );

    RHIRayTracingTrianglesGeometry simple_triangle;
    simple_triangle.index_buffer         = index_buffer;
    simple_triangle.index_element_type   = IET_UINT32;
    simple_triangle.max_vertex_count     = 6;
    simple_triangle.transform_buffer     = nullptr;
    simple_triangle.vertex_buffer        = vertex_buffer;
    simple_triangle.vertex_buffer_stride = sizeof(Moer::Vector3f);
    simple_triangle.vertex_element_type  = PF_R32G32B32_SFLOAT;

    Moer::Array<RHIRayTracingBLASGeometry> blas_geometries;
    RHIRayTracingBLASGeometry              blas_geo{};
    blas_geo.flags              = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    blas_geo.geometry.triangles = simple_triangle;
    blas_geo.geo_type           = RTGT_TRIANGLES;
    blas_geometries.push_back(blas_geo);

    Moer::Array<RHIRayTracingBLASGeometryRangeInfo> blas_range_infos;
    RHIRayTracingBLASGeometryRangeInfo              blas_range_info{};
    blas_range_info.first_vertex     = 0;
    blas_range_info.primitive_count  = 2;
    blas_range_info.primtive_offset  = 0;
    blas_range_info.transform_offset = 0;
    blas_range_infos.push_back(blas_range_info);

    RHIRayTracingBLASInitializer init_blas{};
    init_blas.build_flags = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD |
                            ERayTracingAccelerationStructureBuildFlags::ALLOW_COMPACTION;
    init_blas.geometries  = blas_geometries;
    init_blas.range_infos = blas_range_infos;

    RHIRayTracingBLASRef blas = g_rhi->RHIBuildRayTracingBLAS(init_blas);

    Moer::Array<RHIRayTracingInstance> tlas_instances{};
    RHIRayTracingInstance              tlas_instance{};
    tlas_instance.blas                = blas;
    tlas_instance.custom_index        = 0;
    tlas_instance.flags               = ERayTracingInstanceFlags::TRIANGLE_CULL_DISABLE;
    tlas_instance.instance_mask       = 0xFF;
    tlas_instance.instance_sbt_offset = 0;
    tlas_instance.transform           = RHITransformMatrix();
    tlas_instances.push_back(tlas_instance);

    RHIRayTracingTLASInitializer init_tlas{};
    init_tlas.build_flags     = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD;
    init_tlas.instances       = tlas_instances;
    RHIRayTracingTLASRef tlas = g_rhi->RHIBuildRayTracingTLAS(init_tlas);

    RHIRayTracingPipelineStateInitializer init_rt_pipeline{};

    RHIShaderRef test_raygen_shader  = ShaderResourceManager::GetInstance().GetShader<TestRayGenShader>();
    RHIShaderRef test_raymiss_shader = ShaderResourceManager::GetInstance().GetShader<TestRayMissShader>();
    RHIShaderRef test_raychit_shader =
        ShaderResourceManager::GetInstance().GetShader<TestRayClosestHitShader>();

    init_rt_pipeline.SetRayGenShader(dynamic_cast<RHIRayGenShader*>(test_raygen_shader.Get()));
    init_rt_pipeline.AddMissShader(dynamic_cast<RHIRayMissShader*>(test_raymiss_shader.Get()));
    init_rt_pipeline.AddHitShaderGroup(dynamic_cast<RHIRayClosestHitShader*>(test_raychit_shader.Get()));

    RHIRTPsoRef rt_pipeline = g_rhi->RHICreateRayTracingPipelineState(init_rt_pipeline);

    const Moer::Vector2i attachment_size(1920, 1080);
    RHITextureCreateInfo tex_info;
    tex_info.SetDimension(ETextureDimension::TEX_2D)
        .SetFormat(PF_R8G8B8A8_UNORM)
        .SetPreferredLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED)
        .SetExtent(attachment_size)
        .SetClearAttachment(RHIClearAttachment())
        .SetDepth(1)
        .SetArraySize(1)
        .SetNumMips(1)
        .SetNumSamples(1)
        .SetUsageFlags(ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_SRC);

    //out_put texture
    RHITextureRef tex         = g_rhi->RHICreateTexture(tex_info);
    RHIUAVRef     tex_view    = g_rhi->RHICreateTextureUAV(tex, PF_R8G8B8A8_UNORM, 0, 0, 1);
    RHISRVRef     as_view     = g_rhi->RHICreateAccelerationStructureSRV(tlas);
    RHICBVRef     buffer_view = g_rhi->RHICreateCBV(uniform_buffer, sizeof(clear_value), 0);

    RHIBatchedShaderParameters batched_parameter{};
    const_cast<Moer::Array<RHIShaderResourceParameter>&>(batched_parameter.GetResourceParameters())
        .push_back(RHIShaderResourceParameter{as_view.Get(), 0, 0});
    const_cast<Moer::Array<RHIShaderResourceParameter>&>(batched_parameter.GetResourceParameters())
        .push_back({tex_view.Get(), 1, 0});
    const_cast<Moer::Array<RHIShaderResourceParameter>&>(batched_parameter.GetResourceParameters())
        .push_back({buffer_view.Get(), 0, 1});

    g_rhi->RHISetBatchedShaderParameters(rt_pipeline, batched_parameter);

    RHIRayTracingCommandList* command_list =
        g_rhi->RHICreateRayTracingCommandList(g_rhi->RHIGetCurrentCommandAllocator());

    RHICopyCommandList* copy_command_list =
        g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
    RHICommandQueue* rt_queue   = g_rhi->RHICreateCommandQueue(ECommandQueueType::RAYTRACING);
    RHICommandQueue* copy_queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);

    RHIFenceRef raytracing_finish_fence = g_rhi->RHICreateFence({EFenceUsageFlags::TIMELINE});
    RHIFenceRef copying_finish_fence    = g_rhi->RHICreateFence({EFenceUsageFlags::PRESENT});
    uint64_t    raytracing_fence_value  = 0;
    uint64_t    copying_fence_value     = 0;

    RHIViewportRef viewport = g_rhi->RHIGetMainViewport();

    copy_command_list->BeginRecording();

    RHITextureBarrierInfo texture_barrier_info{};
    texture_barrier_info.dst_stage  = PS_ALL_COMMANDS;
    texture_barrier_info.dst_layout = TEXTURE_LAYOUT_COMMON;
    texture_barrier_info.src_stage  = PS_ALL_COMMANDS;
    texture_barrier_info.src_layout = TEXTURE_LAYOUT_UNDEFINED;
    texture_barrier_info.p_texture  = tex;

    copy_command_list->SetPipelineBarrier({{}, {}, {texture_barrier_info}});

    for (int i = 0; i < viewport->GetViewportInfo().max_frame_in_flight; ++i) {
        RHIUAVRef uav                  = g_rhi->RHIGetViewportBackBufferUAV(viewport, i);
        texture_barrier_info.p_texture = uav->GetTexture();
        copy_command_list->SetPipelineBarrier({{}, {}, {texture_barrier_info}});
    }

    copy_command_list->EndRecording();

    RHISubmitInfo transiton_submit_info{};
    copy_queue->SubmitCommands(1, copy_command_list, &transiton_submit_info);
    copy_queue->WaitForQueueComplete();

    void* data = g_rhi->RHIMapBuffer(uniform_buffer, 0, sizeof(Moer::Vector3f));

    while (1) {
        command_list->Reset();
        uint32_t ref_cnt = as_view->GetRefCount();
        LOG_INFO("ref count: {}", ref_cnt);
        command_list->BeginRecording();
        command_list->SetPipelineState(rt_pipeline);
        command_list->TraceRay(1920, 1080, 1);
        command_list->EndRecording();

        RHISubmitInfo raytracing_submit_info{};
        raytracing_submit_info.Signal(raytracing_finish_fence, ++raytracing_fence_value);
        raytracing_submit_info.Wait(copying_finish_fence, copying_fence_value);
        rt_queue->SubmitCommands(1, command_list, &raytracing_submit_info);

        RHIViewportNextBackBufferInfo next_back_buffer_info =
            g_rhi->RHIGetNextFrameViewportBufferInfo(viewport);
        RHIUAVRef next_back_buffer =
            g_rhi->RHIGetViewportBackBufferUAV(viewport, next_back_buffer_info.backbuffer_index);
        copy_command_list->Reset();
        copy_command_list->BeginRecording();
        RHICopyTextureInfo copyinfo;
        copyinfo.dst_layout            = TEXTURE_LAYOUT_COMMON;
        copyinfo.dst_offset            = {0, 0, 0};
        copyinfo.dst_slice.array_count = 1;
        copyinfo.dst_slice.array_index = 0;
        copyinfo.dst_slice.aspect      = ETextureAspectFlags::COLOR;
        copyinfo.dst_slice.mip_index   = 0;

        copyinfo.src_layout            = TEXTURE_LAYOUT_COMMON;
        copyinfo.src_offset            = {0, 0, 0};
        copyinfo.src_slice.array_count = 1;
        copyinfo.src_slice.array_index = 0;
        copyinfo.src_slice.aspect      = ETextureAspectFlags::COLOR;
        copyinfo.src_slice.mip_index   = 0;

        copyinfo.extent                 = {1920, 1080, 1};
        texture_barrier_info.dst_layout = TEXTURE_LAYOUT_COMMON;
        texture_barrier_info.dst_access = ERHIAccessFlags::TRANSFER_WRITE;
        texture_barrier_info.dst_stage  = PS_TRANSFER;
        texture_barrier_info.src_layout = TEXTURE_LAYOUT_UNDEFINED;
        texture_barrier_info.src_access = ERHIAccessFlags::UNDEFINED;
        texture_barrier_info.src_stage  = PS_TOP_OF_PIPE;
        texture_barrier_info.p_texture  = next_back_buffer->GetTexture();
        copy_command_list->SetPipelineBarrier({{}, {}, {texture_barrier_info}});
        copy_command_list->CopyTexture(copyinfo, tex, next_back_buffer->GetTexture());
        texture_barrier_info.dst_layout = TEXTURE_LAYOUT_PRESENT_SRC;
        texture_barrier_info.dst_access = ERHIAccessFlags::UNDEFINED;
        texture_barrier_info.dst_stage  = PS_BOTTOM_OF_PIPE;
        texture_barrier_info.src_layout = TEXTURE_LAYOUT_COMMON;
        texture_barrier_info.src_access = ERHIAccessFlags::TRANSFER_WRITE;
        texture_barrier_info.src_stage  = PS_TRANSFER;
        texture_barrier_info.p_texture  = next_back_buffer->GetTexture();
        copy_command_list->SetPipelineBarrier({{}, {}, {texture_barrier_info}});
        copy_command_list->EndRecording();

        RHISubmitInfo copying_submit_info{};
        copying_submit_info.Wait(next_back_buffer_info.backbuffer_ready_fence, 0);
        copying_submit_info.Wait(raytracing_finish_fence, raytracing_fence_value);
        copying_submit_info.Signal(copying_finish_fence, ++copying_fence_value);

        copy_queue->SubmitCommands(1, copy_command_list, &copying_submit_info);

        g_rhi->RHIPresentViewport(viewport, copying_finish_fence);

        copy_queue->WaitForQueueComplete();
    }
}

int main(int argc, char** argv) {
    Init(argc, argv);
    Test();
    return 0;
}