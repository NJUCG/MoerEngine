#include "ImGUIRenderer.h"
#include "GLFW/glfw3.h"
#include "IconsFontAwesome6.h"
#include "PixelFormat.h"
#include "RenderThread.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
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

void GuiInitPlatformInterface();
void GUIRender(void* _draw_data, const TextureView& _view, CommandList&);
void GuiRenderWindow(ImGuiViewport* _viewport, void* _cmd_list);
void GuiSwapbuffer(ImGuiViewport* _viewport, void*);

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
    class GUIPipelineBdls : public RasterPipeline {
    public:
        struct Constant {
            Matrix4x4f mvp;
        };
        DEFINE_RASTER_PIPELINE_CLASS(GUIPipelineBdls)

        DEFINE_SHADER_BUFFER(arg_buffer);
        DEFINE_SHADER_BINDLESS_ARRAY(bdls);
        DEFINE_SHADER_CONSTANT_STRUCT(Constant, param);

        DEFINE_SHADER_ARGS(arg_buffer, bdls, param);
    };
    struct GuiFrameRenderBuffers {

        Moer::Render::BufferRef vtx_buffer;
        Moer::Render::BufferRef idx_buffer;
        Moer::Render::BufferRef arg_buffer;
    };
    struct GuiViewportData {
        SwapchainRef sc;

        Array<GuiFrameRenderBuffers> render_buffers;// Used by all viewports
        TextureRef                   framebuffer;

        uint64_t        frame_index;
        uint32_t        viewport_index;
        static uint32_t viewport_count;

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
    struct ImGUIData {
        static constexpr EPixelFormat               s_supported_formats[] = {PF_R8G8B8A8_SRGB,
                                                                             PF_R8G8B8A8_UNORM,
                                                                             //  PF_B8G8R8A8_SRGB,
                                                                             PF_B8G8R8A8_UNORM};
        UnorderedMap<EPixelFormat, GUIPipelineBdls> rast_psos;

        TextureRef font_texture;
        // Render buffers for main window
        uint32_t num_frames_in_flight;

        ImGUIRenderBackend* render_backend = nullptr;
        ImGUIData() {}
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

        auto     config              = Moer::ConfigManager::GetInstance().GetConfig();
        uint32_t max_frame_in_flight = config.engine.rhi.max_frame_in_flight;

        auto& sd_mgr = ShaderManager::Get();

        VertexStream vertex_stream;
        vertex_stream.EmplacePerVertex(
            {Moer::Render::VertexElement(PF_R32G32_SFLOAT),
             Moer::Render::VertexElement(PF_R32G32_SFLOAT),
             Moer::Render::VertexElement(PF_R8G8B8A8_UNORM)});

        ImGUIData* render_backend_data = MoerNew(ImGUIData)();

        for (auto format : ImGUIData::s_supported_formats) {
            GfxPsoCreateInfo pso_info(
                RHIRasterizeInfo::Preset<Rast::CULL_NONE, FrontFace::CW>(),
                vertex_stream,
                {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(format)});
            auto rast_pso = sd_mgr.Raster()
                                .Vertex("GuiVert.hlsl")
                                .Pixel("GuiFrag.hlsl")
                                .Build<GUIPipelineBdls>(std::move(pso_info));

            render_backend_data->rast_psos[format] = std::move(rast_pso);
        }

        render_backend_data->num_frames_in_flight = max_frame_in_flight;

        io.BackendRendererUserData = render_backend_data;
        io.BackendRendererName     = "Moer";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiViewport*   main_viewport      = ImGui::GetMainViewport();
        GuiViewportData* viewport_data      = MoerNew(GuiViewportData)(max_frame_in_flight);
        render_backend_data->render_backend = this;
        main_viewport->RendererUserData     = viewport_data;

        uint8_t* pixels;

        int width, height;
        //MARK... this is freaking slow, it's build first called, we need a default data for it, and async load other fonts
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        using namespace Moer::Render;
        auto& rd_device = Moer::Render::RenderDevice::Get();
        //upload texture
        {
            const uint32_t alignment    = 256;
            uint32_t       upload_pitch = Moer::AlignUp(width * 4, alignment);
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
            uint handle                       = bindless_array->AllocateTexture(font_tex, Sampler(SF_CUBIC, SAM_REPEAT));
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
        {
            //delete main viewport data
            ImGuiViewport*   main_viewport = ImGui::GetMainViewport();
            GuiViewportData* viewport_data = (GuiViewportData*)main_viewport->RendererUserData;
            MoerDelete(viewport_data);
            main_viewport->RendererUserData = nullptr;
        }
        ImGui_ImplGlfw_Shutdown();
        ImGUIData* data = GetGUIBackendData();
        ImGui::DestroyContext();
        bindless_array = nullptr;
        MoerDelete(data);
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

        // if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        //     return;

        ImDrawData* draw_data = ImGui::GetDrawData();
        // if (!draw_data)
        //     return;

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
                        GuiRenderWindow(platform_io.Viewports[i], &_cmd_list);
            }
        }
    }

    void ImGUIRenderBackend::PresentWindows() {
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        auto&            io          = ImGui::GetIO();

        if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiSwapbuffer(platform_io.Viewports[i], nullptr);
        }
    }

    TextureView ImGUIRenderBackend::GetWindowFrameBuffer(void* _window) {
        ImGuiViewport*   viewport      = (ImGuiViewport*)_window;
        GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

        return viewport_data && viewport_data->framebuffer ? viewport_data->framebuffer->GetView() : TextureView();
    }

}// namespace Moer::Render

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);

