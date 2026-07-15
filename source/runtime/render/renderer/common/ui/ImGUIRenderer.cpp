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
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>

#include <backends/imgui_impl_glfw.h>

using namespace Moer::Render;
using namespace Moer;

namespace Moer::Render {
struct ImGUIData;
}

void GuiInitPlatformInterface();
void GUIRender(
    UiViewportDrawPacket& _draw_packet,
    const TextureView&    _view,
    CommandList&,
    ImGUIData&          _backend_data,
    ImGUIRenderBackend& _render_backend
);
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
    FontDesc(const char* _font_path, float _font_size, EFontType _font_type) :
        font_path(_font_path),
        font_size(_font_size),
        font_type(_font_type) {}
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

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_BUFFER(arg_buffer);
    DEFINE_SHADER_CONSTANT_STRUCT(Constant, param);

    DEFINE_SHADER_ARGS(arg_buffer, bdls, param);
};
struct GuiFrameRenderBuffers {

    Moer::Render::BufferRef vtx_buffer;
    Moer::Render::BufferRef idx_buffer;
    Moer::Render::BufferRef arg_buffer;
};
struct GuiViewportRenderResources final : UiViewportRenderResources {
    SwapchainRef sc;

    Array<GuiFrameRenderBuffers> render_buffers;
    TextureRef                   framebuffer;

    uint64_t frame_index = 0;

    explicit GuiViewportRenderResources(uint32_t _frame_in_flight) : render_buffers(_frame_in_flight) {}
};

struct GuiViewportData {
    SharedPtr<GuiViewportRenderResources> render_resources;

    uint32_t        viewport_index = 0;
    static uint32_t viewport_count;

    explicit GuiViewportData(uint32_t _frame_in_flight) :
        render_resources(MakeShared<GuiViewportRenderResources>(_frame_in_flight)),
        viewport_index(viewport_count++) {}
    ~GuiViewportData() {
        viewport_count--;
    }
};
struct ImGUIData {
    static constexpr EPixelFormat s_supported_formats[] =
        {PF_R8G8B8A8_SRGB, PF_R8G8B8A8_UNORM, PF_B8G8R8A8_SRGB, PF_B8G8R8A8_UNORM};
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
        float icon_font_size =
            _desc.font_size * 2.0f /
            3.0f; // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

        icons_config.MergeMode        = true;
        icons_config.GlyphMinAdvanceX = icon_font_size;

