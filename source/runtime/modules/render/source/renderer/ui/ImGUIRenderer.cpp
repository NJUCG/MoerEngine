#include "ImGUIRenderer.h"
#include "GLFW/glfw3.h"
#include "IconsFontAwesome6.h"
#include "PixelFormat.h"
#include "RenderThread.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"

#include "math/Math.h"
#include "misc/STL.h"

#include "window/WindowContext.h"

#include <atomic>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>

#include <backends/imgui_impl_glfw.h>

using namespace Moer::Render;
using namespace Moer;
struct UIFrameData {

    RHIGraphicsCommandList* ui_command_list = nullptr;
    RHICommandQueue*        command_queue   = nullptr;
    RHIFenceRef             present_fence   = nullptr;

    uint64_t timeline_index = 0;

    ~UIFrameData() {
        MoerDelete(ui_command_list);
        MoerDelete(command_queue);
    }
};

void GuiInitPlatformInterface();
void GUIRender(void* _draw_data, const TextureView& _view, CommandList&);
void GuiRenderWindow(ImGuiViewport* viewport, void*);
void GuiSwapbuffer(ImGuiViewport* viewport, void*);
struct GuiFrameRenderBuffers {

    Moer::Render::BufferRef vtx_buffer;
    Moer::Render::BufferRef idx_buffer;
    Moer::Render::BufferRef arg_buffer;
};
struct GuiViewportData {
    Moer::Render::FenceRef     fence;
    Moer::Render::FenceRef     copy_fence;
    Moer::Render::SwapchainRef sc;

    Moer::Array<GuiFrameRenderBuffers> render_buffers;// Used by all viewports
    Moer::Render::TextureRef           framebuffer;
    RHIViewportNextBackBufferInfo      next_frame_info;

    uint64_t        frame_index;
    uint32_t        viewport_index;
    static uint32_t viewport_count;
    bool            IsBackBufferReady() {
        return next_frame_info.backbuffer_index != UINT32_MAX;
    }

    GuiViewportData(uint32_t _frame_in_flight) {
        memset((void*)this, 0, sizeof(*this));
        render_buffers.resize(_frame_in_flight);
        for (uint32_t i = 0; i < _frame_in_flight; ++i) {
            render_buffers[i].vtx_buffer = nullptr;
            render_buffers[i].idx_buffer = nullptr;
            render_buffers[i].arg_buffer = nullptr;
        }
        viewport_index = viewport_count;
        viewport_count++;
    }
    ~GuiViewportData() {
        viewport_count--;
    }
};
namespace Moer::Render {

    struct ImGUIArg {
        ImVec2 min_xy;
        ImVec2 max_xy;
        uint   image_handle;
        uint   padding;
        uint   padding2;
        uint   padding3;
    };

    enum class EFontType {
        Greek,
        Chinese,
        Korean,
        Japanese,
        Cyrillic,
        Thai,
        Vietnamese,
        Icon,
        Default
    };

    struct RENDER_API FontDesc {
        FontDesc(const char* _font_path, float _font_size, EFontType _font_type)
            : font_path(_font_path), font_size(_font_size), font_type(_font_type) {}
        std::string font_path;
        float       font_size = 13.f;
        EFontType   font_type;
    };
    class GUIPipeline : public RasterPipeline {
    public:
        struct Constant {
            Matrix4x4f mvp;
        };
        DEFINE_RASTER_PIPELINE_CLASS(GUIPipeline)

        DEFINE_SHADER_SAMPLER(sampler0);
        DEFINE_SHADER_TEX(texture0);
        DEFINE_SHADER_CONSTANT_STRUCT(Constant, constant);

        DEFINE_SHADER_ARGS(constant);
    };

    class GUIPipelineBdls : public RasterPipeline {
    public:
        struct Constant {
            Matrix4x4f mvp;
        };
        DEFINE_RASTER_PIPELINE_CLASS(GUIPipelineBdls)

        DEFINE_SHADER_BINDLESS_ARRAY(bdls);
        DEFINE_SHADER_BUFFER(arg_buffer);
        DEFINE_SHADER_CONSTANT_STRUCT(Constant, param);

        DEFINE_SHADER_ARGS(arg_buffer, bdls, param);
    };

    struct ImGUIData {
        size_t buffer_memory_alignment;

        GUIPipelineBdls rast_pso;

        TextureRef font_texture;
        BufferRef  upload_buffer;

        // Render buffers for main window
        Array<GuiFrameRenderBuffers> render_buffers;
        SwapchainRef                 sc;
        TextureRef                   framebuffer;
        uint32_t                     num_frames_in_flight;

        ImGUIRenderBackend* render_backend = nullptr;
    };
    const ImWchar* FontTypeToRange(EFontType _font_range_type) {
        using namespace Moer;
        static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

        switch (_font_range_type) {
            case EFontType::Greek:
                return ImGui::GetIO().Fonts->GetGlyphRangesGreek();
            case EFontType::Chinese:
                return ImGui::GetIO().Fonts->GetGlyphRangesChineseFull();
            case EFontType::Korean:
                return ImGui::GetIO().Fonts->GetGlyphRangesKorean();
            case EFontType::Japanese:
                return ImGui::GetIO().Fonts->GetGlyphRangesJapanese();
            case EFontType::Cyrillic:
                return ImGui::GetIO().Fonts->GetGlyphRangesCyrillic();
            case EFontType::Thai:
                return ImGui::GetIO().Fonts->GetGlyphRangesThai();
            case EFontType::Vietnamese:
                return ImGui::GetIO().Fonts->GetGlyphRangesVietnamese();
            case EFontType::Default:
                return ImGui::GetIO().Fonts->GetGlyphRangesDefault();
            case EFontType::Icon:
                return icons_ranges;
            default:
                break;
        }
        return ImGui::GetIO().Fonts->GetGlyphRangesDefault();
    }
    static void AddFont(FontDesc _desc) {
        const auto font_base_path = Moer::ConfigManager::GetInstance().GetEditorResourcePath() / FONTS_DIR;
        const auto font_path      = font_base_path / _desc.font_path;

        auto& io = ImGui::GetIO();

        const ImWchar* font_range = FontTypeToRange(_desc.font_type);
        ImFontConfig   icons_config;
        icons_config.MergeMode            = false;
        icons_config.PixelSnapH           = true;
        icons_config.FontDataOwnedByAtlas = false;
        if (_desc.font_type == EFontType::Icon) {
            float icon_font_size = _desc.font_size * 2.0f / 3.0f;// FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

            icons_config.MergeMode        = true;
            icons_config.GlyphMinAdvanceX = icon_font_size;

            io.Fonts->AddFontFromFileTTF(font_path.generic_string().data(), icon_font_size, &icons_config, font_range);
        } else {
            io.FontDefault = io.Fonts->AddFontFromFileTTF(font_path.generic_string().data(), _desc.font_size, &icons_config, font_range);
        }
    }
    static void* MallocWrapper(size_t size, void* user_data) {
        return Memory::Malloc(size);
    }
    static void FreeWrapper(void* ptr, void* user_data) {
        Memory::Free(ptr);
    }
    ImGUIRenderBackend::ImGUIRenderBackend(RenderDevice& _device) : device(_device) {

        ImGui::SetAllocatorFunctions(MallocWrapper, FreeWrapper, nullptr);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

            GLFWwindow* window = (GLFWwindow*)WindowContext::GetMainWindow()->window;
            switch (device.GetRHIType()) {
                case ERHIType::Vulkan: {
                    ImGui_ImplGlfw_InitForVulkan(window, true);
                    break;
                }
                default:
                    ImGui_ImplGlfw_InitForOther(window, true);
            }
            {

                io.Fonts->AddFontDefault();
                AddFont({FONT_ICON_FILE_NAME_FAS,
                         13.0f,
                         EFontType::Icon});
                AddFont({"msyh.ttc",
                         20.0f,
                         EFontType::Chinese});
            }
        }
        bindless_array = device.CreateBindlessArray();

