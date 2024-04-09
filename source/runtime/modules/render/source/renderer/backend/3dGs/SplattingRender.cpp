#include "SplattingRender.h"
#include "rhi/RHIResource.h"
#include "3DGaissian.h"
#include "RenderThread.h"
#include "rendergraph/RenderGraph.h"
#include "resources/AsyncResources.h"
#include "scene/CameraManager.h"
#include "scene/Scene.h"
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/quaternion.hpp>

#include "shader/ShaderResourceManager.h"
#include <fstream>

IMPLEMENT_SHADER_TYPE(SortHistShader, "3dgs_splatting/hist_temp.hlsl", "main", ST_COMPUTE);
IMPLEMENT_SHADER_TYPE(PrecompCov3dShader, "3dgs_splatting/precomp_cov3d.hlsl", "main", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(PreprocessShader, "3dgs_splatting/preprocess.hlsl", "main", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(PrefixSumShader, "3dgs_splatting/prefix_sum.hlsl", "main", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(PreprocessSortShader, "3dgs_splatting/preprocess_sort.hlsl", "main", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(SortShader, "3dgs_splatting/sort.hlsl", "main", ST_COMPUTE);
IMPLEMENT_SHADER_TYPE(TileBoundaryShader, "3dgs_splatting/tile_boundary.hlsl", "main", ST_COMPUTE);
IMPLEMENT_SHADER_TYPE(RenderShader, "3dgs_splatting/render.hlsl", "main", ST_COMPUTE);

class Moer::SplattingRender::Impl {
public:
    void      Init(const BackendRendererInitInfo& _init_info);
    void      InitBuffers();
    void      ReallocateBuffers();
    void      ShutDown();
    void      DrawFrame();
    void      Present();
    void      SetOriginResolution(uint32_t _width, uint32_t _height);
    void      OnResizeVSwapChain();
    void      SetPresentResolution(uint32_t _width, uint32_t _height);
    RHISRVRef GetRendererOutput();

    void DispatchPrefixSum(RenderGraph& render_graph);
    void DispatchRadixSort(RenderGraph& render_graph);
    void DispatchTileBoundary(RenderGraph& render_graph);
    void DispatchTileRender(RenderGraph& render_graph);
    void UpdateUniform();

    RHIGraphicsCommandList* GetCurrentCmdList() {
        return render_cmd_lists[frame_counter % render_cmd_lists.size()];
    }

protected:
    RHIComputePipelineStateRef m_precomp_cov3d_pipeline;
    RHIComputePipelineStateRef m_preprocess_pipeline;
    RHIComputePipelineStateRef m_pre_fix_sum_pipeline;
    RHIComputePipelineStateRef m_radix_sort_pipeline;
    RHIComputePipelineStateRef m_preprocess_sort_pipeline;
    RHIComputePipelineStateRef m_tile_boundary_pipeline;
    RHIComputePipelineStateRef m_sort_hist_pipeline;
    RHIComputePipelineStateRef m_render_pipeline;

    RHIBufferRef vertexAttributeBuffer;
    RHIBufferRef tileOverlapBuffer;
    RHIBufferRef sortKBufferEven;
    RHIBufferRef sortKBufferOdd;
    RHIBufferRef sortHistBuffer;
    RHIBufferRef totalSumBufferHost;
    RHIBufferRef tileBoundaryBuffer;
    RHIBufferRef sortVBufferEven;
    RHIBufferRef sortVBufferOdd;
    RHIBufferRef sceneVertexBuffer;
    RHIBufferRef sceneCov3DBuffer;

    bool isPrecompCov3d = false;

    RHIBufferRef prefixSumPingBuffer;
    RHIBufferRef prefixSumPongBuffer;

    RHIUAVRef prefixSumPingUAV;
    RHIUAVRef prefixSumPongUAV;

    uint32_t numVertices{0};
    uint32_t sortBufferSizeMultiplier       = 1;
    uint32_t numRadixSortBlocksPerWorkgroup = 32;

    RHIShaderRef precomp_cov3d_shader;
    RHIShaderRef preprocess_shader;
    RHIShaderRef prefix_sum_shader;
    RHIShaderRef preprocess_sort_shader;
    RHIShaderRef sort_shader;
    RHIShaderRef sort_hist_shader;
    RHIShaderRef tile_boundary_shader;
    RHIShaderRef render_shader;

    RHIBufferRef precomp_cov3d_uniform_buffer;
    RHICBVRef    precomp_cov3d_uniform_view;

    RHIBufferRef uniformBuffer;
    RHICBVRef    uniformBufferView;
    Params       uniformParams;

    VirtualViewport* virtual_viewport;
    uint64_t         frame_counter = 0;
    Vector2i         source_resolution;

    Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
    RHICommandQueue*                     render_queue;
    RHIFenceRef                          render_fence;

    uint32_t num_instances{0};
};

struct alignas(16) UniformBuffer {
    Moer::Vector4f camera_position;

    Moer::Matrix4x4f proj_mat;
    Moer::Matrix4x4f view_mat;
    uint32_t         width;
    uint32_t         height;
    float            tan_fovx;
    float            tan_fovy;
};

struct VertexAttributeBuffer {
    Moer::Vector4f  conic_opacity;
    Moer::Vector4f  color_radii;
    Moer::Vector4ui aabb;
    Moer::Vector2f  uv;
    float           depth;
    uint32_t        __padding[1];
};

// struct Camera {
//     Moer::Vector3f position;
//     Moer::Quaternion rotation;
//     float fov;
//     float nearPlane;
//     float farPlane;
//
//     void translate(Moer::Vector3f translation) {
//         position += rotation * translation;
//     }
// };

struct RadixSortPushConstants {
    uint32_t g_num_elements;            // == NUM_ELEMENTS
    uint32_t g_shift;                   // (*)
    uint32_t g_num_workgroups;          // == NUMBER_OF_WORKGROUPS as defined in the section above
    uint32_t g_num_blocks_per_workgroup;// == NUM_BLOCKS_PER_WORKGROUP
};

Moer::Array<uint8_t> read_binary_file(const std::string& filename, const uint32_t count) {
    Moer::Array<uint8_t> data;

    std::ifstream file;

    file.open(filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }

    uint64_t read_count = count;
    if (count == 0) {
        file.seekg(0, std::ios::end);
        read_count = static_cast<uint64_t>(file.tellg());
        file.seekg(0, std::ios::beg);
    }

    data.resize(static_cast<size_t>(read_count));
    file.read(reinterpret_cast<char*>(data.data()), read_count);
    file.close();

    return data;
}

void Moer::SplattingRender::Impl::Init(const BackendRendererInitInfo& _init_info) {

    RenderThreadFence render_thread_fence;
    render_thread_fence.BeginFence();
    render_thread_fence.Wait();
    VirtualViewportCreateInfo create_info;
    source_resolution             = Vector2i(_init_info.width, _init_info.height);
    create_info.name              = "DeferredRendererViewport";
    create_info.extent            = source_resolution;
    create_info.format            = _init_info.format;
    create_info.back_buffer_count = 3;
    virtual_viewport              = MoerNew(VirtualViewport)(create_info);
    render_queue                  = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

    render_cmd_lists.resize(create_info.back_buffer_count);
    for (uint32_t i = 0; i < create_info.back_buffer_count; ++i) {
        render_cmd_lists[i] = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());
    }
    render_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});

    auto& shader_resource_manager = ShaderResourceManager::GetInstance();
    preprocess_shader             = shader_resource_manager.GetShader<PreprocessShader>();
    ShaderCodeEntry entry{};

    prefix_sum_shader      = shader_resource_manager.GetShader<PrefixSumShader>();
    preprocess_sort_shader = shader_resource_manager.GetShader<PreprocessSortShader>();
    sort_shader            = shader_resource_manager.GetShader<SortShader>();

 
    sort_hist_shader     = shader_resource_manager.GetShader<SortHistShader>();
    tile_boundary_shader = shader_resource_manager.GetShader<TileBoundaryShader>();

    render_shader = shader_resource_manager.GetShader<RenderShader>();
    

    precomp_cov3d_shader = shader_resource_manager.GetShader<PrecompCov3dShader>();
    

    m_precomp_cov3d_pipeline   = g_rhi->RHICreateComputePipelineState(precomp_cov3d_shader);
    auto t                     = preprocess_shader->GetMetaShader()->GetShaderMetaType();
    m_preprocess_pipeline      = g_rhi->RHICreateComputePipelineState(preprocess_shader);
    m_pre_fix_sum_pipeline     = g_rhi->RHICreateComputePipelineState(prefix_sum_shader);
    m_radix_sort_pipeline      = g_rhi->RHICreateComputePipelineState(sort_shader);
    m_preprocess_sort_pipeline = g_rhi->RHICreateComputePipelineState(preprocess_sort_shader);
    m_tile_boundary_pipeline   = g_rhi->RHICreateComputePipelineState(tile_boundary_shader);
    m_render_pipeline          = g_rhi->RHICreateComputePipelineState(render_shader);
    m_sort_hist_pipeline       = g_rhi->RHICreateComputePipelineState(sort_hist_shader);
    InitBuffers();
}

void Moer::SplattingRender::Impl::InitBuffers() {
    numVertices           = g_scene->GetBuffer("gs_scene_buffer")->GetByteSize() / sizeof(Vertex);
    sceneCov3DBuffer      = g_rhi->RHICreateBuffer<float>(numVertices * sizeof(float) * 6, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    vertexAttributeBuffer = g_rhi->RHICreateBuffer<float>(numVertices * sizeof(VertexAttributeBuffer), EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    tileOverlapBuffer     = g_rhi->RHICreateBuffer<float>(numVertices * sizeof(uint32_t), EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    uniformBuffer         = g_rhi->RHICreateBuffer<float>(sizeof(UniformBuffer), EBufferUsageFlags::UNIFORM_BUFFER | EBufferUsageFlags::CPU_VISIBLE);
    uniformBufferView     = g_rhi->RHICreateCBV(uniformBuffer);
    prefixSumPingBuffer   = g_rhi->RHICreateBuffer<float>(numVertices * sizeof(uint32_t), EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    prefixSumPongBuffer   = g_rhi->RHICreateBuffer<float>(numVertices * sizeof(uint32_t), EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    totalSumBufferHost    = g_rhi->RHICreateBuffer<float>(sizeof(uint32_t), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::CPU_VISIBLE);

    auto width         = source_resolution.x;
    auto height        = source_resolution.y;
    auto tileX         = (width + 16 - 1) / 16;
    auto tileY         = (height + 16 - 1) / 16;
    tileBoundaryBuffer = g_rhi->RHICreateBuffer<float>(tileX * tileY * sizeof(uint32_t) * 2, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    ReallocateBuffers();
}
void Moer::SplattingRender::Impl::ReallocateBuffers() {

    LOG_INFO("ReallocateBuffers {} {}", num_instances, sortBufferSizeMultiplier);

    uint32_t sort_k_buffer_size = numVertices * sortBufferSizeMultiplier * sizeof(uint64_t);
    sortKBufferEven             = g_rhi->RHICreateBuffer<float>(sort_k_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    sortKBufferOdd              = g_rhi->RHICreateBuffer<float>(sort_k_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    uint32_t sort_v_buffer_size = numVertices * sortBufferSizeMultiplier * sizeof(uint32_t);
    sortVBufferEven             = g_rhi->RHICreateBuffer<float>(sort_v_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
    sortVBufferOdd              = g_rhi->RHICreateBuffer<float>(sort_v_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);

    uint32_t globalInvocationSize = numVertices * sortBufferSizeMultiplier / numRadixSortBlocksPerWorkgroup;
    uint32_t remainder            = numVertices * sortBufferSizeMultiplier % numRadixSortBlocksPerWorkgroup;
    globalInvocationSize += remainder > 0 ? 1 : 0;

    auto     numWorkgroups         = (globalInvocationSize + 256 - 1) / 256;
    uint32_t sort_hist_buffer_size = numWorkgroups * 256 * sizeof(uint32_t);
    sortHistBuffer                 = g_rhi->RHICreateBuffer<float>(sort_hist_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);
}

void Moer::SplattingRender::Impl::ShutDown() {
    RenderThreadFence render_thread_fence;
    render_thread_fence.BeginFence();
    render_thread_fence.Wait();
    MoerDelete(virtual_viewport);
}
void Moer::SplattingRender::Impl::DrawFrame() {
    UpdateUniform();

    EnqueueRenderTask([this]() {
        RHIGraphicsCommandList* cmd_list = GetCurrentCmdList();
        cmd_list->Reset();
        cmd_list->BeginRecording();
        RenderGraph render_graph;

        render_graph.ImportBuffer("vertex_buffer", g_scene->GetBuffer("gs_scene_buffer"));
        render_graph.ImportBuffer("vertex_attribute_buffer", vertexAttributeBuffer);
        render_graph.ImportBuffer("tile_overlap_buffer", tileOverlapBuffer);
        render_graph.ImportBuffer("sort_k_even", sortKBufferEven);
        render_graph.ImportBuffer("sort_k_odd", sortKBufferOdd);
        render_graph.ImportBuffer("sort_hist_buffer", sortHistBuffer);
        //  render_graph.ImportBuffer("total_sum_buffer_host", totalSumBufferHost);
        render_graph.ImportBuffer("prefix_sum_ping", prefixSumPingBuffer);
        render_graph.ImportBuffer("prefix_sum_pong", prefixSumPongBuffer);
        render_graph.ImportBuffer("sort_v_even", sortVBufferEven);
        render_graph.ImportBuffer("sort_v_odd", sortVBufferOdd);
        render_graph.ImportBuffer("tile_boundary_buffer", tileBoundaryBuffer);

        render_graph.ImportBuffer("cov3d_buffer", sceneCov3DBuffer);
        if (!isPrecompCov3d) {
            isPrecompCov3d = true;
            render_graph.AddComputePass(
                "precomp_cov3d", [&](RenderGraph::Builder& _builder) {
                auto vertex_buffer = render_graph.GetBlackBoard().GetHandle("vertex_buffer");
                auto cov3d_buffer = render_graph.GetBlackBoard().GetHandle("cov3d_buffer");
                _builder.ReadBuffers({vertex_buffer}).WriteBuffers({cov3d_buffer});
                _builder.DeclareComputePass({.compute_pipeline =  m_precomp_cov3d_pipeline}); }, [&](RenderPassContext& _context) {
                    auto numGroups = (numVertices + 255) / 256;

                    PrecompCov3dShader::Parameters params;
                    params.vertex_buffer       = render_graph.GetBlackBoard().GetBuffer("vertex_buffer")->GetSRV();
                    params.cov3ds_buffer       = render_graph.GetBlackBoard().GetBuffer("cov3d_buffer")->GetUAV();
                    params.params.scale_factor = 1.f;
                    RHIBatchedShaderParameters batched_params;
                    batched_params.SetParameters(precomp_cov3d_shader, params);
                    g_rhi->RHISetBatchedShaderParameters(m_precomp_cov3d_pipeline, batched_params);
                    _context.cmd_list->Dispatch(numGroups, 1, 1);
                    // _context.cmd_list->Dispatch(numGroups, 1, 1);
                });
        }

        render_graph.AddComputePass(
            "preprocess", [&](RenderGraph::Builder& _builder) {
           auto vertex_buffer           = render_graph.GetBlackBoard().GetHandle("vertex_buffer");
           auto cov3d_buffer            = render_graph.GetBlackBoard().GetHandle("cov3d_buffer");
           auto vertex_attribute_buffer = render_graph.GetBlackBoard().GetHandle("vertex_attribute_buffer");
           auto tile_overlap_buffer     = render_graph.GetBlackBoard().GetHandle("tile_overlap_buffer");
           _builder.ReadBuffers({vertex_buffer, cov3d_buffer})
               .WriteBuffers({vertex_attribute_buffer, tile_overlap_buffer})
               .DeclareComputePass({.compute_pipeline = m_preprocess_pipeline}); }, [&](RenderPassContext& _context) {
                   auto numGroups = (numVertices + 255) / 256;

                   // _context.cmd_list->Dispatch(numGroups, 1, 1);

           RHIBatchedShaderParameters   batched_params;
           PreprocessShader::Parameters params;
           params.cov3ds_buffer = render_graph.GetBlackBoard().GetBuffer("cov3d_buffer")->GetSRV();
           params.vertex_buffer = render_graph.GetBlackBoard().GetBuffer("vertex_buffer")->GetSRV();
           params.vertex_attribute_buffer = render_graph.GetBlackBoard().GetBuffer("vertex_attribute_buffer")->GetUAV();
           params.tile_overlap_buffer = render_graph.GetBlackBoard().GetBuffer("tile_overlap_buffer")->GetUAV();
         //  params.params = uniformParams;
           params.params = uniformBufferView;
           batched_params.SetParameters(preprocess_shader, params);
           g_rhi->RHISetBatchedShaderParameters(m_preprocess_pipeline, batched_params);
           _context.cmd_list->Dispatch(numGroups, 1, 1); });

        DispatchPrefixSum(render_graph);

        render_graph.AddComputePass(
            "preprocess_sort", [&](RenderGraph::Builder& _builder) {
                auto black_board = render_graph.GetBlackBoard();
                auto vertex_attribute_buffer = black_board.GetHandle("vertex_attribute_buffer");
                auto prefix_sum_ping = black_board.GetHandle("prefix_sum_ping");
                auto prefix_sum_pong = black_board.GetHandle("prefix_sum_pong");
                auto sort_k_even = black_board.GetHandle("sort_k_even");
                auto sort_v_even = black_board.GetHandle("sort_v_even");
                _builder.ReadBuffers({vertex_attribute_buffer, prefix_sum_ping})
                    .WriteBuffers({sort_k_even, sort_v_even, prefix_sum_pong})
                    .DeclareComputePass({.compute_pipeline = m_preprocess_sort_pipeline}); }, [&](RenderPassContext& _context) {
           
                if(num_instances > numVertices * sortBufferSizeMultiplier) {
                    while (num_instances > numVertices * sortBufferSizeMultiplier) 
                            sortBufferSizeMultiplier++;
                ReallocateBuffers();
            }
                const auto iters = static_cast<uint32_t>(std::ceil(std::log2(static_cast<float>(numVertices))));
                auto numGroups = (numVertices + 255) / 256;
            PreprocessSortShader::Parameters params;
                        RHIBatchedShaderParameters batched_params;
                        params.attr = render_graph.GetBlackBoard().GetBuffer("vertex_attribute_buffer")->GetSRV();
                        params.keys = render_graph.GetBlackBoard().GetBuffer("sort_k_even")->GetUAV();
                        params.payloads = render_graph.GetBlackBoard().GetBuffer("sort_v_even")->GetUAV();
                        params.prefix_sum = render_graph.GetBlackBoard().GetBuffer(iters%2==0?"prefix_sum_ping":"prefix_sum_pong")->GetSRV();
                        params.params.tileX = (source_resolution.x + 16 - 1) / 16;
                        batched_params.SetParameters(preprocess_sort_shader, params);
                        g_rhi->RHISetBatchedShaderParameters(m_preprocess_sort_pipeline, batched_params);
                
            _context.cmd_list->Dispatch(numGroups, 1, 1); });
        DispatchRadixSort(render_graph);
        DispatchTileBoundary(render_graph);
        DispatchTileRender(render_graph);

        render_graph.SetGraphOutput(render_graph.GetBlackBoard().GetHandle("swapchain_output"));
        render_graph.Execute({render_cmd_lists[frame_counter % render_cmd_lists.size()], virtual_viewport->GetNextBackBufferExtent()});
    });

    // EnqueueRenderTask([this]() {
    //     RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
    //
    //     RHIBarrierDependencyInfo barrier_dependency_info{};
    //     barrier_dependency_info.texture_barriers.resize(1);
    //     auto& attachment_info = barrier_dependency_info.texture_barriers[0];
    //     attachment_info
    //         .SetTexture(virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture())
    //         .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
    //         .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
    //         .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
    //         .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
    //         .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
    //         .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);
    //
    //     cmd_list->SetPipelineBarrier(barrier_dependency_info);
    // });

    {
        auto submit_rendering = [this]() {
            auto* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
            cmd_list->EndRecording();

            auto          back_buffer_info = virtual_viewport->GetBackBufferInfo();
            RHISubmitInfo submit_info;
            submit_info.Wait(back_buffer_info.backbuffer_ready_fence, frame_counter);
            submit_info.Wait(render_fence, frame_counter);
            submit_info.Signal(render_fence, ++frame_counter);

            render_queue->SubmitCommands(1, cmd_list, &submit_info);
        };
        EnqueueRenderTask(std::move(submit_rendering));
    }
}
void Moer::SplattingRender::Impl::Present() {
    EnqueueRenderTask([this]() {
        virtual_viewport->Present(render_fence);
    });
}
void Moer::SplattingRender::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
}
void Moer::SplattingRender::Impl::OnResizeVSwapChain() {
    auto width         = source_resolution.x;
    auto height        = source_resolution.y;
    auto tileX         = (width + 16 - 1) / 16;
    auto tileY         = (height + 16 - 1) / 16;
    tileBoundaryBuffer = g_rhi->RHICreateBuffer<float>(tileX * tileY * sizeof(uint32_t) * 2, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC);

    EnqueueRenderTask([res(this->source_resolution),
                       view_port(this->virtual_viewport),
                       this]() {
        view_port->OnResize(Extent2D(res.x, res.y));
    });
}
void Moer::SplattingRender::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
    if (_width == source_resolution.x && _height == source_resolution.y) {
        return;
    }
    source_resolution = Vector2i(_width, _height);
    OnResizeVSwapChain();
}
RHISRVRef Moer::SplattingRender::Impl::GetRendererOutput() {
    return virtual_viewport->GetPresentTextureSRV();
}

void Moer::SplattingRender::Impl::DispatchPrefixSum(RenderGraph& render_graph) {
    render_graph.AddComputePass(
        "prefix_sum", [&](RenderGraph::Builder& _builder) {
        auto tile_overlap_buffer = render_graph.GetBlackBoard().GetHandle("tile_overlap_buffer");
        auto prefix_buffers = render_graph.GetBlackBoard().GetHandles({ "prefix_sum_ping", "prefix_sum_pong" });
        _builder.ReadBuffer(tile_overlap_buffer).WriteBuffers(prefix_buffers).ReadBuffers(prefix_buffers);
        _builder.DeclareComputePass({ .compute_pipeline = m_pre_fix_sum_pipeline }); }, [&](RenderPassContext& _context) {
        RHICopyBufferInfo copy_info;
        auto tile_overlap_buffer = render_graph.GetBlackBoard().GetBuffer("tile_overlap_buffer");
        copy_info.regions.push_back({ 0, 0, tile_overlap_buffer->GetBuffer()->GetByteSize() });
        auto rdg_prefix_sum_ping_buffer = render_graph.GetBlackBoard().GetBuffer("prefix_sum_ping");
        auto rdg_prefix_sum_pong_buffer = render_graph.GetBlackBoard().GetBuffer("prefix_sum_pong");
        _context.cmd_list->CopyBuffer(copy_info, tile_overlap_buffer->GetBuffer(), rdg_prefix_sum_ping_buffer->GetBuffer());

        RHIBatchedShaderParameters batched_params;
        PrefixSumShader::Parameters params;
        params.src = render_graph.GetBlackBoard().GetBuffer("prefix_sum_ping")->GetUAV();
        params.dst = render_graph.GetBlackBoard().GetBuffer("prefix_sum_pong")->GetUAV();

        const auto iters = static_cast<uint32_t>(std::ceil(std::log2(static_cast<float>(numVertices))));
        auto numGroups = (numVertices + 255) / 256;

        for (uint32_t timestep = 0; timestep <= iters; timestep++) {
            params.params.timestamp = timestep;
            batched_params.SetParameters(prefix_sum_shader, params);
            g_rhi->RHISetBatchedShaderParameters(m_pre_fix_sum_pipeline, batched_params);

            _context.cmd_list->Dispatch(numGroups, 1, 1);

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.buffer_barriers.resize(2);

            auto& barrier = barrier_dependency_info.buffer_barriers[0];
            barrier.SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                   .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                   .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                   .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);
            barrier.SetBuffer(timestep % 2 == 0 ? rdg_prefix_sum_ping_buffer->GetBuffer() : rdg_prefix_sum_pong_buffer->GetBuffer());

            auto & barrier1 = barrier_dependency_info.buffer_barriers[1];
            barrier1.SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ)
                   .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                   .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                   .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);
            barrier1.SetBuffer(timestep % 2 == 0 ? rdg_prefix_sum_pong_buffer->GetBuffer() : rdg_prefix_sum_ping_buffer->GetBuffer());

            _context.cmd_list->SetPipelineBarrier(barrier_dependency_info);
        }

            RHICopyBufferInfo sum_host_copy_info;
            sum_host_copy_info.regions.push_back({ uint32_t((numVertices-1)*sizeof(uint32_t)), 0, sizeof(uint32_t) });
            if(iters %2 == 0) {
                _context.cmd_list->CopyBuffer(sum_host_copy_info, rdg_prefix_sum_ping_buffer->GetBuffer(), totalSumBufferHost);
            }
            else {
                _context.cmd_list->CopyBuffer(sum_host_copy_info, rdg_prefix_sum_pong_buffer->GetBuffer(), totalSumBufferHost);
            }
            auto mapped = g_rhi->RHIMapBuffer(totalSumBufferHost,0,sizeof(uint32_t));
            num_instances = *(uint32_t*)mapped;
            g_rhi->RHIUnmapBuffer(totalSumBufferHost); });
}

static void WriteReadBarrier(RHIBufferRef buffer, RHIGraphicsCommandList* cmd_list) {
    RHIBarrierDependencyInfo barrier_dependency_info{};
    barrier_dependency_info.buffer_barriers.resize(1);
    auto& barrier = barrier_dependency_info.buffer_barriers[0];
    barrier.SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
        .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
        .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
        .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);
    barrier.SetBuffer(buffer);
    cmd_list->SetPipelineBarrier(barrier_dependency_info);
}

static void ReadWriteBarrier(RHIBufferRef buffer, RHIGraphicsCommandList* cmd_list) {
    RHIBarrierDependencyInfo barrier_dependency_info{};
    barrier_dependency_info.buffer_barriers.resize(1);
    auto& barrier = barrier_dependency_info.buffer_barriers[0];
    barrier.SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ)
        .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
        .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
        .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);
    barrier.SetBuffer(buffer);
    cmd_list->SetPipelineBarrier(barrier_dependency_info);
}

void Moer::SplattingRender::Impl::DispatchRadixSort(RenderGraph& render_graph) {
    render_graph.AddComputePass(
        "sort", [&](RenderGraph::Builder& _builder) {
            auto handles = render_graph.GetBlackBoard().GetHandles({ "sort_k_even", "sort_k_odd", "sort_v_even", "sort_v_odd", "sort_hist_buffer" });
            _builder.ReadBuffers(handles).WriteBuffers(handles).DeclareComputePass({m_sort_hist_pipeline}); }, [&](RenderPassContext& _context) {
            for (auto i = 0; i < 8; i++) {
                _context.cmd_list->SetPipelineState(m_sort_hist_pipeline);
                auto invocationSize = (num_instances + numRadixSortBlocksPerWorkgroup - 1) / numRadixSortBlocksPerWorkgroup;
                 invocationSize = (invocationSize + 255) / 256;
                 RHIBatchedShaderParameters batched_params;
                
                SortParameters sort_constants;
                sort_constants.g_num_instances = num_instances;
                sort_constants.g_num_blocks_per_workgroup = numRadixSortBlocksPerWorkgroup;
                sort_constants.g_shift = i * 8;
                sort_constants.g_num_workgroups = invocationSize;
                
                SortHistShader::Parameters hist_params;
                hist_params.params = sort_constants;
                hist_params.g_elements_in =  render_graph.GetBlackBoard().GetBuffer(i%2==0? "sort_k_even":"sort_k_odd")->GetSRV();
                hist_params.g_histograms = render_graph.GetBlackBoard().GetBuffer("sort_hist_buffer")->GetUAV();
                batched_params.SetParameters(sort_hist_shader, hist_params);
                g_rhi->RHISetBatchedShaderParameters(m_sort_hist_pipeline, batched_params);
                _context.cmd_list->Dispatch(invocationSize, 1, 1);

                WriteReadBarrier(render_graph.GetBlackBoard().GetBuffer("sort_hist_buffer")->GetBuffer(), _context.cmd_list);

                _context.cmd_list->SetPipelineState(m_radix_sort_pipeline);
                SortShader::Parameters sort_params;
                sort_params.params = sort_constants;
                sort_params.g_elements_in = render_graph.GetBlackBoard().GetBuffer(i % 2 == 0 ? "sort_k_even" : "sort_k_odd")->GetSRV();
                sort_params.g_elements_out = render_graph.GetBlackBoard().GetBuffer(i % 2 == 0 ? "sort_k_odd" : "sort_k_even")->GetUAV();
                sort_params.g_payload_in = render_graph.GetBlackBoard().GetBuffer(i % 2 == 0 ? "sort_v_even" : "sort_v_odd")->GetSRV();
                sort_params.g_payload_out = render_graph.GetBlackBoard().GetBuffer(i % 2 == 0 ? "sort_v_odd" : "sort_v_even")->GetUAV();
                sort_params.g_histograms = render_graph.GetBlackBoard().GetBuffer("sort_hist_buffer")->GetSRV();
                batched_params.SetParameters(sort_shader, sort_params);
                g_rhi->RHISetBatchedShaderParameters(m_radix_sort_pipeline, batched_params);
                _context.cmd_list->Dispatch(invocationSize, 1, 1);

                if(i%2==0) {
                    ReadWriteBarrier(render_graph.GetBlackBoard().GetBuffer("sort_k_odd")->GetBuffer(), _context.cmd_list);
                    ReadWriteBarrier(render_graph.GetBlackBoard().GetBuffer("sort_v_odd")->GetBuffer(), _context.cmd_list);
                }
                else {
                    ReadWriteBarrier(render_graph.GetBlackBoard().GetBuffer("sort_k_even")->GetBuffer(), _context.cmd_list);
                    ReadWriteBarrier(render_graph.GetBlackBoard().GetBuffer("sort_v_even")->GetBuffer(), _context.cmd_list);
                }
                
              } });
}
void Moer::SplattingRender::Impl::DispatchTileBoundary(RenderGraph& render_graph) {
    render_graph.AddComputePass(
        "tile_boundary", [&](RenderGraph::Builder& _builder) {
            auto sort_k_buffer_even = render_graph.GetBlackBoard().GetHandle("sort_k_even");
            auto tile_boundary_buffer = render_graph.GetBlackBoard().GetHandle("tile_boundary_buffer");
            _builder.ReadBuffers({sort_k_buffer_even}).WriteBuffers({tile_boundary_buffer}).DeclareComputePass({m_tile_boundary_pipeline}); }, [&](RenderPassContext& _context) {
                RHIBatchedShaderParameters batched_params;
                TileBoundaryShader::Parameters params;
                params.params.num_instances = num_instances;
                params.sort_list = render_graph.GetBlackBoard().GetBuffer("sort_k_even")->GetSRV();
                params.sort_out = render_graph.GetBlackBoard().GetBuffer("tile_boundary_buffer")->GetUAV();
                batched_params.SetParameters(tile_boundary_shader, params);
                g_rhi->RHISetBatchedShaderParameters(m_tile_boundary_pipeline, batched_params);
                _context.cmd_list->Dispatch((num_instances+ 255) / 256,1, 1); });
}
void Moer::SplattingRender::Impl::DispatchTileRender(RenderGraph& render_graph) {
    render_graph.AddComputePass(
        "render", [&](RenderGraph::Builder& _builder) {
            auto output = render_graph.ImportTexture("swapchain_output", virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture());
            auto tile_boundary_buffer = render_graph.GetBlackBoard().GetHandle("tile_boundary_buffer");
            auto vertex_attribute_buffer = render_graph.GetBlackBoard().GetHandle("vertex_attribute_buffer");
            auto sort_v_even = render_graph.GetBlackBoard().GetHandle("sort_v_even");
            _builder
          .WriteTexture({output},ETextureUsageFlags::UNORDERED_ACCESS)
            .ReadBuffers({vertex_attribute_buffer,tile_boundary_buffer,sort_v_even}).DeclareComputePass({m_render_pipeline}); }, [&](RenderPassContext& _context) {
            RHIBatchedShaderParameters batched_params;
            RenderShader::Parameters   params;
            params.params.width            = source_resolution.x;
            params.params.height           = source_resolution.y;
            params.output_image            = render_graph.GetBlackBoard().GetTexture("swapchain_output")->GetUAV();
            params.vertex_attribute_buffer = render_graph.GetBlackBoard().GetBuffer("vertex_attribute_buffer")->GetSRV();
            params.boundaries              = render_graph.GetBlackBoard().GetBuffer("tile_boundary_buffer")->GetSRV();
            params.sorted_vertices         = render_graph.GetBlackBoard().GetBuffer("sort_v_even")->GetSRV();
            batched_params.SetParameters(render_shader, params);
            g_rhi->RHISetBatchedShaderParameters(m_render_pipeline, batched_params);

            _context.cmd_list->Dispatch((source_resolution.x + 15) / 16, (source_resolution.y + 15) / 16, 1); });
}

Moer::Matrix4x4f FromGlmMatrix(const glm::mat4& _mat) {
    Moer::Matrix4x4f mat;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++)
            mat[i][j] = _mat[i][j];
    }
    return mat;
}