uint32_t GuiViewportData::viewport_count = 0;

void GUIRender(void* _draw_data, const TextureView& _frame_buffer, CommandList& _cmdlist) {
    ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);

    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    if (_frame_buffer.extent.x <= 0 || _frame_buffer.extent.y <= 0)
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
        render_buffers->vtx_buffer = device.CreateBuffer<ImDrawVert>("GUI::ImGUI Vertex Buffer", new_size, EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);
    }
    if (render_buffers->idx_buffer == nullptr || render_buffers->idx_buffer->GetNumElement() < total_size_idx) {

        if (render_buffers->idx_buffer != nullptr) {}
        uint32_t new_size          = 8192 + total_size_idx;
        render_buffers->idx_buffer = device.CreateBuffer<ImDrawIdx>("GUI::ImGUI Index Buffer", new_size, EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);
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
        render_buffers->arg_buffer = device.CreateBuffer<ImGUIArg>("GUI::ImGUI Arg Buffer", new_size, EBufferUsageFlags::TRANSFER_DST);
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
        float l = draw_data->DisplayPos.x;
        float r = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float t = draw_data->DisplayPos.y;
        float b = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        // float mvp[4][4] = {
        //     {2.0f / (r - l), 0.0f, 0.0f, (r + l) / (l - r)},
        //     {0.0f, 2.0f / (t - b), 0.5f, (t + b) / (b - t)},
        //     {0.0f, 0.0f, 0.f, 0.5f},
        //     {0.f, 0.0f, 0.0f, 1.0f},
        // };

        //transposed
        float mvp[4][4] = {
            {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
            {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
            {0.0f, 0.0f, 0.f, 0.0f},
            {(r + l) / (l - r), (t + b) / (b - t), 0.5f, 1.0f},
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

    auto vtx_view = render_buffers->vtx_buffer->GetView();
    auto idx_view = render_buffers->idx_buffer->GetView();
    auto arg_view = render_buffers->arg_buffer->GetView();
    // Array<ImGUIArg> copy_back_args(render_buffers->arg_buffer->GetNumElement());
    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)vertices.data(), vertices.size() * sizeof(ImDrawVert)), vtx_view);
    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)indices.data(), indices.size() * sizeof(ImDrawIdx)), idx_view);
    _cmdlist.CopyFrom(std::span<Moer::byte>((Moer::byte*)args.data(), args.size() * sizeof(ImGUIArg)), arg_view);
    // _cmdlist.CopyFrom(arg_view, std::span<Moer::byte>((Moer::byte*)copy_back_args.data(), copy_back_args.size() * sizeof(ImGUIArg)));
    assert(backend_data.rast_psos.contains(_frame_buffer.format) && "Unsupported GUI format");
    _cmdlist.Gfx(backend_data.rast_psos[_frame_buffer.format], render_buffers->arg_buffer, render_backend.bindless_array, constant)
        .Draw("ImGui Draws",
              {0, 0, (uint)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x), uint(draw_data->DisplaySize.y * draw_data->FramebufferScale.y)},
              std::move(draw_meshes),
              ColorAttachment(_frame_buffer.GetTexture(), EAttachmentAction::AC_LOAD_STORE));

    _cmdlist.AddCallback([vtx(std::move(vertices)),
                          idx(std::move(indices)),
                          arg(std::move(args))
                          //   copy_back_args(std::move(copy_back_args))
    ]() {
    });
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
        .preferred_format = PF_R8G8B8A8_UNORM};

    if (width == 0 || height == 0) {
        return;
    }
    viewport_data->sc          = rd_device.CreateSwapchain(swapchain_info);
    viewport_data->framebuffer = rd_device.CreateTexture(
        Extent2D(_viewport->Size.x, _viewport->Size.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
    viewport_data->framebuffer->SetName(std::format("ImGui Window {}", viewport_data->viewport_index));
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

    if (_size.x == 0 || _size.y == 0) return;
    viewport_data->sc->Recreate(swapchain_info);
    viewport_data->framebuffer = rd_device.CreateTexture(
        Extent2D(_viewport->Size.x, _viewport->Size.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);
}
void GuiRenderWindow(ImGuiViewport* _viewport, void* _cmd_list) {
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;

    auto sc = viewport_data->sc;
    if (!sc) return;
    auto& device    = Moer::Render::RenderDevice::Get();
    auto  extent    = sc->size;
    auto& gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
    GUIRender(_viewport->DrawData, viewport_data->framebuffer->GetView(), *(CommandList*)(_cmd_list));
    // gfx_queue.Execute(std::move(cmd_list.Submit()));
    // gfx_queue.Sync();
}

void GuiSwapbuffer(ImGuiViewport* _viewport, void*) {
    ImGUIData&       backend_data  = *GetGUIBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;
    backend_data.render_backend->device.GetCommandQueue(Moer::Render::EQueueType::Graphics).Present(viewport_data->sc, viewport_data->framebuffer);
}