        ImGuiIO& io = ImGui::GetIO();
        assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

        const Moer::ConfigManager& config_manager      = Moer::ConfigManager::GetInstance();
        uint32_t                   max_frame_in_flight = config_manager.GetInitConfig().max_frame_in_flight;

        ImGUIData* render_backend_data            = MoerNew(ImGUIData)();
        render_backend_data->num_frames_in_flight = max_frame_in_flight;

        io.BackendRendererUserData = render_backend_data;
        io.BackendRendererName     = "Moer";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiViewport*   main_viewport      = ImGui::GetMainViewport();
        GuiViewportData* viewport_data      = MoerNew(GuiViewportData)(max_frame_in_flight);
        render_backend_data->render_backend = this;
        main_viewport->RendererUserData     = viewport_data;

        auto& sd_mgr = ShaderManager::Get();
        using namespace Moer::Render;
        VertexStream vertex_stream;
        vertex_stream.EmplacePerVertex(
            {Moer::Render::VertexElement(PF_R32G32_SFLOAT),
             Moer::Render::VertexElement(PF_R32G32_SFLOAT),
             Moer::Render::VertexElement(PF_R8G8B8A8_UNORM)});
        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset<Rast::CULL_BACK, FrontFace::CW>(),
            vertex_stream,
            {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(PF_R8G8B8A8_SRGB)},
            RHIDepthStencilStateInfo::Preset());
        render_backend_data->rast_pso = std::move(
            sd_mgr
                .Raster()
                .Vertex("GuiVert.hlsl")
                .Pixel("GuiFrag.hlsl")
                .Build<GUIPipelineBdls>(std::move(pso_info)));

        uint8_t* pixels;

        int width, height;
        //MARK... this is freaking slow, it's build first called, we need a default data for it, and async load other fonts
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        using namespace Moer::Render;
        auto& rd_device = Moer::Render::RenderDevice::Get();
        //upload texture
        {
            const uint32_t alignment    = 256;
            uint32_t       upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
            uint32_t       upload_size  = height * upload_pitch;
            TextureRef     font_tex     = rd_device.CreateTexture(
                Extent2D(width, height),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::SAMPLED);

            CommandList cmd_list;
            cmd_list.CopyFrom(
                std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

            rd_device.GetCommandQueue(EQueueType::Graphics).Execute(std::move(cmd_list.Submit()));
            rd_device.GetCommandQueue(EQueueType::Graphics).Sync();
            render_backend_data->font_texture = font_tex;
            uint handle                       = bindless_array->AllocateTexture(font_tex, Sampler(SF_LINEAR, SAM_REPEAT));
            registered_images.try_emplace(font_tex, handle);
            io.Fonts->SetTexID(handle);
        }

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            GuiInitPlatformInterface();
    };
    inline ImGUIData* GetGUIBackendData() {
        return ImGui::GetCurrentContext() ? (ImGUIData*)ImGui::GetIO().BackendRendererUserData : nullptr;
    }
    ImGUIRenderBackend::~ImGUIRenderBackend() {
        bindless_array  = nullptr;
        ImGUIData* data = GetGUIBackendData();
        MoerDelete(data);
        ImGui::GetIO().BackendRendererUserData = nullptr;

        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    void ImGUIRenderBackend::BeginGUIFrame() {
        ImGui_ImplGlfw_NewFrame();

        ImGui::NewFrame();
    }

    void ImGUIRenderBackend::EndGUIFrame() {
        ImGui::Render();
    }

    void ImGUIRenderBackend::RegisterImage(Texture* _texture, Sampler _sampler) {
        auto iter = registered_images.try_emplace(_texture, 0);
        if (iter.second) {
            uint handle        = bindless_array->AllocateTexture(_texture->GetView(0, _texture->GetNumMips()), _sampler);
            iter.first->second = handle;
        }
    }

    void ImGUIRenderBackend::UnRegisterImage(Texture* _texture) {
    }

    void ImGUIRenderBackend::RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
        ImGuiIO& io = ImGui::GetIO();
        _cmd_list.UpdateBindlessArray(bindless_array);

        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;

        ImDrawData* draw_data = ImGui::GetDrawData();
        if (!draw_data)
            return;

        ImDrawData* main_draw_data     = ImGui::GetDrawData();
        const bool  b_window_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

        if (!b_window_minimized) {
            auto& rd_device = RenderDevice::Get();
            GUIRender(main_draw_data, _framebuffer, _cmd_list);
        }
        {
            auto& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
            }
            ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

            if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
                for (int i = 1; i < platform_io.Viewports.Size; i++)
                    if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                        GuiRenderWindow(platform_io.Viewports[i], nullptr);
                for (int i = 1; i < platform_io.Viewports.Size; i++)
                    if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                        GuiSwapbuffer(platform_io.Viewports[i], nullptr);
            }
        }
    }

}// namespace Moer::Render
struct GuiBackendData {
    size_t buffer_memory_alignment;

    RHIGfxPsoRef              pipeline;
    Moer::Render::GUIPipeline rast_pso;
    RHIShaderRef              shader_module_vert;
    RHIShaderRef              shader_module_frag;

