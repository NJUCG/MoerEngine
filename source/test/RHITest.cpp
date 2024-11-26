#include <filesystem>
#include <vcruntime_string.h>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "modules/render/source/rhi/RHIImpl.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "core/include/Core.h"
#include "modules/resource/include/loader/LoaderInterface.h"
#include "renderer/UIRenderer.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"

using namespace Moer::Render;
using namespace Moer;

static bool b_show_demo        = false;
static bool b_show_scene_color = true;
class TestTrianglePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipeline);
    DEFINE_SHADER_BUFFER(buffer);
    DEFINE_SHADER_ARGS(buffer);
};

struct TestBindlessParam {
    float4     color;
    uint       texture_handle;
    uint       buffer_handle;
    uint       instance_buffer_handle;
    Matrix4x4f camera_view_proj;
};

struct MaterialPassBindlessParam {
    uint material_type;
    uint light_buffer;
    uint material_buffer;
    uint v_buffer;
    uint g_buffer_normal;
    uint g_buffer_uv;
    uint g_buffer_depth;
    uint gbuffer_position;
    uint global_param_handle;
};

struct LightingData {
    Matrix4x4f inv_view_proj;
    uint       light_count;
    uint3      padding;
    float3     camera_position;
};

class TestTrianglePipelineConstColor : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineConstColor);
    DEFINE_SHADER_CONSTANT_STRUCT(TestBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_TEX(texture);
    DEFINE_SHADER_SAMPLER(defaultSampler);
    DEFINE_SHADER_ARGS(defaultSampler, texture, bdls, param);
};

class TestTrianglePipelineBdls : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineBdls);
    DEFINE_SHADER_ARGS();
};

class CombineUIPipeline : public RasterPipeline {

public:
    struct Param {
        float2 min_xy;
        float2 max_xy;
    };

    DEFINE_RASTER_PIPELINE_CLASS(CombineUIPipeline);
    DEFINE_SHADER_TEX(scene_color);
    DEFINE_SHADER_TEX(gui_color);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(Param, scene_rect);

    DEFINE_SHADER_ARGS(scene_color, gui_color, linear_sampler, scene_rect);
};

static float2 scene_color_resolution = {1280, 720};
static float2 scene_color_pos        = {0, 0};