        io.Fonts->AddFontFromFileTTF(
            font_path.generic_string().data(), icon_font_size, &icons_config, font_range
        );
    } else {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            font_path.generic_string().data(), _desc.font_size, &icons_config, font_range
        );
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
            AddFont({FONT_ICON_FILE_NAME_FAS, 13.0f, EFontType::Icon});
            AddFont({"msyh.ttc", 20.0f, EFontType::Chinese});
        }
    }
    bindless_array = device.CreateBindlessArray();

    ImGuiIO& io = ImGui::GetIO();
    assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

    { // Default ini file
        io.IniFilename = "imgui.ini";

        std::ifstream f(io.IniFilename);
        if (f.good()) {
            f.close();
            ImGui::LoadIniSettingsFromDisk(io.IniFilename);
        } else {
            LOG_INFO(
                "No existing imgui.ini file found, loading preset config: {}",
                ConfigManager::GetInstance().GetConfig().editor.preset_imgui_config_path
            );

            ImGui::LoadIniSettingsFromDisk(
                ConfigManager::GetInstance().GetConfig().editor.preset_imgui_config_path.c_str()
            );

            ImGui::SaveIniSettingsToDisk(io.IniFilename);
        }
    }

    auto     config              = Moer::ConfigManager::GetInstance().GetConfig();
    uint32_t max_frame_in_flight = config.engine.rhi.max_frame_in_flight;

    auto& sd_mgr = ShaderManager::Get();

    VertexStream vertex_stream;
    vertex_stream.EmplacePerVertex(
        {Moer::Render::VertexElement(PF_R32G32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32_SFLOAT),
         Moer::Render::VertexElement(PF_R8G8B8A8_UNORM)}
    );

    ImGUIData* render_backend_data = MoerNew(ImGUIData)();

    for (auto format : ImGUIData::s_supported_formats) {
        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset<Rast::CULL_NONE, FrontFace::CW>(),
            vertex_stream,
            {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(format)}
        );
        auto rast_pso = sd_mgr.Raster()
                            .Vertex("features/ui/GuiVert.hlsl")
                            .Pixel("features/ui/GuiFrag.hlsl")
                            .Build<GUIPipelineBdls>(std::move(pso_info));

        render_backend_data->rast_psos[format] = std::move(rast_pso);
    }

    render_backend_data->num_frames_in_flight = max_frame_in_flight;
    backend_data                              = render_backend_data;

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
        TextureRef     font_tex =
            rd_device.CreateTexture(Extent2D(width, height), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED);

        CommandList cmd_list;
        cmd_list.CopyFrom(std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

        rd_device.GetCommandQueue(EQueueType::Graphics).Execute(std::move(cmd_list.Submit()));
        rd_device.GetCommandQueue(EQueueType::Graphics).Sync();
        render_backend_data->font_texture = font_tex;
        uint handle = bindless_array->AllocateTexture(font_tex, Sampler(SF_CUBIC, SAM_REPEAT));
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
    ImGUIData* data = static_cast<ImGUIData*>(backend_data);
    ImGui::DestroyContext();
    bindless_array = nullptr;
    MoerDelete(data);
    backend_data = nullptr;
}
void ImGUIRenderBackend::BeginGUIFrame() {
    ImGui_ImplGlfw_NewFrame();

    ImGui::NewFrame();
}

void ImGUIRenderBackend::EndGUIFrame() {
    ImGui::Render();
}

void ImGUIRenderBackend::UpdatePlatformWindows() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    auto&         io      = ImGui::GetIO();
    if (context && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) &&
        context->FrameCountEnded == context->FrameCount &&
        context->FrameCountPlatformEnded < context->FrameCount) {
        ImGui::UpdatePlatformWindows();
    }
}

void ImGUIRenderBackend::RegisterImage(Texture* _texture, Sampler _sampler) {
    auto iter = registered_images.try_emplace(_texture, 0);
    if (iter.second) {
        uint handle = bindless_array->AllocateTexture(_texture->GetView(0, _texture->GetNumMips()), _sampler);
        iter.first->second = handle;
    }
}

void ImGUIRenderBackend::UnRegisterImage(Texture* _texture) {}