    // Font data
    RHISamplerRef            font_sampler;
    RHITextureRef            font_texture;
    Moer::Render::TextureRef font_tex;
    RHISRVRef                font_view;
    RHIBufferRef             upload_buffer;

    // Render buffers for main window
    GuiFrameRenderBuffers* main_viewport_render_buffers;
    RHIViewport*           main_viewport;
    TextureRef             framebuffer;

    EPixelFormat attachment_format;
    uint32_t     num_frames_in_flight;

    GuiBackendData() {
        memset((void*)this, 0, sizeof(*this));
        buffer_memory_alignment = 256;
    }
    ~GuiBackendData();
};

class ImGuiShaderVert : public Shader {
    DEFINE_SHADER_TYPE(ImGuiShaderVert, Global, RENDER_API, ...)
public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UIVertex)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, mvp)
    DEFINE_SHADER_PARAM(bool, need_correction)
    END_SHADER_CONSTANT_STRUCT_DEFINITION()
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    // DEFINE_SHADER_PARAM_CBV(Constant, vertexBuffer)
    DEFINE_SHADER_PARAM_STRUCT(UIVertex, vertexBuffer)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
IMPLEMENT_SHADER_TYPE(ImGuiShaderVert, "GuiVert.hlsl", "main", ST_VERTEX)
class ImGuiShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(ImGuiShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UIVertex)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, mvp)
    DEFINE_SHADER_PARAM(bool, need_correction)
    END_SHADER_CONSTANT_STRUCT_DEFINITION()
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(UIVertex, vertexBuffer)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, sampler0)
    DEFINE_SHADER_PARAM_SRV(Texture2D, texture0)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(ImGuiShaderFrag, "GuiFrag.hlsl", "main", ST_FRAGMENT)

class ImGUIRenderer::Impl {
public:
    Impl() {
    }
    ~Impl() {
    }

    void Init();
    void ShutDown();

    void BeginRenderFrame();
    void EndRenderFrame();

private:
    RHIGraphicsCommandList* ui_command_list = nullptr;
    RHICommandQueue*        command_queue   = nullptr;
    RHIFenceRef             present_fence   = nullptr;

    uint64_t timeline_index = 0;

    //render_thread
    RHIViewportNextBackBufferInfo next_frame_info;
    Moer::Render::BackBufferInfo  back_buffer_info;

    void UpdateGUIData();
};
bool CreateDeviceObjects();
void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);
void InvalidateDeviceObjects();

void GUIUploadData(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);
void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);
void GUIUploadData(void* _draw_data, CommandList&);

inline GuiBackendData* GetBackendData() {
    return ImGui::GetCurrentContext() ? (GuiBackendData*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

void ImGUIRenderer::Init() {
    impl = MoerNew(Impl)();
    impl->Init();
}

void ImGUIRenderer::ShutDown() {
    impl->ShutDown();
    MoerDelete(impl);
}

void ImGUIRenderer::BeginRenderFrame() {
    impl->BeginRenderFrame();
}
//collect render data and record
void ImGUIRenderer::EndRenderFrame() {
    impl->EndRenderFrame();
}

void ImGUIRenderer::Impl::Init() {
    present_fence   = g_rhi->RHICreateFence(RHIFenceCreateInfo(EFenceUsageFlags::PRESENT));
    ui_command_list = g_rhi->RHICreateGraphicsCommandList();
    command_queue   = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

    ImGuiIO& io = ImGui::GetIO();
    assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

    const Moer::ConfigManager& config_manager      = Moer::ConfigManager::GetInstance();
    uint32_t                   max_frame_in_flight = config_manager.GetInitConfig().max_frame_in_flight;

    GuiBackendData* render_backend_data       = MoerNew(GuiBackendData)();
    render_backend_data->num_frames_in_flight = max_frame_in_flight;

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiViewport*   main_viewport  = ImGui::GetMainViewport();
    GuiViewportData* viewport_data  = MoerNew(GuiViewportData)(max_frame_in_flight);
    main_viewport->RendererUserData = viewport_data;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        GuiInitPlatformInterface();
}

void ImGUIRenderer::Impl::BeginRenderFrame() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call GuiInit(uint32_t)?");

    if (!bd->pipeline)
        CreateDeviceObjects();
    ImGui::NewFrame();
}

void ImGUIRenderer::Impl::EndRenderFrame() {
    ImGui::Render();

    ImDrawData* main_draw_data     = ImGui::GetDrawData();
    const bool  b_window_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

    if (!b_window_minimized) {
        auto& rd_device = RenderDevice::Get();

        SwapchainCreateInfo swapchain_info{
            .window_handle    = (uintptr_t)Moer::WindowContext::GetMainWindow(),
            .size             = {1920u, 1080u},
            .back_buffer_sz   = 2,
            .preferred_format = PF_R8G8B8A8_SRGB};

        auto sc = rd_device.CreateSwapchain(swapchain_info);
        if (!sc) {
            sc = rd_device.CreateSwapchain(swapchain_info);
        }
        CommandList cmd_list{};

        auto        tex  = GetBackendData()->framebuffer;
        TextureView view = tex->GetView(0);
        // GUIUploadData(main_draw_data, ui_command_list, &next_frame_info);
        // GUIRender(main_draw_data, cmd_list);
        rd_device.GetCommandQueue(EQueueType::Graphics).Present(sc, view);
    }
    {
        auto& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
        }
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

        if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiRenderWindow(platform_io.Viewports[i], nullptr);
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiSwapbuffer(platform_io.Viewports[i], nullptr);
        }
    }
}

void ImGUIRenderer::Impl::ShutDown() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    // Clean up windows and device objects
    ImGui::DestroyPlatformWindows();
    InvalidateDeviceObjects();

    io.BackendRendererName     = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);
    MoerDelete(bd);

    EnqueueRenderTask([this] {
        command_queue->WaitForQueueComplete();
        MoerDelete(command_queue);
        MoerDelete(ui_command_list);
    });

    Moer::RenderThreadFence fence;
    fence.BeginFence();
    fence.Wait();

    // Manually delete main viewport render resources in-case we haven't initialized for viewports
    auto* main_viewport = ImGui::GetDrawData()->OwnerViewport;
    if (GuiViewportData* viewport_data = (GuiViewportData*)main_viewport->RendererUserData) {
        // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
        for (uint32_t i = 0; i < bd->num_frames_in_flight; i++)
            DestroyRenderBuffers(&viewport_data->render_buffers[i]);
        MoerDelete(viewport_data);
        main_viewport->RendererUserData = nullptr;
    }
}

