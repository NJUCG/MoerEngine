//
// Created by 74535 on 2023/10/10.
//

#include <GLFW/glfw3.h>

#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"

// global shader

#include "rhi/RHICommand.h"
RHIBufferRef CreateBufferFromData(const RHIBufferCreateInfo& info, uint32_t size, void* data) {
    RHIBufferRef buffer     = g_rhi->RHICreateBuffer(info);
    void*        mapped_ptr = g_rhi->RHIMapBuffer(buffer, 0, size);
    memcpy(mapped_ptr, data, size);
    g_rhi->RHIUnmapBuffer(buffer);
    return buffer;
}

void Test(int argc, char** argv) {
    g_rhi                           = new VulkanRHIImpl();
    std::filesystem::path workspace = argv[0];
    Moer::ConfigManager::GetInstance().Init(workspace.parent_path());
    Moer::SurfaceInitInfo info{};
    Moer::WindowContext::Init(info);

    g_rhi->Initialize(RHIInitInfo());
    g_rhi->PostInit();

    uint32_t            index_data[]  = {0, 1, 2};
    Moer::Vector3f      vertex_data[] = {{0, -0.5, 1},
                                         {-0.5, 0.5, 1},
                                         {0.5, 0.5, 1}};
    RHIBufferCreateInfo index_buffer_info{};
    index_buffer_info.size    = sizeof(index_data);
    index_buffer_info.stride  = sizeof(uint32_t);
    index_buffer_info.usage   = EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE;
    RHIBufferRef index_buffer = CreateBufferFromData(index_buffer_info, sizeof(index_data), index_data);

    RHIBufferCreateInfo vertex_buffer_info{};
    vertex_buffer_info.size    = sizeof(vertex_data);
    vertex_buffer_info.stride  = sizeof(Moer::Vector3f);
    vertex_buffer_info.usage   = EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE;
    RHIBufferRef vertex_buffer = CreateBufferFromData(vertex_buffer_info, sizeof(vertex_data), vertex_data);
    
    RHIRayTracingTrianglesGeometry simple_triangle;
    simple_triangle.index_buffer   = index_buffer;
    simple_triangle.index_element_type = IET_UINT32;
    simple_triangle.max_vertex_count   = 3;
    simple_triangle.transform_buffer   = nullptr;
    simple_triangle.vertex_buffer      = vertex_buffer;
    simple_triangle.vertex_buffer_stride = sizeof(Moer::Vector3f);
    simple_triangle.vertex_element_type  = PF_R32G32B32_SFLOAT;

    Moer::Array<RHIRayTracingBLASGeometry> blas_geometries;
    RHIRayTracingBLASGeometry              blas_geo{};
    blas_geo.flags = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    blas_geo.geometry.triangles = simple_triangle;
    blas_geo.geo_type = RTGT_TRIANGLES;
    blas_geometries.push_back(blas_geo);

    Moer::Array<RHIRayTracingBLASGeometryRangeInfo> blas_range_infos;
    RHIRayTracingBLASGeometryRangeInfo blas_range_info{};
    blas_range_info.first_vertex = 0;
    blas_range_info.primitive_count = 1;
    blas_range_info.primtive_offset = 0;
    blas_range_info.transform_offset = 0;
    blas_range_infos.push_back(blas_range_info);

    RHIRayTracingBLASInitializer init_blas{};
    init_blas.build_flags = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_BUILD;
    init_blas.geometries  = blas_geometries;
    init_blas.range_infos = blas_range_infos;

    Moer::Array<RHIRayTracingBLASRef> blas = g_rhi->RHIBuildRayTracingBLAS({init_blas});


}

int main(int argc, char** argv) {
    Test(argc, argv);
    return 0;
}