void Moer::SplattingRender::Impl::UpdateUniform() {
    auto camera_entity            = g_scene->GetMainCamera();
    auto camera                   = CameraManager::Get().Get(camera_entity);
    uniformParams.camera_position = Vector4f(camera->GetPosition(), 1.0f);
    uniformParams.proj_mat        = camera->GetProjectionMatrix() * camera->GetViewMatrix();
    uniformParams.view_mat        = camera->GetViewMatrix();
    uniformParams.width           = source_resolution.x;
    uniformParams.height          = source_resolution.y;
    uniformParams.tan_fovx        = camera->GetTanHalfFov();
    uniformParams.tan_fovy        = camera->GetTanHalfFov() * static_cast<float>(source_resolution.y) / static_cast<float>(source_resolution.x);

    auto rotation    = glm::mat4_cast(glm::quat());
    auto translation = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f));
    auto view        = glm::inverse(translation * rotation);
    // auto inverse_translation = glm::inverse(translation);
    // auto inverse_rotation = glm::inverse(rotation);

    auto width  = source_resolution.x;
    auto height = source_resolution.y;

    float tan_fovx = std::tan(glm::radians(45.f) / 2.0);
    float tan_fovy = tan_fovx * static_cast<float>(source_resolution.y) / static_cast<float>(source_resolution.x);

    auto proj = glm::perspective(std::atan(tan_fovy) * 2.0f,
                                 static_cast<float>(width) / static_cast<float>(height),
                                 0.1f,
                                 1000.f) *
                view;
    // view[0][1] *= -1.0f;
    // view[1][1] *= -1.0f;
    // view[2][1] *= -1.0f;
    // view[3][1] *= -1.0f;
    // view[0][2] *= -1.0f;
    // view[1][2] *= -1.0f;
    // view[2][2] *= -1.0f;
    // view[3][2] *= -1.0f;
    //
    // proj[0][1] *= -1.0f;
    // proj[1][1] *= -1.0f;
    // proj[2][1] *= -1.0f;
    // proj[3][1] *= -1.0f;

    uniformParams.view_mat = FromGlmMatrix(view);
    uniformParams.proj_mat = FromGlmMatrix(proj);
    uniformParams.tan_fovx = tan_fovx;
    uniformParams.tan_fovy = tan_fovy;

    uniformParams.view_mat.t[0][1] *= -1.0f;
    uniformParams.view_mat.t[1][1] *= -1.0f;
    uniformParams.view_mat.t[2][1] *= -1.0f;
    uniformParams.view_mat.t[3][1] *= -1.0f;
    uniformParams.view_mat.t[0][2] *= -1.0f;
    uniformParams.view_mat.t[1][2] *= -1.0f;
    uniformParams.view_mat.t[2][2] *= -1.0f;
    uniformParams.view_mat.t[3][2] *= -1.0f;
    uniformParams.proj_mat.t[0][1] *= -1.0f;
    uniformParams.proj_mat.t[1][1] *= -1.0f;
    uniformParams.proj_mat.t[2][1] *= -1.0f;
    uniformParams.proj_mat.t[3][1] *= -1.0f;

    // uniformParams.view_mat[1][1] *= -1.0f;
    // uniformParams.view_mat[2][1] *= -1.0f;
    // uniformParams.view_mat[3][1] *= -1.0f;
    // uniformParams.view_mat[0][2] *= -1.0f;

    auto buffer_mapped = g_rhi->RHIMapBuffer(uniformBuffer, 0, sizeof(UniformBuffer));
    memcpy(buffer_mapped, &uniformParams, sizeof(UniformBuffer));
    g_rhi->RHIUnmapBuffer(uniformBuffer);
}

void Moer::SplattingRender::Init(const BackendRendererInitInfo& _init_info) {
    impl = MoerNew(Impl)();
    impl->Init(_init_info);
}
void Moer::SplattingRender::DrawFrame() {
    impl->DrawFrame();
}
void Moer::SplattingRender::ShutDown() {
    impl->ShutDown();
}
void Moer::SplattingRender::Present() {
    impl->Present();
}
void Moer::SplattingRender::SetOriginResolution(uint32_t _width, uint32_t _height) {
    impl->SetOriginResolution(_width, _height);
}
void Moer::SplattingRender::SetPresentResolution(uint32_t _width, uint32_t _height) {
    impl->SetPresentResolution(_width, _height);
}
void* Moer::SplattingRender::GetRendererOutput() {
    return impl->GetRendererOutput();
}
Moer::SplattingRender::~SplattingRender() {
    MoerDelete(impl);
}