static UiViewportDrawPacket
CaptureViewportDrawPacket(ImDrawData* _draw_data, GuiViewportData* _viewport_data) {
    UiViewportDrawPacket packet{};
    if (!_draw_data || !_viewport_data || !_viewport_data->render_resources) {
        return packet;
    }

    static_assert(sizeof(UiDrawVertex) == sizeof(ImDrawVert));
    static_assert(offsetof(UiDrawVertex, position) == offsetof(ImDrawVert, pos));
    static_assert(offsetof(UiDrawVertex, uv) == offsetof(ImDrawVert, uv));
    static_assert(offsetof(UiDrawVertex, color) == offsetof(ImDrawVert, col));
    static_assert(sizeof(UiDrawIndex) == sizeof(ImDrawIdx));

    packet.display_position  = {_draw_data->DisplayPos.x, _draw_data->DisplayPos.y};
    packet.display_size      = {_draw_data->DisplaySize.x, _draw_data->DisplaySize.y};
    packet.framebuffer_scale = {_draw_data->FramebufferScale.x, _draw_data->FramebufferScale.y};
    packet.render_resources  = _viewport_data->render_resources;
    packet.framebuffer       = _viewport_data->render_resources->framebuffer;
    packet.swapchain         = _viewport_data->render_resources->sc;

    if (_draw_data->DisplaySize.x <= 0.f || _draw_data->DisplaySize.y <= 0.f) {
        return packet;
    }

    packet.vertices.resize(_draw_data->TotalVtxCount);
    packet.indices.resize(_draw_data->TotalIdxCount);

    size_t vertex_offset       = 0;
    size_t index_offset        = 0;
    uint32 total_command_count = 0;
    for (int32_t list_index = 0; list_index < _draw_data->CmdListsCount; ++list_index) {
        const ImDrawList* draw_list = _draw_data->CmdLists[list_index];
        std::memcpy(
            packet.vertices.data() + vertex_offset,
            draw_list->VtxBuffer.Data,
            draw_list->VtxBuffer.Size * sizeof(ImDrawVert)
        );
        std::memcpy(
            packet.indices.data() + index_offset,
            draw_list->IdxBuffer.Data,
            draw_list->IdxBuffer.Size * sizeof(ImDrawIdx)
        );
        vertex_offset += draw_list->VtxBuffer.Size;
        index_offset += draw_list->IdxBuffer.Size;
        total_command_count += draw_list->CmdBuffer.Size;
    }

    packet.commands.reserve(total_command_count);
    uint32 global_vertex_offset = 0;
    uint32 global_index_offset  = 0;
    for (int32_t list_index = 0; list_index < _draw_data->CmdListsCount; ++list_index) {
        const ImDrawList* draw_list = _draw_data->CmdLists[list_index];
        for (const ImDrawCmd& draw_command : draw_list->CmdBuffer) {
            if (draw_command.UserCallback != nullptr) {
                if (draw_command.UserCallback != ImDrawCallback_ResetRenderState) {
                    static std::atomic_bool warned_unsupported_callback = false;
                    if (!warned_unsupported_callback.exchange(true)) {
                        LOG_WARNING("Custom ImGui draw callbacks are skipped by copied UI frame packets.");
                    }
                }
                continue;
            }

            const float clip_min_x =
                (draw_command.ClipRect.x - _draw_data->DisplayPos.x) * _draw_data->FramebufferScale.x;
            const float clip_min_y =
                (draw_command.ClipRect.y - _draw_data->DisplayPos.y) * _draw_data->FramebufferScale.y;
            const float clip_max_x =
                (draw_command.ClipRect.z - _draw_data->DisplayPos.x) * _draw_data->FramebufferScale.x;
            const float clip_max_y =
                (draw_command.ClipRect.w - _draw_data->DisplayPos.y) * _draw_data->FramebufferScale.y;
            if (clip_max_x <= clip_min_x || clip_max_y <= clip_min_y) {
                continue;
            }

            packet.commands.emplace_back(UiDrawCommand{
                .clip_min       = {clip_min_x, clip_min_y},
                .clip_max       = {clip_max_x, clip_max_y},
                .texture_handle = static_cast<uint32>(draw_command.TextureId),
                .element_count  = draw_command.ElemCount,
                .vertex_offset  = draw_command.VtxOffset + global_vertex_offset,
                .index_offset   = draw_command.IdxOffset + global_index_offset
            });
        }
        global_index_offset += draw_list->IdxBuffer.Size;
        global_vertex_offset += draw_list->VtxBuffer.Size;
    }

    return packet;
}

