// 负责 Dear ImGui 上下文、GPU 资源、绘制数据复制以及多视口交换链的完整后端流程。

#include "DearImGuiRenderer.h"

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
#include "rhi/RHIExecutor.h"

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
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>

#include <backends/imgui_impl_glfw.h>

using namespace Moer::Render;
using namespace Moer;

namespace Moer::Render {
struct ImGuiData;
struct GuiViewportData;
}

void InitializeGuiPlatformInterface();
bool RefreshGuiViewportPresentation(
    ImGuiViewport*                      viewport,
    Moer::Render::GuiViewportData&      viewport_data,
    Moer::Render::RenderDevice&         device,
    uint32_t                            frames_in_flight
);
void RenderGuiDrawPacket(
    const UiViewportDrawPacket& _draw_packet,
    const TextureView&    _framebuffer,
    CommandList&          _cmd_list,
    ImGuiData&            _backend_data,
    ImGuiRenderBackend&   _render_backend
);

namespace Moer::Render {

struct ImGuiDrawArgument {
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
class GuiPipeline : public RasterPipeline {
public:
    struct Constant {
        Matrix4x4f mvp;
    };
    DEFINE_RASTER_PIPELINE_CLASS(GuiPipeline)

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_BUFFER(arg_buffer);
    DEFINE_SHADER_CONSTANT_STRUCT(Constant, param);

    DEFINE_SHADER_ARGS(arg_buffer, bdls, param);
};
struct GuiFrameRenderBuffers {
    Moer::Render::BufferRef vertex_buffer;
    Moer::Render::BufferRef index_buffer;
    Moer::Render::BufferRef argument_buffer;
};
struct GuiViewportRenderResources final : UiViewportRenderResources {
    UniquePtr<PresentationSurface> presentation_surface;

    Array<GuiFrameRenderBuffers> render_buffers;

    uint64_t frame_index = 0;

    explicit GuiViewportRenderResources(uint32_t _frame_in_flight) : render_buffers(_frame_in_flight) {}

    uint64_t GetPendingRecordingSlot() const noexcept override {
        return frame_index;
    }

    bool IsPendingRecordingSlot(uint64_t slot) const noexcept override {
        return slot == frame_index;
    }

    void CommitRecordingSlot(uint64_t slot) noexcept override {
        assert(slot == frame_index);
        if (slot == frame_index) {
            ++frame_index;
        }
    }
};

struct GuiViewportData {
    SharedPtr<GuiViewportRenderResources> render_resources;
    SwapchainSurfaceInfo                  surface_info;
    WindowFrameSnapshotTracker            window_frame_tracker;
    WindowFrameSnapshot                   latest_window_frame;
    bool                                  presentation_refresh_pending = true;

    uint32_t        viewport_index = 0;
    static uint32_t viewport_count;

    explicit GuiViewportData(uint32_t _frame_in_flight) :
        render_resources(MakeShared<GuiViewportRenderResources>(_frame_in_flight)),
        viewport_index(viewport_count++) {}
    ~GuiViewportData() {
        viewport_count--;
    }
};
struct ImGuiData {
    static constexpr EPixelFormat s_supported_formats[] =
        {PF_R8G8B8A8_SRGB, PF_R8G8B8A8_UNORM, PF_B8G8R8A8_SRGB, PF_B8G8R8A8_UNORM};
    UnorderedMap<EPixelFormat, GuiPipeline> pipelines_by_format;

    TextureRef font_texture;
    uint32_t frames_in_flight;