uint32_t GuiViewportData::viewport_count = 0;

GuiBackendData::~GuiBackendData() {
}

bool CreateDeviceObjects();
void CreateFontsTexture();

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiViewportData* _viewport_data, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);

// void GUIUploadData(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
//     ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);
//     if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
//         return;
//     // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);
//     uint32_t total_size_vert = draw_data->TotalVtxCount;
//     uint32_t total_size_idx  = draw_data->TotalIdxCount * sizeof(ImDrawIdx);

//     GuiBackendData* backend_data = GetBackendData();

//     GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

//     uint32_t num_frames_in_flight = backend_data->num_frames_in_flight;
//     EnqueueRenderTask([_next_frame_info_render_thread, viewport_data, num_frames_in_flight, total_size_vert, total_size_idx] {
//         if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

//         GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];

//         if (render_buffers->vertex_buffer == nullptr || render_buffers->vertex_buffer->GetNumElement() < total_size_vert) {
//             //delete the old one and create new
//             if (render_buffers->vertex_buffer != nullptr) {}
//             // render_buffers->vertex_buffer->DeRef();
//             uint32_t new_size             = 4096 + total_size_vert;
//             render_buffers->vertex_buffer = g_rhi->RHICreateBuffer<ImDrawVert>(
//                 new_size * sizeof(ImDrawVert),
//                 EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);

//             render_buffers->staging_vertex_buffer = g_rhi->RHICreateBuffer<ImDrawVert>(
//                 new_size * sizeof(ImDrawVert), EBufferUsageFlags::CPU_VISIBLE);

//             render_buffers->staging_vertex_buffer->SetName("staging_vertex_buffer");
//         }
//         if (render_buffers->index_buffer == nullptr || render_buffers->index_buffer->GetNumElement() < total_size_idx) {

//             if (render_buffers->index_buffer != nullptr) {}
//             uint32_t new_size            = 8192 + total_size_idx;
//             render_buffers->index_buffer = g_rhi->RHICreateBuffer<ImDrawIdx>(
//                 new_size * sizeof(ImDrawIdx),
//                 EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);

//             render_buffers->staging_index_buffer = g_rhi->RHICreateBuffer<ImDrawIdx>(
//                 new_size * sizeof(ImDrawIdx), EBufferUsageFlags::CPU_VISIBLE);
//         }
//     });
//     GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];
//     auto&                  cmd_list       = viewport_data->cmd_list;

//     Moer::Render::CommandQueue* q;
//     auto&&                      submit = cmd_list.Submit();
//     q->Execute(std::move(submit));

//     ImDrawVert* vertex_dst = nullptr;
//     ImDrawIdx*  index_dst  = nullptr;

//     // RHIBufferRef staging_index_buffer  = render_buffers->staging_index_buffer;
//     // RHIBufferRef staging_vertex_buffer = render_buffers->staging_vertex_buffer;

//     // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(render_buffers->staging_vertex_buffer, 0, UINT64_MAX);
//     // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(render_buffers->staging_index_buffer, 0, UINT64_MAX);
//     // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
//     // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
//     size_t vertex_offset = 0;
//     size_t index_offset  = 0;
//     for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
//         const ImDrawList*       cmd_list = draw_data->CmdLists[n];
//         Moer::Array<ImDrawVert> vertices(cmd_list->VtxBuffer.Size);
//         Moer::Array<ImDrawIdx>  indices(cmd_list->IdxBuffer.Size);
//         memcpy(vertices.data(), cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
//         memcpy(indices.data(), cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
//         EnqueueRenderTask([_next_frame_info_render_thread,
//                            num_frames_in_flight,
//                            vertices{std::move(vertices)},
//                            indices{std::move(indices)},
//                            viewport_data,
//                            vertex_offset,
//                            index_offset]() {
//             if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//             GuiFrameRenderBuffers* render_buffers        = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];
//             RHIBufferRef           staging_vertex_buffer = render_buffers->staging_vertex_buffer;
//             RHIBufferRef           staging_index_buffer  = render_buffers->staging_index_buffer;

//             ImDrawVert* vertex_dst = nullptr;
//             ImDrawIdx*  index_dst  = nullptr;

//             vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
//             index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
//             // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
//             // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
//             memcpy(vertex_dst + vertex_offset, vertices.data(), vertices.size() * sizeof(ImDrawVert));
//             memcpy(index_dst + index_offset, indices.data(), indices.size() * sizeof(ImDrawIdx));
//             g_rhi->RHIUnmapBuffer(staging_vertex_buffer);
//             g_rhi->RHIUnmapBuffer(staging_index_buffer);
//         });

//         vertex_offset += cmd_list->VtxBuffer.Size;
//         index_offset += cmd_list->IdxBuffer.Size;
//     }
//     // g_rhi->RHIUnmapBuffer(render_buffers->staging_vertex_buffer);
//     // g_rhi->RHIUnmapBuffer(render_buffers->staging_index_buffer);

//     EnqueueRenderTask([_next_frame_info_render_thread,
//                        _ui_command_list,
//                        viewport_data,
//                        num_frames_in_flight] {
//         if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

//         GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];

//         RHIBufferRef staging_vertex_buffer = render_buffers->staging_vertex_buffer;
//         RHIBufferRef staging_index_buffer  = render_buffers->staging_index_buffer;
//         RHIBufferRef vertex_buffer         = render_buffers->vertex_buffer;
//         RHIBufferRef index_buffer          = render_buffers->index_buffer;

//         RHICopyBufferInfo copy_info{};
//         copy_info.regions.emplace_back(0,
//                                        0,
//                                        staging_vertex_buffer->GetByteSize());
//         RHICopyBufferInfo copy_index_info{};
//         copy_index_info.regions.emplace_back(0,
//                                              0,
//                                              staging_index_buffer->GetByteSize());

//         RHIBarrierDependencyInfo dependency_info{};
//         auto&                    buffer_barriers = dependency_info.buffer_barriers;
//         buffer_barriers.resize(2);
//         buffer_barriers[0]
//             .SetBuffer(vertex_buffer)
//             .SetOffset(0)
//             .SetSize(Moer::MAX_INT64)
//             .SetSrcAccessFlags(ERHIAccessFlags::VERTEX_ATTRIBUTE_READ)
//             .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//             .SetSrcStage(PS_VERTEX_INPUT)
//             .SetDstStage(PS_TRANSFER);