UiDrawFramePacket ImGUIRenderBackend::CaptureDrawFrame() {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    UiDrawFramePacket frame{};
    ImDrawData*       main_draw_data = ImGui::GetDrawData();
    if (main_draw_data && main_draw_data->OwnerViewport) {
        auto* viewport_data = static_cast<GuiViewportData*>(main_draw_data->OwnerViewport->RendererUserData);
        frame.main_viewport = CaptureViewportDrawPacket(main_draw_data, viewport_data);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (platform_io.Viewports.Size > 1) {
            frame.platform_viewports.reserve(platform_io.Viewports.Size - 1);
        }
        for (int viewport_index = 1; viewport_index < platform_io.Viewports.Size; ++viewport_index) {
            ImGuiViewport* viewport = platform_io.Viewports[viewport_index];
            if ((viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0 || !viewport->DrawData) {
                continue;
            }
            auto* viewport_data   = static_cast<GuiViewportData*>(viewport->RendererUserData);
            auto  viewport_packet = CaptureViewportDrawPacket(viewport->DrawData, viewport_data);
            if (viewport_packet.framebuffer && viewport_packet.swapchain) {
                frame.platform_viewports.emplace_back(std::move(viewport_packet));
            }
        }

        if (!frame.platform_viewports.empty()) {
            static std::atomic_bool logged_platform_viewport_capture = false;
            if (!logged_platform_viewport_capture.exchange(true)) {
                LOG_INFO(
                    "[Threading] Copied ImGui frame includes {} platform viewport(s).",
                    frame.platform_viewports.size()
                );
            }
        }
    }

    return frame;
}

void ImGUIRenderBackend::RenderGUI(
    CommandList&           _cmd_list,
    const TextureView&     _main_framebuffer,
    UiDrawFramePacket&     _frame,
    EUiDrawExecutionThread _execution_thread
) {
    assert(
        (_execution_thread == EUiDrawExecutionThread::Game && IsCurrentlyGameThread()) ||
        (_execution_thread == EUiDrawExecutionThread::Render && IsCurrentlyRenderThread())
    );
    auto& render_data = *static_cast<ImGUIData*>(backend_data);

    _cmd_list.UpdateBindlessArray(bindless_array);
    GUIRender(_frame.main_viewport, _main_framebuffer, _cmd_list, render_data, *this);
    for (auto& viewport : _frame.platform_viewports) {
        if (viewport.framebuffer) {
            GUIRender(viewport, viewport.framebuffer->GetView(), _cmd_list, render_data, *this);
        }
    }
}

void ImGUIRenderBackend::PresentWindows(
    const UiDrawFramePacket& _frame,
    EUiDrawExecutionThread   _execution_thread
) {
    assert(
        (_execution_thread == EUiDrawExecutionThread::Game && IsCurrentlyGameThread()) ||
        (_execution_thread == EUiDrawExecutionThread::Render && IsCurrentlyRenderThread())
    );
    auto& graphics_queue = device.GetCommandQueue(EQueueType::Graphics);
    for (const auto& viewport : _frame.platform_viewports) {
        if (viewport.swapchain && viewport.framebuffer) {
            const auto framebuffer_view = viewport.framebuffer->GetView();
            if (framebuffer_view.extent.x != viewport.swapchain->size.x ||
                framebuffer_view.extent.y != viewport.swapchain->size.y) {
                LOG_WARNING(
                    "Skipping stale ImGui viewport present: source={}x{}, swapchain={}x{}.",
                    framebuffer_view.extent.x,
                    framebuffer_view.extent.y,
                    viewport.swapchain->size.x,
                    viewport.swapchain->size.y
                );
                continue;
            }
            graphics_queue.Present(viewport.swapchain, viewport.framebuffer);
        }
    }
}

TextureRef ImGUIRenderBackend::GetWindowFrameBuffer(void* _window) {
    ImGuiViewport*   viewport      = (ImGuiViewport*)_window;
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

    return viewport_data && viewport_data->render_resources && viewport_data->render_resources->framebuffer ?
               viewport_data->render_resources->framebuffer :
               TextureRef();
}

} // namespace Moer::Render

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);

uint32_t GuiViewportData::viewport_count = 0;

