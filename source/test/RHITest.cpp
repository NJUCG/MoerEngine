#include <filesystem>
#include <vcruntime_string.h>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "math/Matrix.h"
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
#include "renderer/UIRenderer.h"

using namespace Moer::Render;
using namespace Moer;

static bool b_show_demo        = true;
static bool b_show_scene_color = true;
class TestTrianglePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipeline);
    DEFINE_SHADER_BUFFER(buffer);
    DEFINE_SHADER_ARGS(buffer);
};

struct TestBindlessParam {
    float4 color;
    uint   texture_handle;
    uint   buffer_handle;
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

    auto&&                   scope_exit = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });
    Moer::Render::UIRenderer gui(device);

    auto* window_handle = WindowContext::GetMainWindow();

    auto                buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    SwapchainCreateInfo sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB};
    auto                sc             = device.CreateSwapchain(sc_info);
    BindlessArrayRef    bindless_array = device.CreateBindlessArray();
    auto&               cmd_queue      = device.GetCommandQueue(EQueueType::Graphics);
    auto&               copy_queue     = device.GetCommandQueue(EQueueType::Copy);

    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }

    FenceRef  copy_timeline     = device.CreateFence();
    BufferRef copy_queue_buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    {
        CommandList copy_cmd_list;
        copy_cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), copy_queue_buffer->GetView());
        copy_queue.Execute(copy_cmd_list.Submit().Signal(copy_timeline, 1));
    }
    CommandList cmd_list{};
    auto        buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);

    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(copy_queue_buffer->GetView(), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));
    cmd_queue.Execute(cmd_list.Submit().Wait(copy_timeline, 1));
    cmd_queue.Sync();

    ubyte*   pixels;
    int      width, height;
    uint     alignment = 4;
    ImGuiIO& io        = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    uint32_t   upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
    uint32_t   upload_size  = height * upload_pitch;
    TextureRef font_tex     = device.CreateTexture(
        Extent2D(width, height),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);
    output->SetName("output");

    TextureRef output2 = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);

    cmd_list.CopyFrom(
        std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    VertexStream vertex_stream;
    vertex_stream.EmplacePerVertex(
        {Moer::Render::VertexElement(PF_R32G32B32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32_SFLOAT)});
    GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                              vertex_stream,
                              {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)},
                              RHIDepthStencilStateInfo::Preset());

    auto raster_pipeline = manager
                               .Raster()
                               .Vertex("test/BasicVertex.hlsl")
                               .Pixel("test/BasicFrag.hlsl")
                               .Build<TestTrianglePipeline>(std::move(pso_info));

    auto raster_pipeline_constant_color = manager
                                              .Raster()
                                              .Vertex("test/BasicVertex.hlsl")
                                              .Pixel("test/BasicFragConstant.hlsl")
                                              .Build<TestTrianglePipelineConstColor>(std::move(pso_info));
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
    uint    bdls_tex_handle = bindless_array->AllocateTexture(font_tex, sampler);

    auto vertex_buffer = device.CreateBuffer<float>(3 * sizeof(Vertex) / sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
    auto index_buffer  = device.CreateBuffer<uint>(3, EBufferUsageFlags::INDEX_BUFFER);
    cmd_list.CopyFrom(std::span<byte>((byte*)vertices, sizeof(vertices)), vertex_buffer->GetView());
    cmd_list.CopyFrom(std::span<byte>((byte*)indices, sizeof(indices)), index_buffer->GetView());
    TextureRef red_tex = device.CreateTexture(
        Extent2D(1, 1),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    byte red_data[4] = {byte(255), byte(0), byte(0), byte(255)};
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data, sizeof(red_data)), red_tex);
    uint bdls_tex_handle_red = bindless_array->AllocateTexture(red_tex, sampler);

    BufferRef  red_buffer = device.CreateBuffer<float>(4, EBufferUsageFlags::UNORDERED_ACCESS);
    BufferView red_buffer_view(red_buffer, 0, 4, 4);
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data, sizeof(red_data)), red_buffer->GetView());
    uint bdls_buffer_handle_red = bindless_array->AllocateBuffer(red_buffer_view);

    cmd_list.UpdateBindlessArray(bindless_array);
    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    VertexBuffer vb(vertex_buffer, 0);
    IndexBuffer  ib(index_buffer->GetView(), EIndexElementType::IET_UINT32);

    FenceRef timeline = device.CreateFence();
    uint64   time     = 0;

    TextureRef scene_color = device.CreateTexture(
        Extent2D(scene_color_resolution.x, scene_color_resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
    scene_color->SetName("scene_color");
    float2 temp_scene_color_size = {scene_color_resolution.x, scene_color_resolution.y};

    TextureRef gui_color = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
    gui_color->SetName("gui_color");

    GfxPsoCreateInfo combine(RHIRasterizeInfo::Preset<Rast::CULL_BACK, FrontFace::CCW>(),
                             {},
                             {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_SRGB)},
                             RHIDepthStencilStateInfo::Preset());
    auto             combine_gui_pipeline = manager.Raster()
                                    .Vertex("CombineGuiVert.hlsl")
                                    .Pixel("CombineGuiFrag.hlsl")
                                    .Build<CombineUIPipeline>(std::move(combine));

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

        int w_width, w_height;

        std::span<VertexBuffer> vb_span(&vb, 1);
        IndexBuffer             ib_span = ib;

        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {

            resolution = {uint32(w_width), uint32(w_height)};
            output     = device.CreateTexture(
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT);
            output->SetName("output");
            cmd_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);

            gui_color = device.CreateTexture(
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
            gui_color->SetName("gui_color");
        }

        if (temp_scene_color_size.x != scene_color_resolution.x || temp_scene_color_size.y != scene_color_resolution.y) {
            scene_color = device.CreateTexture(
                Extent2D(scene_color_resolution.x, scene_color_resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
            temp_scene_color_size = {scene_color_resolution.x, scene_color_resolution.y};
            scene_color->SetName("scene_color");
        }
        {
            Array<SingleDrawParam> draw_datas;
            draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});

            cmd_list.Gfx(raster_pipeline, red_buffer)
                .Draw("Draw Red Tex", Rect2D(0, 0, 1, 1), vb_span, ib, std::move(draw_datas), ColorAttachment(red_tex));
        }
        // //float color with time sine
        // color_red[0] = 0.5f + 0.5f * sinf(time * 0.1f);
        // color_red[2] = 0.5f + 0.5f * cosf(time * 0.1f);
        // cmd_list.CopyFrom(std::span<byte>((byte*)&color_red, sizeof(color_red)), red_buffer_view);
        {
            Array<SingleDrawParam> draw_datas2;
            draw_datas2.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
            TestBindlessParam param;
            param.color          = color_red;
            param.texture_handle = bdls_tex_handle_red;
            param.buffer_handle  = bdls_buffer_handle_red;
            cmd_list.Gfx(raster_pipeline_constant_color, sampler, red_tex, bindless_array, param)
                .Draw("Scene Color", Rect2D(0, 0, scene_color_resolution.x, scene_color_resolution.y), vb_span, ib, std::move(draw_datas2), ColorAttachment(scene_color));
        }
        TextureView gui_color_view(gui_color);
        gui.RenderGUI(cmd_list, gui_color_view);

        {
            Array<MeshDrawData> draw(1);
            draw.back().EmplaceDraw(3, 0, 0);
            CombineUIPipeline::Param param;
            param.min_xy = scene_color_pos / resolution;
            param.max_xy = (scene_color_pos + scene_color_resolution) / resolution;

            cmd_list.Gfx(combine_gui_pipeline, scene_color, gui_color_view, sampler, param)
                .Draw("Combine UI", Rect2D(0, 0, resolution.x, resolution.y), std::move(draw), ColorAttachment(output));
        }

        // cmd_list.Barriers(ReadTexture(red_tex, ETextureState::SAMPLE));
        time++;
        cmd_queue.Execute(cmd_list.Submit().Signal(timeline, time));
        cmd_queue.Present(sc, output);
    }
    cmd_queue.Sync();
}