//         buffer_barriers[1]
//             .SetBuffer(index_buffer)
//             .SetOffset(0)
//             .SetSize(Moer::MAX_INT64)
//             .SetSrcAccessFlags(ERHIAccessFlags::INDEX_READ)
//             .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//             .SetSrcStage(PS_VERTEX_INPUT)
//             .SetDstStage(PS_TRANSFER);

//         _ui_command_list->SetPipelineBarrier(dependency_info);

//         _ui_command_list->CopyBuffer(copy_info, staging_vertex_buffer, vertex_buffer);
//         _ui_command_list->CopyBuffer(copy_index_info, staging_index_buffer, index_buffer);

//         buffer_barriers[0]
//             .SetBuffer(vertex_buffer)
//             .SetOffset(0)
//             .SetSize(Moer::MAX_INT64)
//             .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//             .SetDstAccessFlags(ERHIAccessFlags::VERTEX_ATTRIBUTE_READ)
//             .SetSrcStage(PS_TRANSFER)
//             .SetDstStage(PS_VERTEX_INPUT);

//         buffer_barriers[1]
//             .SetBuffer(index_buffer)
//             .SetOffset(0)
//             .SetSize(Moer::MAX_INT64)
//             .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//             .SetDstAccessFlags(ERHIAccessFlags::INDEX_READ)
//             .SetSrcStage(PS_TRANSFER)
//             .SetDstStage(PS_VERTEX_INPUT);

//         _ui_command_list->SetPipelineBarrier(dependency_info);
//     });
// }

void ImGUIRenderer::Impl::UpdateGUIData() {
}
// void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
//     ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);
//     // std::this_thread::sleep_for(std::chrono::milliseconds(10));

//     if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
//         return;
//     // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);

//     GuiBackendData* backend_data = GetBackendData();

//     GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

//     // GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % backend_data->num_frames_in_flight];
//     //todo frame_index should not manage here

//     ImVec2 clip_off   = draw_data->DisplayPos;      // (0,0) unless using multi-viewports
//     ImVec2 clip_scale = draw_data->FramebufferScale;// (1,1) unless using retina display which are often (2,2)
//     SetupRenderState(draw_data, _ui_command_list, viewport_data, _next_frame_info_render_thread);
//     int32_t global_vertex_offset = 0,
//             global_index_offset  = 0;
//     ImGuiShaderVert::Parameters vert_param;
//     {
//         float l         = draw_data->DisplayPos.x;
//         float r         = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
//         float t         = draw_data->DisplayPos.y;
//         float b         = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
//         float mvp[4][4] = {
//             {2.0f / (r - l), 0.0f, 0.0f, (r + l) / (l - r)},
//             {0.0f, 2.0f / (t - b), 0.5f, (t + b) / (b - t)},
//             {0.0f, 0.0f, 0.f, 0.5f},
//             {0.f, 0.0f, 0.0f, 1.0f},
//         };
//         memcpy(&vert_param.vertexBuffer.mvp, mvp, sizeof(mvp));
//     }
//     // ImVec2 clip_off = draw_data->DisplayPos;
//     for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
//         const ImDrawList* cmd_list = draw_data->CmdLists[n];
//         for (uint32_t cmd_index = 0; cmd_index < cmd_list->CmdBuffer.Size; ++cmd_index) {
//             const ImDrawCmd* cmd = &cmd_list->CmdBuffer[cmd_index];
//             if (cmd->UserCallback != nullptr) {
//                 if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
//                     SetupRenderState(draw_data, _ui_command_list, viewport_data, _next_frame_info_render_thread);
//                 } else {
//                     cmd->UserCallback(cmd_list, cmd);
//                 }
//             } else {
//                 // Project scissor/clipping rectangles into framebuffer space
//                 ImVec2 clip_min((cmd->ClipRect.x - clip_off.x) * clip_scale.x, (cmd->ClipRect.y - clip_off.y) * clip_scale.y);
//                 ImVec2 clip_max((cmd->ClipRect.z - clip_off.x) * clip_scale.x, (cmd->ClipRect.w - clip_off.y) * clip_scale.y);
//                 if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
//                     continue;

//                 Rect2D r = {
//                     (int32_t)clip_min.x,
//                     (int32_t)clip_min.y,
//                     (uint32_t)(clip_max.x - clip_min.x),
//                     uint32_t(clip_max.y - clip_min.y)};

//                 // 5. local: set scissor and viewport
//                 EnqueueRenderTask([_ui_command_list, r, _next_frame_info_render_thread] {
//                     if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//                     _ui_command_list->SetScissor(r);
//                     // _ui_command_list->SetViewPort(g_rhi->RHIGetMainViewport()->GetViewportExtent());
//                 });

//                 // _ui_command_list->SetScissor(r);
//                 // _ui_command_list->SetViewPort(g_rhi->RHIGetMainViewport()->GetViewportExtent());

//                 // 6. local: set texture
//                 RHISRVRef texture_view = (RHISRVRef)cmd->GetTexID();

//                 ImGuiShaderFrag::Parameters params;
//                 params.texture0 = texture_view;
//                 if (texture_view.Get() == backend_data->font_view.Get()) {
//                     vert_param.vertexBuffer.need_correction = false;
//                 } else {
//                     vert_param.vertexBuffer.need_correction = true;
//                 }
//                 RHIBatchedShaderParameters batched_params;

//                 batched_params.SetParameters(backend_data->shader_module_frag, params, false);
//                 batched_params.SetParameters(backend_data->shader_module_vert, vert_param);

//                 RHIGfxPsoRef pipeline = backend_data->pipeline;

//                 uint32_t elem_count = cmd->ElemCount;
//                 uint32_t vtx_offset = cmd->VtxOffset + global_vertex_offset;
//                 uint32_t idx_offset = cmd->IdxOffset + global_index_offset;

//                 EnqueueRenderTask([_ui_command_list,
//                                    batched_params,
//                                    pipeline,
//                                    elem_count,
//                                    vtx_offset,
//                                    idx_offset,
//                                    _next_frame_info_render_thread] {
//                     if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//                     g_rhi->RHISetBatchedShaderParameters(pipeline, batched_params);

//                     _ui_command_list->DrawIndexedInstanced(elem_count, 1, idx_offset, vtx_offset, 0);
//                 });

//                 // g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

//                 // 7. local: draw indexed instanced
//                 // _ui_command_list->DrawIndexedInstanced(cmd->ElemCount, 1, cmd->IdxOffset + global_index_offset, cmd->VtxOffset + global_vertex_offset, 0);
//             }
//         }
//         global_index_offset += cmd_list->IdxBuffer.Size;
//         global_vertex_offset += cmd_list->VtxBuffer.Size;
//     }