void GUIRender(
    UiViewportDrawPacket& _draw_packet,
    const TextureView&    _frame_buffer,
    CommandList&          _cmdlist,
    ImGUIData&            _backend_data,
    ImGUIRenderBackend&   _render_backend
) {
    if (_draw_packet.display_size.x <= 0.f || _draw_packet.display_size.y <= 0.f ||
        _frame_buffer.extent.x <= 0 || _frame_buffer.extent.y <= 0 || _draw_packet.commands.empty()) {
        return;
    }

    auto render_resources =
        std::static_pointer_cast<GuiViewportRenderResources>(_draw_packet.render_resources);
    if (!render_resources || render_resources->render_buffers.empty()) {
        return;
    }

    GuiFrameRenderBuffers& render_buffers =
        render_resources
            ->render_buffers[render_resources->frame_index % render_resources->render_buffers.size()];
    render_resources->frame_index++;

    auto&        device        = _render_backend.device;
    const uint32 vertex_count  = static_cast<uint32>(_draw_packet.vertices.size());
    const uint32 index_count   = static_cast<uint32>(_draw_packet.indices.size());
    const uint32 command_count = static_cast<uint32>(_draw_packet.commands.size());

    if (!render_buffers.vtx_buffer || render_buffers.vtx_buffer->GetNumElement() < vertex_count) {
        render_buffers.vtx_buffer = device.CreateBuffer<ImDrawVert>(
            "GUI::ImGUI Vertex Buffer",
            4096 + vertex_count,
            EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }
    if (!render_buffers.idx_buffer || render_buffers.idx_buffer->GetNumElement() < index_count) {
        render_buffers.idx_buffer = device.CreateBuffer<ImDrawIdx>(
            "GUI::ImGUI Index Buffer",
            8192 + index_count,
            EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }
    if (!render_buffers.arg_buffer || render_buffers.arg_buffer->GetNumElement() < command_count) {
        render_buffers.arg_buffer = device.CreateBuffer<ImGUIArg>(
            "GUI::ImGUI Arg Buffer",
            128 + command_count,
            EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::UNORDERED_ACCESS
        );
    }

    Array<ImGUIArg> args;
    args.reserve(command_count);

    Array<MeshDrawData> draw_meshes;
    VertexBuffer        vertex_buffers[] = {{render_buffers.vtx_buffer, 0}};
    IndexBuffer         index_buffer = {render_buffers.idx_buffer->GetView(), EIndexElementType::IET_UINT16};
    draw_meshes.emplace_back(std::span<VertexBuffer>(vertex_buffers, 1), index_buffer);
    MeshDrawData& batch = draw_meshes[0];
    batch.Reserve(command_count);

    uint32 command_offset = 0;
    for (const UiDrawCommand& command : _draw_packet.commands) {
        args.emplace_back(
            ImVec2{command.clip_min.x, command.clip_min.y},
            ImVec2{command.clip_max.x, command.clip_max.y},
            command.texture_handle
        );
        batch.EmplaceDrawIndexed(
            command.index_offset, command.element_count, command.vertex_offset, command_offset++
        );
    }

    GUIPipelineBdls::Constant constant{};
    const float               left      = _draw_packet.display_position.x;
    const float               right     = left + _draw_packet.display_size.x;
    const float               top       = _draw_packet.display_position.y;
    const float               bottom    = top + _draw_packet.display_size.y;
    const float               mvp[4][4] = {
        {2.f / (right - left), 0.f, 0.f, 0.f},
        {0.f, 2.f / (top - bottom), 0.f, 0.f},
        {0.f, 0.f, 0.f, 0.f},
        {(right + left) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1.f},
    };
    std::memcpy(static_cast<void*>(&constant.mvp), mvp, sizeof(mvp));

    const auto vertex_view = render_buffers.vtx_buffer->GetView();
    const auto index_view  = render_buffers.idx_buffer->GetView();
    const auto arg_view    = render_buffers.arg_buffer->GetView();
    _cmdlist.CopyFrom(
        std::span<Moer::byte>(
            reinterpret_cast<Moer::byte*>(_draw_packet.vertices.data()),
            _draw_packet.vertices.size() * sizeof(UiDrawVertex)
        ),
        vertex_view
    );
    _cmdlist.CopyFrom(
        std::span<Moer::byte>(
            reinterpret_cast<Moer::byte*>(_draw_packet.indices.data()),
            _draw_packet.indices.size() * sizeof(UiDrawIndex)
        ),
        index_view
    );
    _cmdlist.CopyFrom(
        std::span<Moer::byte>(reinterpret_cast<Moer::byte*>(args.data()), args.size() * sizeof(ImGUIArg)),
        arg_view
    );

    assert(_backend_data.rast_psos.contains(_frame_buffer.format) && "Unsupported GUI format");
    _cmdlist
        .Gfx(
            _backend_data.rast_psos[_frame_buffer.format],
            render_buffers.arg_buffer,
            _render_backend.bindless_array,
            constant
        )
        .Draw(
            "ImGui Draws",
            {0,
             0,
             static_cast<uint>(_draw_packet.display_size.x * _draw_packet.framebuffer_scale.x),
             static_cast<uint>(_draw_packet.display_size.y * _draw_packet.framebuffer_scale.y)},
            std::move(draw_meshes),
            ColorAttachment(_frame_buffer.GetTexture(), EAttachmentAction::AC_LOAD_STORE)
        );

    _cmdlist.AddCallback([vertices(std::move(_draw_packet.vertices)),
                          indices(std::move(_draw_packet.indices)),
                          args(std::move(args))]() {});
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

void CreateOrResizeViewportResources(
    RenderDevice&                                       _device,
    const SharedPtr<GuiViewportRenderResources>&        _resources,
    void*                                               _platform_window,
    uint2                                               _extent,
    uint32_t                                            _frame_in_flight,
    uint32_t                                            _viewport_index
) {
    assert(_resources);
    assert(!IsRenderThreadRunning() || IsCurrentlyRenderThread());

    const bool has_swapchain = _resources->sc.IsValid();
    const bool swapchain_matches =
        has_swapchain && _resources->sc->size.x == _extent.x && _resources->sc->size.y == _extent.y;
    const bool framebuffer_matches =
        _resources->framebuffer && _resources->framebuffer->GetExtent().x == _extent.x &&
        _resources->framebuffer->GetExtent().y == _extent.y;
    if (swapchain_matches && framebuffer_matches) {
        return;
    }

    auto& graphics_queue = _device.GetCommandQueue(EQueueType::Graphics);
    if (has_swapchain || _resources->framebuffer) {
        graphics_queue.Sync();
    }

    Moer::WindowHandle handle{static_cast<Moer::WindowType*>(_platform_window)};
    SwapchainCreateInfo swapchain_info{
        .window_handle    = reinterpret_cast<uintptr_t>(&handle),
        .size             = _extent,
        .back_buffer_sz   = _frame_in_flight,
        .preferred_format = PF_R8G8B8A8_SRGB
    };

    if (!has_swapchain) {
        _resources->sc = _device.CreateSwapchain(swapchain_info);
    } else if (!swapchain_matches) {
        _resources->sc->Sync();
        _resources->sc->Recreate(swapchain_info);
    }

    _resources->framebuffer = _device.CreateTexture(
        Extent2D(_extent.x, _extent.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );
    _resources->framebuffer->SetName(std::format("ImGui Window {}", _viewport_index));

    LOG_INFO(
        "[Threading][UI] {} ImGui viewport {} resources at {}x{} on {} Thread.",
        has_swapchain ? "Updated" : "Created",
        _viewport_index,
        _extent.x,
        _extent.y,
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
}

void ReleaseViewportResources(
    RenderDevice&                                _device,
    const SharedPtr<GuiViewportRenderResources>& _resources,
    uint32_t                                     _viewport_index
) {
    if (!_resources) {
        return;
    }
    assert(!IsRenderThreadRunning() || IsCurrentlyRenderThread());

    _device.GetCommandQueue(EQueueType::Graphics).Sync();
    if (_resources->sc) {
        _resources->sc->Sync();
    }
    _resources->sc          = nullptr;
    _resources->framebuffer = nullptr;
    for (auto& render_buffers : _resources->render_buffers) {
        DestroyRenderBuffers(&render_buffers);
    }
    _resources->frame_index = 0;

    LOG_INFO(
        "[Threading][UI] Released ImGui viewport {} resources on {} Thread.",
        _viewport_index,
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
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

    void* platform_window =
        _viewport->PlatformHandle ? _viewport->PlatformHandle : _viewport->PlatformHandleRaw;
    if (!platform_window) {
        return;
    }
    Moer::WindowHandle handle{static_cast<Moer::WindowType*>(platform_window)};
    using namespace Moer::Render;
    using namespace Moer;

    int width, height;
    WindowContext::GetWindowSize(&handle, &width, &height);
    const uint2 extent{static_cast<uint>(_viewport->Size.x), static_cast<uint>(_viewport->Size.y)};
    if (width == 0 || height == 0 || extent.x == 0 || extent.y == 0) {
        return;
    }

    const auto render_resources = viewport_data->render_resources;
    const auto viewport_index   = viewport_data->viewport_index;
    RunRenderThreadControlAndWait(
        [&rd_device,
         render_resources,
         platform_window,
         extent,
         frame_in_flight = backend_data.num_frames_in_flight,
         viewport_index]() {
            CreateOrResizeViewportResources(
                rd_device, render_resources, platform_window, extent, frame_in_flight, viewport_index
            );
        }
    );
}

void GuiDestroyWindow(ImGuiViewport* _viewport) {
    ImGUIData& backend_data = *GetGUIBackendData();
    using namespace Moer::Render;
    using namespace Moer;
    auto& device = backend_data.render_backend->device;

    if (GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData) {
        _viewport->RendererUserData = nullptr;

        auto       render_resources = std::move(viewport_data->render_resources);
        const auto viewport_index   = viewport_data->viewport_index;
        RunRenderThreadControlAndWait(
            [&device, render_resources, viewport_index]() {
                ReleaseViewportResources(device, render_resources, viewport_index);
            }
        );
        MoerDelete(viewport_data);
    }
    _viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* _viewport, ImVec2 _size) {
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;
    if (!viewport_data || !viewport_data->render_resources || _size.x == 0 || _size.y == 0) {
        return;
    }

    void* platform_window =
        _viewport->PlatformHandle ? _viewport->PlatformHandle : _viewport->PlatformHandleRaw;
    if (!platform_window) {
        return;
    }

    ImGUIData* backend_data = GetGUIBackendData();
    auto&      rd_device    = backend_data->render_backend->device;
    const auto render_resources = viewport_data->render_resources;
    const auto viewport_index   = viewport_data->viewport_index;
    const uint2 extent{static_cast<uint>(_size.x), static_cast<uint>(_size.y)};
    RunRenderThreadControlAndWait(
        [&rd_device,
         render_resources,
         platform_window,
         extent,
         frame_in_flight = backend_data->num_frames_in_flight,
         viewport_index]() {
            CreateOrResizeViewportResources(
                rd_device, render_resources, platform_window, extent, frame_in_flight, viewport_index
            );
        }
    );
}
void GuiRenderWindow(ImGuiViewport* _viewport, void* _cmd_list) {
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;
    if (!viewport_data || !viewport_data->render_resources || !viewport_data->render_resources->sc ||
        !viewport_data->render_resources->framebuffer) {
        return;
    }

    ImGUIData& backend_data = *GetGUIBackendData();
    auto       draw_packet  = CaptureViewportDrawPacket(_viewport->DrawData, viewport_data);
    GUIRender(
        draw_packet,
        viewport_data->render_resources->framebuffer->GetView(),
        *static_cast<CommandList*>(_cmd_list),
        backend_data,
        *backend_data.render_backend
    );
}

void GuiSwapbuffer(ImGuiViewport* _viewport, void*) {
    ImGUIData&       backend_data  = *GetGUIBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData;
    if (!viewport_data || !viewport_data->render_resources) {
        return;
    }
    backend_data.render_backend->device.GetCommandQueue(Moer::Render::EQueueType::Graphics)
        .Present(viewport_data->render_resources->sc, viewport_data->render_resources->framebuffer);
}