static void ShowSceneColor(bool* _b_show) {

    ImGuiIO&         io           = ImGui::GetIO();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    if (!*_b_show) {
        return;
    }
    if (!ImGui::Begin("Scene Color", _b_show, window_flags)) {

        ImGui::End();
        return;
    }
    float2 scene_size = {0, 0};

    static float2 xy_ratio = {16, 9};
    // auto          menu_rect = ImGui::GetCurrentWindow()->MenuBarRect();

    auto* current_window    = ImGui::FindWindowByName("Scene Color");
    bool  b_separate_window = current_window->ParentWindow == nullptr;
    auto  menu_rect         = current_window->MenuBarRect();

    scene_size.x = current_window->Size.x;
    scene_size.y = current_window->Size.y + current_window->Pos.y - menu_rect.Max.y;

    auto   window_rect = current_window->Rect();// this is main window rect
    ImRect parent_rect{};

    if (b_separate_window) {
    } else {
        parent_rect = current_window->ParentWindow->Rect();
    }

    float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};
    // LOG_INFO("window_rect: {} {} {} {}", window_rect.Min.x, window_rect.Min.y, window_rect.Max.x, window_rect.Max.y);

    //calculate final pos and size base on xy_ratio

    // if (scene_size.x / scene_size.y > xy_ratio.x / xy_ratio.y) {
    //     scene_size.x = scene_size.y * xy_ratio.x / xy_ratio.y;
    // } else {
    //     scene_size.y = scene_size.x * xy_ratio.y / xy_ratio.x;
    // }
    // scene_pos.x += (ImGui::GetWindowWidth() - scene_size.x) / 2;
    // scene_pos.y += (ImGui::GetWindowHeight() - scene_size.y) / 2;

    scene_color_resolution = {scene_size.x, scene_size.y};
    scene_color_pos        = {local_pos.x, local_pos.y};
    ImGui::End();
}
class MaterialShadingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(MaterialShadingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(MaterialPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

// Post Process Pipeline Definition
struct PostProcessPipelineBindlessParam {
    uint input_image;
};
class PostProcessPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(PostProcessPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(PostProcessPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

// FXAA Pipeline Definition
struct FxaaPipelineBindlessParam {
    uint   input_image;
    uint   fxaa_mode;
    float2 resolution;
    float2 inv_resolution;
};
class FxaaPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(FxaaPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

static void ShowGUI(bool* _b_show) {

    static bool               opt_fullscreen  = true;
    static bool               opt_padding     = false;
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    // ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
    // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
    // because it would be confusing to have two docking targets within each others.
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar;
    if (opt_fullscreen) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        window_flags |= ImGuiWindowFlags_NoBackground;
    } else {
        dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
    }

    // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
    // and handle the pass-thru hole, so we ask Begin() to not render a background.
    if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
        window_flags |= ImGuiWindowFlags_NoBackground;

    // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
    // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
    // all active windows docked into it will lose their parent and become undocked.
    // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
    // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
    if (!opt_padding)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Editor Menu", _b_show, window_flags);
    if (!opt_padding)
        ImGui::PopStyleVar();

    if (opt_fullscreen)
        ImGui::PopStyleVar(2);

    // Submit the DockSpace
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("Docking Main");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Menu")) {
            // if (ImGui::MenuItem("Reload Current Level")) {
            // }
            // if (ImGui::MenuItem("Save Current Level")) {
            // }
            if (ImGui::MenuItem("Exit")) {
                exit(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {

            ImGui::MenuItem("Scene Color", nullptr, &b_show_scene_color);
            // ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector_window);
            ImGui::MenuItem("Demo", nullptr, &b_show_demo);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

int main(int argc, const char** argv) {

    using namespace Moer::Render;
    using namespace Moer;
    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();
    const auto& rhi_config_as_json = ConfigManager::GetInstance().GetRHIConfigAsJSON();

    DeviceInitInfo info{
        .type           = ERHIType::Vulkan,
        .name           = "RHITest",
        .config_as_json = rhi_config_as_json};
    RenderDevice::Init(std::move(info));
    auto&           device = RenderDevice::Get();
    ShaderManager   manager(device);
    uint2           resolution = {1280, 720};
    SurfaceInitInfo surface_info("Vulkan", resolution.x, resolution.y, "RHITest", false);
    WindowContext::Init(surface_info);

    auto&& scope_exit_window_context_and_etc = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });

    Moer::Render::UIRenderer gui(device);

    auto* window_handle = WindowContext::GetMainWindow();

    auto                buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    SwapchainCreateInfo sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB};
    auto                sc             = device.CreateSwapchain(sc_info);
    Scene               scene          = {};
    BindlessArrayRef    bindless_array = scene.GetBindlessArray();
    auto&               gfx_queue      = device.GetCommandQueue(EQueueType::Graphics);
    auto&               copy_queue     = device.GetCopyQueue();

    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }

    BufferRef copy_queue_buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    {
        CommandList copy_cmd_list;
        copy_cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), copy_queue_buffer->GetView());
        auto evt = copy_queue.Execute(copy_cmd_list.Submit());
        copy_queue.Sync(evt.timeline);
    }
    CommandList cmd_list{};
    auto        buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);

    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(copy_queue_buffer->GetView(), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));
    auto copy_queue_timeline = copy_queue.GetFenceHandle();
    gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, 0));
    gfx_queue.Sync();

    TextureRef vbuffer = device.CreateTexture(
        "vbuffer",
        Extent2D(resolution.x, resolution.y),
        PF_R32_UINT,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef normal = device.CreateTexture(
        "normal",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef uv = device.CreateTexture(
        "uv",
        Extent2D(resolution.x, resolution.y),
        PF_R32G32_SFLOAT,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    DepthBufferRef depth = device.CreateDepthBuffer(
        "depth",
        Extent2D(resolution.x, resolution.y),
        PF_D32_SFLOAT_S8_UINT,
        1,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT);

    TextureRef position = device.CreateTexture(
        "position",
        Extent2D(resolution.x, resolution.y),
        PF_R32G32B32A32_SFLOAT,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef pbr_shading_output = device.CreateTexture(
        "pbr_shading_output",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef post_process_output = device.CreateTexture(
        "post_process_output",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef fxaa_output = device.CreateTexture(
        "fxaa_output",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    VertexStream vertex_stream;
    vertex_stream.EmplacePerVertex(
        {Moer::Render::VertexElement(PF_R32G32B32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32B32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32B32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32_SFLOAT)});
    GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                              vertex_stream,
                              {RHIColorAttachmentInfo::Preset(PF_R32_UINT),
                               RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM),
                               RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),
                               RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)},
                              RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
                              PF_D32_SFLOAT_S8_UINT);

    // auto raster_pipeline = manager
    //                            .Raster()
    //                            .Vertex("test/BasicVertex.hlsl")
    //                            .Pixel("test/BasicFrag.hlsl")
    //                            .Build<TestTrianglePipeline>(std::move(pso_info));

    auto raster_pipeline_constant_color = manager
                                              .Raster()
                                              .Vertex("test/BasicVertex.hlsl")
                                              .Pixel("test/BasicFragConstant.hlsl")
                                              .Build<TestTrianglePipelineConstColor>(std::move(pso_info));

    // FIXME: vertex_full_screen_stream and vertex_stream, these two variabels have not been used in the following code
    VertexStream vertex_full_screen_stream;
    vertex_stream.EmplacePerVertex(
        {Moer::Render::VertexElement(PF_R32G32B32_SFLOAT)});
    GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                          {},
                                          {RHIColorAttachmentInfo::Preset(pbr_shading_output->GetFormat())});

    auto pbr_pipeline = manager
                            .Raster()
                            .Vertex("test/PBRMaterialVertex.hlsl")
                            .Pixel("test/PBRMaterialFrag.hlsl")
                            .Build<MaterialShadingPipeline>(std::move(pso_full_screen_info));

    auto post_process_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(post_process_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/FxaaPrecompute.hlsl")
            .Build<PostProcessPipeline>(std::move(pso_full_screen_info));
    }();

    auto fxaa_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(fxaa_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/Fxaa.hlsl")
            .Build<FxaaPipeline>(std::move(pso_full_screen_info));
    }();// IILE(Immediately Invoked Lambda Expression), usually for complex varaible initialization and avoid naming conflicts

    struct Vertex {
        float3 pos;
        float2 uv;
    };
    Vertex vertices[] = {
        {{0.0f, -0.5f, 0.0f}, {0.5f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}},
    };
    uint    indices[] = {0, 1, 2};
    float4  color_red = {1, 1, 1, 1};
    Sampler sampler(SF_LINEAR, SAM_REPEAT);
    uint    instance_buffer_handle;
    // auto vertex_buffer = device.CreateBuffer<float>(3 * sizeof(Vertex) / sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
    // auto index_buffer  = device.CreateBuffer<uint>(3, EBufferUsageFlags::INDEX_BUFFER);
    // cmd_list.CopyFrom(std::span<byte>((byte*)vertices, sizeof(vertices)), vertex_buffer->GetView());
    // cmd_list.CopyFrom(std::span<byte>((byte*)indices, sizeof(indices)), index_buffer->GetView());
    TextureRef red_tex = device.CreateTexture(
        Extent2D(1, 1),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    byte red_data[4] = {byte(255), byte(0), byte(0), byte(255)};
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data, sizeof(red_data)), red_tex);
    uint bdls_tex_handle_red = bindless_array->AllocateTexture(red_tex, sampler);

    BufferRef  red_buffer      = device.CreateBuffer<float>(4, EBufferUsageFlags::UNORDERED_ACCESS);
    float4     red_data_float4 = {1, 0, 0, 1};
    BufferView red_buffer_view(red_buffer, 0, 4, 4);
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data_float4, sizeof(red_data_float4)), red_buffer->GetView());
    uint bdls_buffer_handle_red = bindless_array->AllocateBuffer(red_buffer_view);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    Resource::LoaderInterface::LoadSceneFromFileAsync(ConfigManager::GetInstance().GetScenePath(), &scene);
    auto&& scope_exit_reset_async_load_info = OnScopeExit([&] {
        Scene::ResetAsyncLoadInfo();
    });

    FenceRef timeline   = device.CreateFence();
    uint64   time       = 0;
    bool     first_load = true;

    // uint bdls_tex_handle_depth   = bindless_array->AllocateTexture(depth, sampler);
    uint bdls_tex_handle_vbuffer             = 0;
    uint bdls_tex_handle_normal              = 0;
    uint bdls_tex_handle_uv                  = 0;
    uint bdls_tex_handle_position            = 0;
    uint bdls_tex_handle_depth               = 0;
    uint bdls_tex_handle_pbr_shading_output  = 0;
    uint bdls_tex_handle_post_process_output = 0;
    uint bdls_tex_handle_fxaa_output         = 0;

    uint material_buffer_handle = 0;
    uint light_buffer_handle    = 0;
    uint lighting_data_handle   = 0;

    BufferRef lighting_buffer = device.CreateBuffer<byte>(1 * sizeof(LightingData), EBufferUsageFlags::UNORDERED_ACCESS);

    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        gui.BeginGUIFrame();
        {
            static bool show = true;
            ShowGUI(&show);
            ShowSceneColor(&b_show_scene_color);
            ImGui::ShowDemoWindow(&b_show_demo);
        }
        gui.EndGUIFrame();
        if (time > 2) {
            timeline->Wait(time - 2);
        }

        uint last_io_change_timeline = 0;
        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
            if (first_load) {
                instance_buffer_handle = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView());
                material_buffer_handle = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView());
                light_buffer_handle    = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::LightInfo)->GetView());
                lighting_data_handle   = bindless_array->AllocateBuffer(lighting_buffer->GetView());

                bdls_tex_handle_vbuffer             = bindless_array->AllocateTexture(vbuffer, sampler);
                bdls_tex_handle_normal              = bindless_array->AllocateTexture(normal, sampler);
                bdls_tex_handle_uv                  = bindless_array->AllocateTexture(uv, sampler);
                bdls_tex_handle_position            = bindless_array->AllocateTexture(position, sampler);
                bdls_tex_handle_depth               = bindless_array->AllocateTexture(depth->GetView(), sampler);
                bdls_tex_handle_pbr_shading_output  = bindless_array->AllocateTexture(pbr_shading_output, sampler);
                bdls_tex_handle_post_process_output = bindless_array->AllocateTexture(post_process_output, sampler);
                bdls_tex_handle_fxaa_output         = bindless_array->AllocateTexture(fxaa_output, sampler);

                Array<ImportTexture> sampled_textures;
                sampled_textures.reserve((scene.GetGpuScene().material_textures.size()));

                for (auto& [name, tex] : scene.GetGpuScene().material_textures) {
                    sampled_textures.emplace_back(ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE));
                }

                cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(sampled_textures));

                cmd_list.UpdateBindlessArray(bindless_array);
                last_io_change_timeline = copy_queue_timeline->GetValue();
                first_load              = false;

                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
            }

            auto camera_entity = scene.GetCameras()[0];
            auto camera        = CameraManager::Get().Get(camera_entity);
            camera->Tick();

            // GBuffer Pass

            auto                    vertex_buffer = scene.GetVertexBuffer();
            auto                    index_buffer  = scene.GetIndexBuffer();
            VertexBuffer            vb(vertex_buffer, 0);
            IndexBuffer             ib(index_buffer->GetView(), EIndexElementType::IET_UINT32);
            std::span<VertexBuffer> vb_span(&vb, 1);
            IndexBuffer             ib_span = ib;
            Array<SingleDrawParam>  draw_datas;
            // draw_datas.emplace_back(SingleDrawParam{uint(index_buffer->GetByteSize()/sizeof(uint)), 1, 0, 0, 0});

            uint instance_count = 0;
            for (auto entity : scene.GetEntities()) {
                auto& mesh = RenderableManager::Get().GetMeshInfo(entity);
                draw_datas.emplace_back(SingleDrawParam{mesh.index_count, 1, mesh.index_offset, mesh.vertex_offset, instance_count++});
            }

            TestBindlessParam param;
            param.color                  = color_red;
            param.texture_handle         = bdls_tex_handle_red;
            param.buffer_handle          = bdls_buffer_handle_red;
            param.instance_buffer_handle = instance_buffer_handle;
            param.camera_view_proj       = camera->GetProjectionMatrix() * camera->GetViewMatrix();
            cmd_list.Gfx(raster_pipeline_constant_color, sampler, red_tex, bindless_array, param)
                .Draw(Rect2D(0, 0, resolution.x, resolution.y), vb_span, ib, std::move(draw_datas), DepthAttachment(depth->GetView().GetTexture()), ColorAttachment(vbuffer), ColorAttachment(normal), ColorAttachment(uv), ColorAttachment(position));

            // PBR Pass

            MaterialPassBindlessParam material_param;
            // material_param.material_buffer = bdls_buffer_handle_red;
            material_param.g_buffer_uv         = bdls_tex_handle_uv;
            material_param.g_buffer_normal     = bdls_tex_handle_normal;
            material_param.v_buffer            = bdls_tex_handle_vbuffer;
            material_param.g_buffer_depth      = bdls_tex_handle_depth;
            material_param.gbuffer_position    = bdls_tex_handle_position;
            material_param.global_param_handle = lighting_data_handle;
            material_param.light_buffer        = light_buffer_handle;

            LightingData lighting_data;
            lighting_data.inv_view_proj   = Inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());
            lighting_data.light_count     = scene.GetLights().size();
            lighting_data.camera_position = camera->GetPosition();
            cmd_list.CopyFrom(std::span<byte>((byte*)&lighting_data, sizeof(lighting_data)), lighting_buffer->GetView());

            Moer::UnorderedSet<EMaterialType>                                   material_types = {EMaterialType::E_PBR_STANDARD};
            Moer::UnorderedMap<EMaterialType, Moer::Array<MaterialInstanceRef>> material_instances;
            material_param.material_buffer = material_buffer_handle;
            for (auto type : material_types) {
                Array<SingleDrawParam> full_screen_draw_datas;
                full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
                material_param.material_type = uint(type);
                cmd_list.Gfx(pbr_pipeline, bindless_array, material_param)
                    .Draw("Lighting Pass", Rect2D(0, 0, resolution.x, resolution.y), std::move(full_screen_draw_datas), ColorAttachment(pbr_shading_output));
            };

            // Post process Pass (only for FXAA Precompute now)
            {
                // draw data
                Array<SingleDrawParam> full_screen_draw_datas;
                full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});

                // param
                PostProcessPipelineBindlessParam param;
                param.input_image = bdls_tex_handle_pbr_shading_output;

                // command
                cmd_list
                    .Gfx(post_process_pipeline, bindless_array, param)
                    .Draw("FXAA Precompute Pass", Rect2D(0, 0, resolution.x, resolution.y), std::move(full_screen_draw_datas), ColorAttachment(post_process_output));
            }

            /**
             * FXAA Pass
             * 
             * Press M to switch FXAA mode:
             * 0: FXAA Off                ：645+-fps
             * 1: FXAA Quality(Simplified)：630+-fps
             * 2: FXAA Quality            ：610+-fps [Default]
             * 
             * TODO: Move the control (input) code to another place (next 7-12 lines)
             */
            {
                // draw data
                Array<SingleDrawParam> full_screen_draw_datas;
                full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});

                // input (this part code should be refactored, move to another place)
                static uint8_t fxaa_mode = 2;
                // 0: off; 1: fxaa quality(simplified); 2: fxaa quality;
                if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                    fxaa_mode = (fxaa_mode + 1) % 3;
                }

                // param
                FxaaPipelineBindlessParam param;
                param.input_image    = bdls_tex_handle_post_process_output;
                param.fxaa_mode      = fxaa_mode;
                param.resolution     = float2(resolution);
                param.inv_resolution = float2(1.0) / float2(resolution);

                // command
                cmd_list
                    .Gfx(fxaa_pipeline, bindless_array, param)
                    .Draw("FXAA Pass", Rect2D(0, 0, resolution.x, resolution.y), std::move(full_screen_draw_datas), ColorAttachment(fxaa_output));
            }
        }

        auto output = fxaa_output;// actual output (must be R8G8B8A8_SRGB format)

        int w_width, w_height;

        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {

            resolution = {uint32(w_width), uint32(w_height)};
            vbuffer    = device.CreateTexture(
                Extent2D(resolution.x, resolution.y),
                PF_R32_UINT,
                ETextureUsageFlags::COLOR_ATTACHMENT);
            output->SetName("output");
            gfx_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);
            bindless_array->FreeTexture(bdls_tex_handle_vbuffer);
            bindless_array->FreeTexture(bdls_tex_handle_normal);
            bindless_array->FreeTexture(bdls_tex_handle_uv);
            bindless_array->FreeTexture(bdls_tex_handle_position);
            bindless_array->FreeTexture(bdls_tex_handle_depth);

            normal = device.CreateTexture(
                "normal",
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
            uv = device.CreateTexture(
                "uv",
                Extent2D(resolution.x, resolution.y),
                PF_R32G32_SFLOAT,
                ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

            position = device.CreateTexture(
                "position",
                Extent2D(resolution.x, resolution.y),
                PF_R32G32B32A32_SFLOAT,
                ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

            depth = device.CreateDepthBuffer(
                "depth",
                Extent2D(resolution.x, resolution.y),
                PF_D32_SFLOAT_S8_UINT,
                1,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT);

            bdls_tex_handle_vbuffer  = bindless_array->AllocateTexture(vbuffer, sampler);
            bdls_tex_handle_normal   = bindless_array->AllocateTexture(normal, sampler);
            bdls_tex_handle_uv       = bindless_array->AllocateTexture(uv, sampler);
            bdls_tex_handle_position = bindless_array->AllocateTexture(position, sampler);

            Sampler depth_sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE);

            bdls_tex_handle_depth = bindless_array->AllocateTexture(depth->GetView(), depth_sampler);
        }

        // cmd_list.Gfx(raster_pipeline, red_buffer)
        //     .Draw(Rect2D(0, 0, 1, 1), vb_span, ib, std::move(draw_datas), ColorAttachment(red_tex));

        // //float color with time sine
        // color_red[0] = 0.5f + 0.5f * sinf(time * 0.1f);
        // color_red[2] = 0.5f + 0.5f * cosf(time * 0.1f);
        // cmd_list.CopyFrom(std::span<byte>((byte*)&color_red, sizeof(color_red)), red_buffer_view);

        gui.RenderGUI(cmd_list, output);

        // cmd_list.Barriers(ReadTexture(red_tex, ETextureState::SAMPLE));
        time++;
        /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
         */
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).Wait(copy_queue_timeline, 0));
        gfx_queue.Present(sc, output);
    }
    gfx_queue.Sync();
}