//     EnqueueRenderTask([viewport_data, _next_frame_info_render_thread] {
//         if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//         viewport_data->frame_index += 1;
//     });
// }

void GUIRender(void* _draw_data, const TextureView& _frame_buffer, CommandList& _cmdlist) {
    ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);

    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);
    uint32_t total_size_vert = draw_data->TotalVtxCount;
    uint32_t total_size_idx  = draw_data->TotalIdxCount * sizeof(ImDrawIdx);

    ImGUIData&          backend_data   = *GetGUIBackendData();
    ImGUIRenderBackend& render_backend = *backend_data.render_backend;
    auto&               device         = RenderDevice::Get();

    GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

    uint32_t num_frames_in_flight = backend_data.num_frames_in_flight;

    GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];
    viewport_data->frame_index++;

    if (render_buffers->vtx_buffer == nullptr || render_buffers->vtx_buffer->GetNumElement() < total_size_vert) {
        //delete the old one and create new
        if (render_buffers->vtx_buffer != nullptr) {}
        // render_buffers->vertex_buffer->DeRef();
        uint32_t new_size          = 4096 + total_size_vert;
        render_buffers->vtx_buffer = device.CreateBuffer<ImDrawVert>(new_size, EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);
    }
    if (render_buffers->idx_buffer == nullptr || render_buffers->idx_buffer->GetNumElement() < total_size_idx) {

        if (render_buffers->idx_buffer != nullptr) {}
        uint32_t new_size          = 8192 + total_size_idx;
        render_buffers->idx_buffer = device.CreateBuffer<ImDrawIdx>(new_size, EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);
    }

    size_t            vertex_offset = 0;
    size_t            index_offset  = 0;
    Array<ImDrawVert> vertices(draw_data->TotalVtxCount);
    Array<ImDrawIdx>  indices(draw_data->TotalIdxCount);
    uint              total_cmd_cnt = 0;

    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        memcpy(vertices.data() + vertex_offset, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(indices.data() + index_offset, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vertex_offset += cmd_list->VtxBuffer.Size;
        index_offset += cmd_list->IdxBuffer.Size;
        total_cmd_cnt += cmd_list->CmdBuffer.Size;
    }
    Array<ImGUIArg> args;
    args.reserve(total_cmd_cnt);
    if (render_buffers->arg_buffer == nullptr || render_buffers->arg_buffer->GetNumElement() < total_cmd_cnt) {
        uint32_t new_size          = 128 + total_cmd_cnt;
        render_buffers->arg_buffer = device.CreateBuffer<ImGUIArg>(new_size, EBufferUsageFlags::TRANSFER_DST);
    }

    ImVec2 clip_off   = draw_data->DisplayPos;      // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale;// (1,1) unless using retina display which are often (2,2)

    Array<MeshDrawData> draw_meshes;
    VertexBuffer        vtx_buffers[] = {{render_buffers->vtx_buffer,
                                          0}};
    IndexBuffer         idx_buffer    = {
        render_buffers->idx_buffer->GetView(),
        EIndexElementType::IET_UINT16};
    draw_meshes.emplace_back(
        std::span<VertexBuffer>(vtx_buffers, 1),
        idx_buffer);

    MeshDrawData& batch = draw_meshes[0];

    batch.Reserve(total_cmd_cnt);

    int32_t global_vertex_offset = 0,
            global_index_offset  = 0;

    uint                      cmd_offset = 0;
    GUIPipelineBdls::Constant constant;

    {
        float l         = draw_data->DisplayPos.x;
        float r         = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float t         = draw_data->DisplayPos.y;
        float b         = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        float mvp[4][4] = {
            {2.0f / (r - l), 0.0f, 0.0f, (r + l) / (l - r)},
            {0.0f, 2.0f / (t - b), 0.5f, (t + b) / (b - t)},
            {0.0f, 0.0f, 0.f, 0.5f},
            {0.f, 0.0f, 0.0f, 1.0f},
        };
        memcpy(&constant.mvp, mvp, sizeof(mvp));
    }
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (uint32_t cmd_index = 0; cmd_index < cmd_list->CmdBuffer.Size; ++cmd_index) {
            const ImDrawCmd* cmd = &cmd_list->CmdBuffer[cmd_index];
            if (cmd->UserCallback != nullptr) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    // SetupRenderState(draw_data, _cmdlist, viewport_data);
                } else {
                    cmd->UserCallback(cmd_list, cmd);
                }
            } else {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min((cmd->ClipRect.x - clip_off.x) * clip_scale.x, (cmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((cmd->ClipRect.z - clip_off.x) * clip_scale.x, (cmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                uint texture_handle = (uint)cmd->TextureId;
                // arg_buffer[cmd_offset].min_xy = {clip_min.x, clip_min.y};
                // arg_buffer[cmd_offset].max_xy = {clip_max.x, clip_max.y};
                args.emplace_back(
                    ImVec2{clip_min.x, clip_min.y},
                    ImVec2{clip_max.x, clip_max.y},
                    texture_handle);

                uint32_t elem_count = cmd->ElemCount;
                uint32_t vtx_offset = cmd->VtxOffset + global_vertex_offset;
                uint32_t idx_offset = cmd->IdxOffset + global_index_offset;

                batch.EmplaceDrawIndexed(
                    idx_offset,
                    elem_count,
                    vtx_offset,
                    cmd_offset);
                cmd_offset++;
            }
        }
        global_index_offset += cmd_list->IdxBuffer.Size;
        global_vertex_offset += cmd_list->VtxBuffer.Size;
    }

    auto            vtx_view = render_buffers->vtx_buffer->GetView();
    auto            idx_view = render_buffers->idx_buffer->GetView();
    auto            arg_view = render_buffers->arg_buffer->GetView();
    Array<ImGUIArg> copy_back_args(render_buffers->arg_buffer->GetNumElement());

    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)vertices.data(), vertices.size() * sizeof(ImDrawVert)), vtx_view);
    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)indices.data(), indices.size() * sizeof(ImDrawIdx)), idx_view);
    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)args.data(), args.size() * sizeof(ImGUIArg)), arg_view);
    _cmdlist.CopyFrom(arg_view, std::span<Moer::byte>((Moer::byte*)copy_back_args.data(), copy_back_args.size() * sizeof(ImGUIArg)));

    _cmdlist.Gfx(backend_data.rast_pso, render_buffers->arg_buffer, render_backend.bindless_array, constant)
        .Draw(
            {0, 0, (uint)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x), uint(draw_data->DisplaySize.y * draw_data->FramebufferScale.y)},
            std::move(draw_meshes),
            ColorAttachment(_frame_buffer.GetTexture()));

    _cmdlist.AddCallback([vtx(std::move(vertices)),
                          idx(std::move(indices)),
                          arg(std::move(args)),
                          copy_back_args(std::move(copy_back_args))]() {
    });
}