    ImGuiRenderBackend* render_backend = nullptr;
};
static const ImWchar* FontTypeToRange(EFontType _font_type) {
    using namespace Moer;
    static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

    switch (_font_type) {
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
static void AddFont(const FontDesc& _desc) {
    const auto font_base_path = Moer::ConfigManager::GetInstance().GetEditorResourcePath() / FONTS_DIR;
    const auto font_path      = font_base_path / _desc.font_path;

    auto& io = ImGui::GetIO();

    const ImWchar* font_range = FontTypeToRange(_desc.font_type);
    ImFontConfig   icons_config;
    icons_config.MergeMode            = false;
    icons_config.PixelSnapH           = true;
    icons_config.FontDataOwnedByAtlas = false;
    if (_desc.font_type == EFontType::Icon) {
        // Font Awesome 图标需要缩小到原尺寸的 2/3，才能与正文基线正确对齐。
        const float icon_font_size = _desc.font_size * 2.0f / 3.0f;

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
static void* MallocWrapper(size_t size, void*) {
    return Memory::Malloc(size);
}
static void FreeWrapper(void* ptr, void*) {
    Memory::Free(ptr);
}
static void InitializeGlfwBackendAndFonts(RenderDevice& _device) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    auto* window = static_cast<GLFWwindow*>(WindowContext::GetMainWindow()->window);
    if (_device.GetRHIType() == ERHIType::Vulkan) {
        ImGui_ImplGlfw_InitForVulkan(window, true);
    } else {
        ImGui_ImplGlfw_InitForOther(window, true);
    }

    io.Fonts->AddFontDefault();
    AddFont({FONT_ICON_FILE_NAME_FAS, 13.0f, EFontType::Icon});
    AddFont({"msyh.ttc", 20.0f, EFontType::Chinese});
}

static void LoadImGuiLayout(ImGuiIO& _io) {
    _io.IniFilename = "imgui.ini";

    std::ifstream settings_file(_io.IniFilename);
    if (settings_file.good()) {
        settings_file.close();
        ImGui::LoadIniSettingsFromDisk(_io.IniFilename);
        return;
    }

    const auto& preset_path = ConfigManager::GetInstance().GetConfig().editor.preset_imgui_config_path;
    LOG_INFO("No existing imgui.ini file found, loading preset config: {}", preset_path);
    ImGui::LoadIniSettingsFromDisk(preset_path.c_str());
    ImGui::SaveIniSettingsToDisk(_io.IniFilename);
}

static uint UploadFontAtlasAndCreatePipelines(
    ImGuiRenderBackend& _backend,
    ImGuiData*          _backend_data,
    int                 _width,
    int                 _height,
    Array<byte>         _font_pixels
) {
    uint font_texture_handle = 0;
    RunRenderThreadControlAndWait(
        [&_backend,
         _backend_data,
         _width,
         _height,
         font_pixels = std::move(_font_pixels),
         &font_texture_handle]() mutable {
            assert(!IsRenderThreadRunning() || IsCurrentlyRenderThread());

            auto& device            = _backend.device;
            _backend.bindless_array = device.CreateBindlessArray();

            auto&        shader_manager = ShaderManager::Get();
            VertexStream vertex_stream;
            vertex_stream.EmplacePerVertex(
                {Moer::Render::VertexElement(PF_R32G32_SFLOAT),
                 Moer::Render::VertexElement(PF_R32G32_SFLOAT),
                 Moer::Render::VertexElement(PF_R8G8B8A8_UNORM)}
            );

            for (auto format : ImGuiData::s_supported_formats) {
                GfxPsoCreateInfo pso_info(
                    RHIRasterizeInfo::Preset<Rast::CULL_NONE, FrontFace::CW>(),
                    vertex_stream,
                    {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(format)}
                );
                auto rast_pso = shader_manager.Raster()
                                    .Vertex("features/ui/GuiVert.hlsl")
                                    .Pixel("features/ui/GuiFrag.hlsl")
                                    .Build<GuiPipeline>(std::move(pso_info));

                _backend_data->pipelines_by_format[format] = std::move(rast_pso);
            }

            TextureRef font_texture = device.CreateTexture(
                Extent2D(_width, _height), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED
            );
            CommandList cmd_list;
            cmd_list.CopyFrom(std::move(font_pixels), font_texture);

            RHIExecutor::Get().Submit(
                EQueueType::Graphics,
                std::move(cmd_list.Submit()),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

            _backend_data->font_texture = font_texture;
            font_texture_handle =
                _backend.bindless_array->AllocateTexture(font_texture, Sampler(SF_CUBIC, SAM_REPEAT));
            _backend.registered_images.try_emplace(font_texture, font_texture_handle);

            LOG_INFO(
                "[Threading][UI] Initialized ImGui GPU resources on {} Thread.",
                IsCurrentlyRenderThread() ? "Render" : "Game"
            );
        }
    );

    return font_texture_handle;
}

ImGuiRenderBackend::ImGuiRenderBackend(RenderDevice& _device) : device(_device) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    ImGui::SetAllocatorFunctions(MallocWrapper, FreeWrapper, nullptr);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    InitializeGlfwBackendAndFonts(device);

    ImGuiIO& io = ImGui::GetIO();
    assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");
    LoadImGuiLayout(io);

    const auto&    config              = Moer::ConfigManager::GetInstance().GetConfig();
    const uint32_t frames_in_flight    = config.engine.rhi.max_frame_in_flight;
    auto*          render_backend_data = MoerNew(ImGuiData)();
    render_backend_data->frames_in_flight     = frames_in_flight;
    render_backend_data->render_backend       = this;
    backend_data                              = render_backend_data;

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

    ImGui::GetMainViewport()->RendererUserData = MoerNew(GuiViewportData)(frames_in_flight);

    uint8_t* font_atlas_pixels = nullptr;
    int      font_atlas_width  = 0;
    int      font_atlas_height = 0;
    // 字体图集必须在创建首帧 GPU 资源前同步生成，避免首帧引用尚未就绪的纹理。
    io.Fonts->GetTexDataAsRGBA32(&font_atlas_pixels, &font_atlas_width, &font_atlas_height);
    const size_t font_byte_size =
        static_cast<size_t>(font_atlas_width) * static_cast<size_t>(font_atlas_height) * 4;
    Array<byte> font_pixels(font_byte_size);
    std::memcpy(font_pixels.data(), font_atlas_pixels, font_byte_size);

    const uint font_texture_handle = UploadFontAtlasAndCreatePipelines(
        *this, render_backend_data, font_atlas_width, font_atlas_height, std::move(font_pixels)
    );
    io.Fonts->SetTexID(font_texture_handle);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        InitializeGuiPlatformInterface();
    }
}
inline ImGuiData* GetImGuiBackendData() {
    if (!ImGui::GetCurrentContext()) {
        return nullptr;
    }
    return static_cast<ImGuiData*>(ImGui::GetIO().BackendRendererUserData);
}
void ImGuiRenderBackend::BeginGUIFrame() {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    ImGui_ImplGlfw_NewFrame();

    ImGui::NewFrame();
    ++input_capture_sequence;
    if (input_capture_sequence == 0) {
        ++input_capture_sequence;
    }
    input_snapshot = CaptureImGuiIOInput(input_capture_sequence);
}

void ImGuiRenderBackend::EndGUIFrame() {
    ImGui::Render();
}

const WindowInputSourceSnapshot& ImGuiRenderBackend::GetInputSnapshot() const noexcept {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    return input_snapshot;
}

void ImGuiRenderBackend::UpdatePlatformWindows() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    auto&         io      = ImGui::GetIO();
    if (context && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) &&
        context->FrameCountEnded == context->FrameCount &&
        context->FrameCountPlatformEnded < context->FrameCount) {
        ImGui::UpdatePlatformWindows();
    }
}

void ImGuiRenderBackend::RegisterImage(Texture* _texture, Sampler _sampler) {
    TextureRef texture = _texture;
    RunRenderThreadControlAndWait([this, texture, _sampler]() {
        auto iter = registered_images.try_emplace(texture.Get(), 0);
        if (iter.second) {
            uint handle =
                bindless_array->AllocateTexture(texture->GetView(0, texture->GetNumMips()), _sampler);
            iter.first->second = handle;
        }
    });
}

void ImGuiRenderBackend::UnregisterImage(Texture* _texture) {
    // Bindless 纹理句柄目前与 UI 后端同生命周期，暂不单独回收；保留接口供后续资源策略使用。
    static_cast<void>(_texture);
}

static UiViewportDrawPacket
CaptureViewportDrawPacket(ImDrawData* _draw_data, GuiViewportData* _viewport_data) {
    UiViewportDrawPacket packet{};
    if (!_viewport_data || !_viewport_data->render_resources) {
        return packet;
    }

    const PresentationSurface* presentation_surface =
        _viewport_data->render_resources->presentation_surface.get();
    packet.render_resources = _viewport_data->render_resources;
    if (presentation_surface) {
        packet.framebuffer = presentation_surface->GetFrameBuffer();
        packet.swapchain   = presentation_surface->GetSwapchain();
    }
    BindUiViewportWindowFrame(packet, _viewport_data->latest_window_frame);
    if (presentation_surface) {
        BindUiViewportPresentation(
            packet,
            presentation_surface->GetCommittedSnapshot()
        );
    }
    if (!_draw_data) {
        return packet;
    }

    static_assert(sizeof(UiDrawVertex) == sizeof(ImDrawVert));
    static_assert(offsetof(UiDrawVertex, position) == offsetof(ImDrawVert, pos));
    static_assert(offsetof(UiDrawVertex, uv) == offsetof(ImDrawVert, uv));
    static_assert(offsetof(UiDrawVertex, color) == offsetof(ImDrawVert, col));
    static_assert(sizeof(UiDrawIndex) == sizeof(ImDrawIdx));

    packet.display_position  = {_draw_data->DisplayPos.x, _draw_data->DisplayPos.y};
    packet.display_size      = {_draw_data->DisplaySize.x, _draw_data->DisplaySize.y};
    if (!packet.window_frame.IsValid()) {
        // ImGui's main viewport does not use the detached-window refresh path.
        // Preserve its platform backend scale until the renderer binds the
        // immutable main-window snapshot to the copied packet.
        packet.framebuffer_scale = {
            _draw_data->FramebufferScale.x,
            _draw_data->FramebufferScale.y
        };
    }

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

            UiClipRect clip_rect{
                .min = {draw_command.ClipRect.x, draw_command.ClipRect.y},
                .max = {draw_command.ClipRect.z, draw_command.ClipRect.w},
            };
            if (!ConvertUiClipRectToDrawable(
                    clip_rect,
                    packet.display_position,
                    float2(1.f, 1.f)
                )) {
                continue;
            }

            packet.commands.emplace_back(UiDrawCommand{
                .clip_min       = clip_rect.min,
                .clip_max       = clip_rect.max,
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

UiDrawFramePacket ImGuiRenderBackend::CaptureDrawFrame() {
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
            auto* viewport_data =
                static_cast<GuiViewportData*>(viewport->RendererUserData);
            if (viewport_data != nullptr) {
                static_cast<void>(RefreshGuiViewportPresentation(
                    viewport,
                    *viewport_data,
                    device,
                    static_cast<ImGuiData*>(backend_data)->frames_in_flight
                ));
            }
            const bool drawable =
                viewport_data != nullptr &&
                viewport_data->latest_window_frame.IsDrawable();
            if (!drawable ||
                (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0 ||
                !viewport->DrawData) {
                auto retained_packet =
                    CaptureViewportDrawPacket(nullptr, viewport_data);
                if (!drawable &&
                    IsUiViewportPresentationCommitted(retained_packet)) {
                    retained_packet.presentation_metadata_only = true;
                    frame.platform_viewports.emplace_back(
                        std::move(retained_packet)
                    );
                }
                continue;
            }
            auto  viewport_packet = CaptureViewportDrawPacket(viewport->DrawData, viewport_data);
            if (IsUiViewportPresentationCurrent(viewport_packet)) {
                frame.platform_viewports.emplace_back(std::move(viewport_packet));
            }
        }

    }

    return frame;
}

void ImGuiRenderBackend::RenderGUI(
    CommandList&           _cmd_list,
    const TextureView&     _main_framebuffer,
    const UiDrawFramePacket& _frame,
    EUiDrawExecutionThread _execution_thread
) {
    assert(
        (_execution_thread == EUiDrawExecutionThread::Game && IsCurrentlyGameThread()) ||
        (_execution_thread == EUiDrawExecutionThread::Render && IsCurrentlyRenderThread())
    );
    auto& render_data = *static_cast<ImGuiData*>(backend_data);

    _cmd_list.UpdateBindlessArray(bindless_array);
    if (!_frame.main_viewport.commands.empty()) {
        ScopedGpuMarker viewport_marker(
            _cmd_list, "Main Viewport", GpuMarkerPalette::Ui()
        );
        RenderGuiDrawPacket(_frame.main_viewport, _main_framebuffer, _cmd_list, render_data, *this);
    }
    for (size_t viewport_index = 0; viewport_index < _frame.platform_viewports.size(); ++viewport_index) {
        const auto& viewport = _frame.platform_viewports[viewport_index];
        if (IsUiViewportPresentationCurrent(viewport) && !viewport.commands.empty()) {
            ScopedGpuMarker viewport_marker(
                _cmd_list,
                std::format("Platform Viewport {}", viewport_index),
                GpuMarkerPalette::Ui()
            );
            RenderGuiDrawPacket(viewport, viewport.framebuffer->GetView(), _cmd_list, render_data, *this);
        }
    }

    if (!_frame.main_viewport.commands.empty()) {
        static std::atomic_bool logged_draw_packet_render = false;
        if (!logged_draw_packet_render.exchange(true)) {
            LOG_INFO(
                "[Threading] Copied ImGui frame includes {} platform viewport(s).",
                _frame.platform_viewports.size()
            );
        }
    }
}

void ImGuiRenderBackend::PresentWindows(
    const UiDrawFramePacket& _frame,
    EUiDrawExecutionThread   _execution_thread
) {
    assert(
        (_execution_thread == EUiDrawExecutionThread::Game && IsCurrentlyGameThread()) ||
        (_execution_thread == EUiDrawExecutionThread::Render && IsCurrentlyRenderThread())
    );
    for (const auto& viewport : _frame.platform_viewports) {
        if (IsUiViewportPresentationCurrent(viewport)) {
            const auto framebuffer_view = viewport.framebuffer->GetView();
            RHIExecutor::Get().Present(
                RHIPresentRequest(viewport.swapchain, framebuffer_view), true
            );
        }
    }
}

TextureRef ImGuiRenderBackend::GetWindowFrameBuffer(void* _window) {
    auto* viewport      = static_cast<ImGuiViewport*>(_window);
    auto* viewport_data = static_cast<GuiViewportData*>(viewport->RendererUserData);

    return viewport_data &&
                   viewport_data->render_resources &&
                   viewport_data->render_resources->presentation_surface ?
               viewport_data->render_resources->presentation_surface
                   ->GetFrameBuffer() :
               TextureRef{};
}

} // namespace Moer::Render

uint32_t GuiViewportData::viewport_count = 0;

static void EnsureGuiRenderBufferCapacity(
    GuiFrameRenderBuffers& _buffers,
    RenderDevice&          _device,
    uint32                 _vertex_count,
    uint32                 _index_count,
    uint32                 _command_count
) {
    if (!_buffers.vertex_buffer || _buffers.vertex_buffer->GetNumElement() < _vertex_count) {
        _buffers.vertex_buffer = _device.CreateBuffer<ImDrawVert>(
            "GUI::ImGui Vertex Buffer",
            4096 + _vertex_count,
            EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }
    if (!_buffers.index_buffer || _buffers.index_buffer->GetNumElement() < _index_count) {
        _buffers.index_buffer = _device.CreateBuffer<ImDrawIdx>(
            "GUI::ImGui Index Buffer",
            8192 + _index_count,
            EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }
    if (!_buffers.argument_buffer || _buffers.argument_buffer->GetNumElement() < _command_count) {
        _buffers.argument_buffer = _device.CreateBuffer<ImGuiDrawArgument>(
            "GUI::ImGui Argument Buffer",
            128 + _command_count,
            EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::UNORDERED_ACCESS
        );
    }
}

static void BuildGuiDrawBatch(
    const UiViewportDrawPacket& _draw_packet,
    GuiFrameRenderBuffers&      _render_buffers,
    Array<ImGuiDrawArgument>&   _arguments,
    Array<MeshDrawData>&        _draw_meshes
) {
    _arguments.reserve(_draw_packet.commands.size());

    VertexBuffer vertex_buffers[] = {{_render_buffers.vertex_buffer, 0}};
    IndexBuffer  index_buffer = {
        _render_buffers.index_buffer->GetView(), EIndexElementType::IET_UINT16
    };
    _draw_meshes.emplace_back(std::span<VertexBuffer>(vertex_buffers, 1), index_buffer);
    MeshDrawData& batch = _draw_meshes[0];
    batch.Reserve(_draw_packet.commands.size());

    uint32 command_offset = 0;
    for (const UiDrawCommand& command : _draw_packet.commands) {
        UiClipRect clip_rect{
            .min = command.clip_min,
            .max = command.clip_max,
        };
        if (!ConvertUiClipRectToDrawable(
                clip_rect,
                float2(0.f, 0.f),
                _draw_packet.framebuffer_scale
            )) {
            continue;
        }
        _arguments.emplace_back(
            ImVec2{clip_rect.min.x, clip_rect.min.y},
            ImVec2{clip_rect.max.x, clip_rect.max.y},
            command.texture_handle
        );
        batch.EmplaceDrawIndexed(
            command.index_offset, command.element_count, command.vertex_offset, command_offset++
        );
    }
}

static GuiPipeline::Constant CreateGuiProjection(const UiViewportDrawPacket& _draw_packet) {
    GuiPipeline::Constant constant{};
    const float           left   = _draw_packet.display_position.x;
    const float           right  = left + _draw_packet.display_size.x;
    const float           top    = _draw_packet.display_position.y;
    const float           bottom = top + _draw_packet.display_size.y;
    const float           mvp[4][4] = {
        {2.f / (right - left), 0.f, 0.f, 0.f},
        {0.f, 2.f / (top - bottom), 0.f, 0.f},
        {0.f, 0.f, 0.f, 0.f},
        {(right + left) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1.f},
    };
    std::memcpy(static_cast<void*>(&constant.mvp), mvp, sizeof(mvp));
    return constant;
}

static void UploadGuiDrawData(
    CommandList&                    _cmd_list,
    const UiViewportDrawPacket&     _draw_packet,
    Array<ImGuiDrawArgument>&       _arguments,
    const GuiFrameRenderBuffers&    _render_buffers
) {
    _cmd_list.CopyFrom(
        std::span<const Moer::byte>(
            reinterpret_cast<const Moer::byte*>(_draw_packet.vertices.data()),
            _draw_packet.vertices.size() * sizeof(UiDrawVertex)
        ),
        _render_buffers.vertex_buffer->GetView()
    );
    _cmd_list.CopyFrom(
        std::span<const Moer::byte>(
            reinterpret_cast<const Moer::byte*>(_draw_packet.indices.data()),
            _draw_packet.indices.size() * sizeof(UiDrawIndex)
        ),
        _render_buffers.index_buffer->GetView()
    );
    _cmd_list.CopyFrom(
        std::span<const Moer::byte>(
            reinterpret_cast<const Moer::byte*>(_arguments.data()),
            _arguments.size() * sizeof(ImGuiDrawArgument)
        ),
        _render_buffers.argument_buffer->GetView()
    );
}

void RenderGuiDrawPacket(
    const UiViewportDrawPacket& _draw_packet,
    const TextureView&    _framebuffer,
    CommandList&          _cmd_list,
    ImGuiData&            _backend_data,
    ImGuiRenderBackend&   _render_backend
) {
    if (_draw_packet.display_size.x <= 0.f || _draw_packet.display_size.y <= 0.f ||
        _framebuffer.extent.x <= 0 || _framebuffer.extent.y <= 0 || _draw_packet.commands.empty()) {
        return;
    }

    const uint32 draw_width =
        static_cast<uint32>(_draw_packet.display_size.x * _draw_packet.framebuffer_scale.x);
    const uint32 draw_height =
        static_cast<uint32>(_draw_packet.display_size.y * _draw_packet.framebuffer_scale.y);
    if (draw_width > _framebuffer.extent.x || draw_height > _framebuffer.extent.y) {
        LOG_WARNING(
            "Skipping stale ImGui draw packet: draw={}x{}, target={}x{}.",
            draw_width,
            draw_height,
            _framebuffer.extent.x,
            _framebuffer.extent.y
        );
        return;
    }

    auto render_resources =
        std::static_pointer_cast<GuiViewportRenderResources>(_draw_packet.render_resources);
    if (!render_resources || render_resources->render_buffers.empty()) {
        return;
    }

    if (_draw_packet.recording_slot == UiViewportDrawPacket::InvalidRecordingSlot ||
        !render_resources->IsPendingRecordingSlot(_draw_packet.recording_slot)) {
        throw std::logic_error(
            "copied UI draw packet does not own the current upload-ring slot"
        );
    }
    GuiFrameRenderBuffers& render_buffers =
        render_resources->render_buffers[
            _draw_packet.recording_slot % render_resources->render_buffers.size()
        ];

    auto&        device        = _render_backend.device;
    const uint32 vertex_count  = static_cast<uint32>(_draw_packet.vertices.size());
    const uint32 index_count   = static_cast<uint32>(_draw_packet.indices.size());
    const uint32 command_count = static_cast<uint32>(_draw_packet.commands.size());

    EnsureGuiRenderBufferCapacity(render_buffers, device, vertex_count, index_count, command_count);

    Array<ImGuiDrawArgument> arguments;
    Array<MeshDrawData> draw_meshes;
    BuildGuiDrawBatch(_draw_packet, render_buffers, arguments, draw_meshes);
    auto constant = CreateGuiProjection(_draw_packet);
    UploadGuiDrawData(_cmd_list, _draw_packet, arguments, render_buffers);

    assert(_backend_data.pipelines_by_format.contains(_framebuffer.format) && "Unsupported GUI format");
    _cmd_list
        .Gfx(
            _backend_data.pipelines_by_format[_framebuffer.format],
            render_buffers.argument_buffer,
            _render_backend.bindless_array,
            constant
        )
        .Draw(
            "ImGui Draws",
            {0, 0, draw_width, draw_height},
            std::move(draw_meshes),
            ColorAttachment(_framebuffer.GetTexture(), EAttachmentAction::AC_LOAD_STORE)
        );

}

void GuiCreateWindow(ImGuiViewport* _viewport);
void GuiDestroyWindow(ImGuiViewport* _viewport);
void GuiSetWindowSize(ImGuiViewport* _viewport, ImVec2 _size);

void InitializeGuiPlatformInterface() {
    ImGuiPlatformIO& platform_io       = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow  = GuiCreateWindow;
    platform_io.Renderer_DestroyWindow = GuiDestroyWindow;
    platform_io.Renderer_SetWindowSize = GuiSetWindowSize;
    // Rendering and Present are consumed from immutable UI frame packets on
    // the Render owner. Do not advertise the legacy GT callbacks used by
    // RenderPlatformWindowsDefault(); they cannot carry our CommandList or
    // presentation ownership contract.
    platform_io.Renderer_RenderWindow = nullptr;
    platform_io.Renderer_SwapBuffers  = nullptr;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers) {
    _render_buffers->vertex_buffer   = nullptr;
    _render_buffers->index_buffer    = nullptr;
    _render_buffers->argument_buffer = nullptr;
}

bool CreateOrResizeViewportResources(
    RenderDevice&                                _device,
    const SharedPtr<GuiViewportRenderResources>& _resources,
    SwapchainSurfaceInfo                         _surface_info,
    WindowFrameSnapshot                          _window_frame,
    uint32_t                                     _frame_in_flight,
    uint32_t                                     _viewport_index
) {
    assert(_resources);
    assert(!IsRenderThreadRunning() || IsCurrentlyRenderThread());
    if (!_surface_info.IsValid() || !_window_frame.IsDrawable() ||
        _surface_info.GetIdentity() != _window_frame.surface_identity) {
        return false;
    }

    const bool had_committed_surface =
        _resources->presentation_surface &&
        _resources->presentation_surface->GetCommittedSnapshot().IsValid();
    if (!_resources->presentation_surface) {
        _resources->presentation_surface =
            MakeUnique<PresentationSurface>(
                _device,
                PresentationSurfaceDesc{
                    .back_buffer_count = _frame_in_flight,
                    .preferred_format  = PF_R8G8B8A8_SRGB,
                    .debug_name = std::format(
                        "ImGui Window {}", _viewport_index
                    ),
                    .frame_buffer = PresentationSurfaceFrameBufferDesc{
                        .format = PF_R8G8B8A8_SRGB,
                        .usage =
                            ETextureUsageFlags::COLOR_ATTACHMENT |
                            ETextureUsageFlags::SAMPLED |
                            ETextureUsageFlags::PRESENTATION_SOURCE |
                            ETextureUsageFlags::TRANSFER_SRC |
                            ETextureUsageFlags::TRANSFER_DST,
                    },
                }
            );
    }

    const EPresentationSurfaceEnsureResult result =
        _resources->presentation_surface->EnsureCurrent(
            _surface_info,
            _window_frame
        );
    if (!IsPresentationSurfaceReady(result)) {
        return false;
    }
    if (result == EPresentationSurfaceEnsureResult::Current) {
        return true;
    }

    const Extent2D actual_extent =
        _resources->presentation_surface->GetExtent();

    LOG_INFO(
        "[Threading][UI] {} ImGui viewport {} resources at {}x{} on {} Thread.",
        had_committed_surface ? "Updated" : "Created",
        _viewport_index,
        actual_extent.x,
        actual_extent.y,
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
    return true;
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

    if (_resources->presentation_surface) {
        _resources->presentation_surface->Release();
        _resources->presentation_surface.reset();
    }
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

ImGuiRenderBackend::~ImGuiRenderBackend() {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    if (ImGui::GetCurrentContext() && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        ImGui::DestroyPlatformWindows();
    }

    ImGuiData*       data          = static_cast<ImGuiData*>(backend_data);
    ImGuiViewport*   main_viewport = ImGui::GetMainViewport();
    GuiViewportData* viewport_data = static_cast<GuiViewportData*>(main_viewport->RendererUserData);
    main_viewport->RendererUserData = nullptr;

    auto       render_resources = viewport_data ? std::move(viewport_data->render_resources) : nullptr;
    const auto viewport_index   = viewport_data ? viewport_data->viewport_index : 0;
    RunRenderThreadControlAndWait([this, data, render_resources, viewport_index]() {
        ReleaseViewportResources(device, render_resources, viewport_index);
        bindless_array = nullptr;
        if (data) {
            data->font_texture = nullptr;
            data->pipelines_by_format.clear();
        }

        LOG_INFO(
            "[Threading][UI] Released ImGui backend GPU resources on {} Thread.",
            IsCurrentlyRenderThread() ? "Render" : "Game"
        );
    });

    if (viewport_data) {
        MoerDelete(viewport_data);
    }
    registered_images.clear();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    MoerDelete(data);
    backend_data = nullptr;
}

bool RefreshGuiViewportPresentation(
    ImGuiViewport*    viewport,
    GuiViewportData&  viewport_data,
    RenderDevice&     device,
    uint32_t          frames_in_flight
) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    void* platform_window =
        viewport->PlatformHandle ? viewport->PlatformHandle : viewport->PlatformHandleRaw;
    if (platform_window == nullptr) {
        viewport_data.latest_window_frame =
            viewport_data.window_frame_tracker.Advance({}, {});
        viewport_data.presentation_refresh_pending = true;
        return false;
    }

    const WindowHandle handle{static_cast<WindowType*>(platform_window)};
    const SwapchainSurfaceInfo incoming_surface =
        WindowContext::CreateSwapchainSurfaceInfo(handle);
    if (incoming_surface.IsValid()) {
        viewport_data.surface_info = incoming_surface;
    }
    viewport_data.latest_window_frame = viewport_data.window_frame_tracker.Advance(
        incoming_surface.GetIdentity(),
        WindowContext::CaptureWindowFrameMetrics(handle)
    );

    const auto transition = viewport_data.latest_window_frame.transition;
    const bool presentation_changed =
        transition == EWindowFrameTransition::Initial ||
        transition == EWindowFrameTransition::DrawableResized ||
        transition == EWindowFrameTransition::Restored ||
        transition == EWindowFrameTransition::Replaced;
    viewport_data.presentation_refresh_pending =
        viewport_data.presentation_refresh_pending || presentation_changed;
    if (!viewport_data.latest_window_frame.IsDrawable()) {
        return false;
    }
    if (!viewport_data.presentation_refresh_pending) {
        return true;
    }

    const auto render_resources = viewport_data.render_resources;
    const auto surface_info     = viewport_data.surface_info;
    const auto window_frame     = viewport_data.latest_window_frame;
    const auto viewport_index   = viewport_data.viewport_index;
    bool       committed        = false;
    RunRenderThreadControlAndWait(
        [&device,
         render_resources,
         surface_info,
         window_frame,
         frames_in_flight,
         viewport_index,
         &committed]() {
            committed = CreateOrResizeViewportResources(
                device,
                render_resources,
                surface_info,
                window_frame,
                frames_in_flight,
                viewport_index
            );
        }
    );
    viewport_data.presentation_refresh_pending = !committed;
    return committed;
}

void GuiCreateWindow(ImGuiViewport* _viewport) {
    ImGuiData&       backend_data  = *GetImGuiBackendData();
    GuiViewportData* viewport_data = MoerNew(GuiViewportData)(backend_data.frames_in_flight);
    ImGuiRenderBackend& render_backend = *backend_data.render_backend;

    _viewport->RendererUserData = viewport_data;
    static_cast<void>(RefreshGuiViewportPresentation(
        _viewport,
        *viewport_data,
        render_backend.device,
        backend_data.frames_in_flight
    ));
}

void GuiDestroyWindow(ImGuiViewport* _viewport) {
    ImGuiData& backend_data = *GetImGuiBackendData();
    using namespace Moer::Render;
    using namespace Moer;
    auto& device = backend_data.render_backend->device;

    if (auto* viewport_data = static_cast<GuiViewportData*>(_viewport->RendererUserData)) {
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
    auto* viewport_data = static_cast<GuiViewportData*>(_viewport->RendererUserData);
    if (!viewport_data || !viewport_data->render_resources) {
        return;
    }
    static_cast<void>(_size);
    ImGuiData* backend_data = GetImGuiBackendData();
    if (backend_data == nullptr) {
        return;
    }
    static_cast<void>(RefreshGuiViewportPresentation(
        _viewport,
        *viewport_data,
        backend_data->render_backend->device,
        backend_data->frames_in_flight
    ));
}