// void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiViewportData* _viewport_data, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
//     GuiBackendData* backend_data = GetBackendData();

//     uint32_t num_frames_in_flight = backend_data->num_frames_in_flight;
//     // 1. bind pipeline
//     RHIGfxPsoRef pipeline = backend_data->pipeline;
//     EnqueueRenderTask([commandList, pipeline, _next_frame_info_render_thread] {
//         if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//         commandList->SetPipelineState(pipeline);
//     });
//     // commandList->SetPipelineState(backend_data->pipeline);

//     // // 2. global: push constants
//     // ImGuiShaderVert::Parameters vert_param;
//     // ImGuiShaderFrag::Parameters frag_param;
//     // frag_param.sampler0 = backend_data->font_sampler;
//     // std::memset(&vert_param.vertexBuffer, 0, sizeof(vert_param.vertexBuffer));
//     // {
//     //     float l         = draw_data->DisplayPos.x;
//     //     float r         = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
//     //     float t         = draw_data->DisplayPos.y;
//     //     float b         = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
//     //     float mvp[4][4] = {
//     //         {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
//     //         {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
//     //         {0.0f, 0.0f, 0.5f, 0.0f},
//     //         {(r + l) / (l - r), (t + b) / (b - t), 0.5f, 1.0f},
//     //     };
//     //     memcpy(&vert_param.vertexBuffer.mvp, mvp, sizeof(mvp));
//     // }
//     // RHIBatchedShaderParameters batched_params;
//     // batched_params.SetParameters(backend_data->shader_module_vert, vert_param);
//     // batched_params.SetParameters(backend_data->shader_module_frag, frag_param);
//     // // should internally allocate descriptor set for vulkan if not allocated
//     // EnqueueRenderTask([commandList, batched_params, pipeline, _next_frame_info_render_thread] {
//     //     if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
//     //     g_rhi->RHISetBatchedShaderParameters(pipeline, batched_params, true);
//     // });
//     // g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params, true);

//     // 3. global: set viewport, MARK: does it work ?
//     ViewPort view_port(0, 0, draw_data->DisplaySize.x * draw_data->FramebufferScale.x, draw_data->DisplaySize.y * draw_data->FramebufferScale.y, 0.f, 1.f);

//     EnqueueRenderTask([commandList, view_port, _viewport_data, num_frames_in_flight, _next_frame_info_render_thread] {
//         if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

//         GuiFrameRenderBuffers* render_buffers = &_viewport_data->render_buffers[_viewport_data->frame_index % num_frames_in_flight];

//         RHIBufferRef vertex_buffer = render_buffers->vertex_buffer;
//         RHIBufferRef index_buffer  = render_buffers->index_buffer;

//         commandList->SetViewPort(view_port);
//         // 4. global: bind vertex/index
//         uint32_t offsets[] = {0};
//         commandList->BindVertexBuffers(0, 1, &vertex_buffer, offsets);
//         commandList->BindIndexBuffer(index_buffer, 0, EIndexElementType::IET_UINT16);
//     });
// }

//
void InvalidateDeviceObjects() {
    GuiBackendData* bd = GetBackendData();
    if (!bd)
        return;

    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->SetTexID(0);// We copied bd->pFontTextureView to io.Fonts->TexID so let's clear that as well.
}
bool CreateDeviceObjects() {
    GuiBackendData* backend_data = GetBackendData();
    if (!backend_data)
        return false;
    if (backend_data->pipeline)
        InvalidateDeviceObjects();

    RHISamplerCreateInfo sampler_init(ESamplerFilter::SF_CUBIC,
                                      TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    backend_data->font_sampler = g_rhi->RHICreateSampler(sampler_init.SetCompareOp(SCF_ALWAYS));

    auto&        shader_resource_manager = ShaderResourceManager::GetInstance();
    RHIShaderRef gui_vert                = shader_resource_manager.GetShader<ImGuiShaderVert>();
    RHIShaderRef gui_frag                = shader_resource_manager.GetShader<ImGuiShaderFrag>();

    RHIVertexInputInfo vertex_input_info(
        ::VertexElement(0, IM_OFFSETOF(ImDrawVert, pos), PF_R32G32_SFLOAT, 0, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX),
        ::VertexElement(0, IM_OFFSETOF(ImDrawVert, uv), PF_R32G32_SFLOAT, 1, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX),
        ::VertexElement(0, IM_OFFSETOF(ImDrawVert, col), PF_R8G8B8A8_UNORM, 2, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX));

    RHIGraphicsPSOCreateInfo pso_create_info =
        std::move(RHIGraphicsPSOCreateInfo::Create()
                      .SetRasterizerInfo(RHIRasterizeInfo::Preset())
                      .SetMultisampleInfo(RHIMultisampleStateInfo::Preset())
                      .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset())
                      .SetShaderStage(std::move(RHIGraphicsShaderInputInfo::Create()
                                                    .SetVertexWorkFlow(
                                                        std::move(vertex_input_info),
                                                        gui_vert,
                                                        gui_frag)))
                      .SetColorAttachmentInfo(
                          {RHIColorAttachmentInfo::Preset(g_rhi->RHIGetMainViewport()->GetViewportInfo().backbuffer_format)
                               .SetBlendStateInfo(RHIBlendAttachmentInfo::Preset<Moer::Render::Blend::ALPHA_BLEND>())})
                      .Finalize());

    backend_data->pipeline = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));
    auto& sd_mgr           = Moer::Render::ShaderManager::Get();
    using namespace Moer::Render;
    VertexStream vertex_stream;
    vertex_stream.EmplacePerVertex(
        {Moer::Render::VertexElement(PF_R32G32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32_SFLOAT),
         Moer::Render::VertexElement(PF_R8G8B8A8_UNORM)});
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        vertex_stream,
        {RHIColorAttachmentInfo::Preset<Moer::Render::Blend::ALPHA_BLEND>(PF_R8G8B8A8_SRGB)},
        RHIDepthStencilStateInfo::Preset());
    backend_data->rast_pso = std::move(
        sd_mgr
            .Raster()
            .Vertex("GuiVert.hlsl")
            .Pixel("GuiFrag.hlsl")
            .Build<GUIPipeline>(std::move(pso_info)));
    // setup backend data
    backend_data->shader_module_vert = gui_vert;
    backend_data->shader_module_frag = gui_frag;

    //font texture and srv
    CreateFontsTexture();

    return true;
};

void CreateFontsTexture() {
    ImGuiIO&        io           = ImGui::GetIO();
    GuiBackendData* backend_data = GetBackendData();

    uint8_t* pixels;

    int width, height;
    //MARK... this is freaking slow, it's build first called, we need a default data for it, and async load other fonts
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    using namespace Moer::Render;
    auto& rd_device = Moer::Render::RenderDevice::Get();
    //upload texture
    {
        const uint32_t alignment = 256;
        // RHITextureRef  font_texture = nullptr;

        // font_texture            = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create("GuiFontTexture2D", ETextureDimension::TEX_2D)
        //                                            .SetNumSamples(1)
        //                                            .SetExtent({width, height})
        //                                            .SetNumMips(1)
        //                                            .SetArraySize(1)
        //                                            .SetFormat(PF_R8G8B8A8_UNORM)
        //                                            .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::SRGB | ETextureUsageFlags::TRANSFER_DST));
        uint32_t   upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
        uint32_t   upload_size  = height * upload_pitch;
        TextureRef font_tex     = rd_device.CreateTexture(
            Extent2D(width, height),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED);

        CommandList cmd_list;
        cmd_list.CopyFrom(
            std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

        rd_device.GetCommandQueue(EQueueType::Graphics).Execute(std::move(cmd_list.Submit()));
        rd_device.GetCommandQueue(EQueueType::Graphics).Sync();
        backend_data->font_tex = font_tex;
    }
    // io.Fonts->SetTexID((ImTextureID)backend_data->font_view);
}

void GuiCreateWindow(ImGuiViewport* _viewport);
void GuiDestroyWindow(ImGuiViewport* _viewport);
void GuiSetWindowSize(ImGuiViewport* _viewport, ImVec2 _size);
void GuiRenderWindow(ImGuiViewport* _viewport, void*);

void GuiInitPlatformInterface() {
    ImGuiPlatformIO& platform_io       = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow  = GuiCreateWindow;
    platform_io.Renderer_DestroyWindow = GuiDestroyWindow;
    platform_io.Renderer_SetWindowSize = GuiSetWindowSize;
    platform_io.Renderer_RenderWindow  = GuiRenderWindow;
    platform_io.Renderer_SwapBuffers   = GuiSwapbuffer;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers) {
    _render_buffers->vtx_buffer = nullptr;
    _render_buffers->idx_buffer = nullptr;
    _render_buffers->arg_buffer = nullptr;
}

void GuiCreateWindow(ImGuiViewport* _viewport) {
    ImGUIData&       backend_data  = *GetGUIBackendData();
    GuiViewportData* viewport_data = MoerNew(GuiViewportData)(backend_data.num_frames_in_flight);
    using namespace Moer::Render;
    ImGUIRenderBackend& render_backend = *backend_data.render_backend;
    RenderDevice&       rd_device      = render_backend.device;

    _viewport->RendererUserData = viewport_data;
    // viewport_data->copy_fence   = rd_device.CreateFence();
    // viewport_data->fence        = rd_device.CreateFence();

    Moer::WindowHandle handle{
        (Moer::WindowType*)(_viewport->PlatformHandle ?
                                _viewport->PlatformHandle :
                                _viewport->PlatformHandleRaw)};
    using namespace Moer::Render;
    using namespace Moer;

    int width, height;
    WindowContext::GetWindowSize(&handle, &width, &height);

    SwapchainCreateInfo swapchain_info{
        .window_handle    = (uint64)&handle,
        .size             = {(uint)_viewport->Size.x, (uint)_viewport->Size.y},
        .back_buffer_sz   = backend_data.num_frames_in_flight,
        .preferred_format = PF_R8G8B8A8_SRGB};

    viewport_data->sc          = rd_device.CreateSwapchain(swapchain_info);
    viewport_data->framebuffer = rd_device.CreateTexture(
        Extent2D(_viewport->Size.x, _viewport->Size.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
}

void GuiDestroyWindow(ImGuiViewport* _viewport) {
    ImGUIData& backend_data = *GetGUIBackendData();
    using namespace Moer::Render;
    using namespace Moer;
    auto& device = backend_data.render_backend->device;

    if (GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData) {
        if (viewport_data && viewport_data->sc) {
            device.GetCommandQueue(EQueueType::Graphics).Sync();
            viewport_data->sc          = nullptr;
            viewport_data->framebuffer = nullptr;
            // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
            for (uint32_t i = 0; i < backend_data.num_frames_in_flight; i++)
                DestroyRenderBuffers(&viewport_data->render_buffers[i]);
            MoerDelete(viewport_data);
        }
    }
    _viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* _viewport, ImVec2 _size) {
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;

    auto& rd_device = GetGUIBackendData()->render_backend->device;
    auto  sc        = viewport_data->sc;
    if (sc->size.x == _size.x && sc->size.y == _size.y) return;
    rd_device.GetCommandQueue(EQueueType::Graphics).Sync();

    Moer::WindowHandle handle{
        (Moer::WindowType*)(_viewport->PlatformHandle ?
                                _viewport->PlatformHandle :
                                _viewport->PlatformHandleRaw)};

    SwapchainCreateInfo swapchain_info{
        .window_handle    = (uintptr_t)&handle,
        .size             = {(uint)_size.x, (uint)_size.y},
        .back_buffer_sz   = 2,
        .preferred_format = PF_R8G8B8A8_SRGB};
    viewport_data->sc->Recreate(swapchain_info);
    viewport_data->framebuffer = rd_device.CreateTexture(
        Extent2D(_viewport->Size.x, _viewport->Size.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
}
void GuiRenderWindow(ImGuiViewport* _viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;

    auto sc = viewport_data->sc;
    if (!sc) return;
    CommandList cmd_list;
    auto&       device    = Moer::Render::RenderDevice::Get();
    auto        extent    = sc->size;
    auto&       gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
    GUIRender(_viewport->DrawData, viewport_data->framebuffer->GetView(), cmd_list);
    gfx_queue.Execute(std::move(cmd_list.Submit()));
    gfx_queue.Sync();
}

void GuiSwapbuffer(ImGuiViewport* _viewport, void*) {
    ImGUIData&       backend_data  = *GetGUIBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;
    backend_data.render_backend->device.GetCommandQueue(Moer::Render::EQueueType::Graphics).Present(viewport_data->sc, viewport_data->framebuffer);
}