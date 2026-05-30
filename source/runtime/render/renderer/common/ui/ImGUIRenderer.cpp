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
#include "renderer/common/PresentationSurface.h"
#include "string/Format.h"

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
#include <string>

#include <backends/imgui_impl_glfw.h>

using namespace Moer::Render;
using namespace Moer;

void GuiInitPlatformInterface();
void GUIRender(void* _draw_data, const TextureView& _view, CommandList&);
void GuiRenderWindow(ImGuiViewport* _viewport, void* _cmd_list);
void GuiSwapbuffer(ImGuiViewport* _viewport, void*);

namespace Moer::Render {

namespace {
constexpr uint64_t k_render_output_texture_token_bit = 1ull << 63;
constexpr uint64_t k_render_output_slot_index_mask   = (1ull << 31) - 1ull;

bool IsRenderOutputTextureId(uint64_t texture_id) {
    return (texture_id & k_render_output_texture_token_bit) != 0;
}

UIRenderer::RenderOutputSlotHandle DecodeRenderOutputTextureId(uint64_t texture_id) {
    return UIRenderer::RenderOutputSlotHandle{
        .slot_index = static_cast<uint32_t>(texture_id & k_render_output_slot_index_mask),
        .generation = static_cast<uint32_t>((texture_id >> 31) & 0xFFFFFFFFull),
    };
}

uint64_t EncodeRenderOutputTextureId(UIRenderer::RenderOutputSlotHandle handle) {
    if (!handle.IsValid() || handle.slot_index > k_render_output_slot_index_mask) {
        return 0;
    }
    return k_render_output_texture_token_bit |
           (static_cast<uint64_t>(handle.generation) << 31) |
           static_cast<uint64_t>(handle.slot_index);
}
} // namespace

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
struct GuiViewportData {
    UniquePtr<PresentationSurface> surface;

    Array<GuiFrameRenderBuffers> render_buffers; // Used by all viewports

    uint64_t        frame_index = 0;
    uint32_t        viewport_index = 0;
    static uint32_t viewport_count;

    GuiViewportData(uint32_t _frame_in_flight) {
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

static GuiViewportData* GetGuiViewportData(ImGuiViewport* _viewport) {
    return _viewport ? static_cast<GuiViewportData*>(_viewport->RendererUserData) : nullptr;
}

struct ImGUIData {
    static constexpr EPixelFormat s_supported_formats[] =
        {PF_R8G8B8A8_SRGB, PF_R8G8B8A8_UNORM, PF_B8G8R8A8_SRGB, PF_B8G8R8A8_UNORM};
    UnorderedMap<EPixelFormat, GUIPipelineBdls> rast_psos;

    TextureRef font_texture;
    Array<BufferRef> retired_buffers;
    // Render buffers for main window
    uint32_t num_frames_in_flight;

    ImGUIRenderBackend* render_backend = nullptr;
    ImGUIData() {}
};

static void RetireGuiBuffer(ImGUIData& data, BufferRef& buffer) {
    if (buffer) {
        data.retired_buffers.emplace_back(std::move(buffer));
    }
    buffer = nullptr;
}

static void RetireRenderBuffers(ImGUIData& data, GuiFrameRenderBuffers& render_buffers) {
    RetireGuiBuffer(data, render_buffers.vtx_buffer);
    RetireGuiBuffer(data, render_buffers.idx_buffer);
    RetireGuiBuffer(data, render_buffers.arg_buffer);
}
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
            AddFont({"msyh.ttc", 20.0f, EFontType::Chinese});
            AddFont({FONT_ICON_FILE_NAME_FAS, 24.0f, EFontType::Icon});
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
        } else {
            LOG_INFO(
                MOER_TEXT("No existing imgui.ini file found, loading preset config: {}"),
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

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiViewport*   main_viewport      = ImGui::GetMainViewport();
    GuiViewportData* viewport_data      = MoerNew(GuiViewportData)(max_frame_in_flight);
    render_backend_data->render_backend = this;
    main_viewport->RendererUserData     = viewport_data;

    using namespace Moer::Render;
    auto& rd_device = Moer::Render::RenderDevice::Get();
    {
        Array<std::byte> transparent_pixels(256);
        transparent_texture = rd_device.CreateTexture(
            MOER_TEXT("ImGUI::TransparentTexture"),
            Extent2D(1, 1),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED
        );
        FenceRef upload_fence = rd_device.CreateFence();

        CommandList cmd_list;
        cmd_list.CopyFrom(std::span<std::byte>(transparent_pixels.data(), transparent_pixels.size()), transparent_texture);
        cmd_list.Signal(upload_fence, 1);

        Array<CommandList> upload_cmd_lists{};
        upload_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(upload_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        upload_fence->Wait(1);
        rd_device.GetCommandQueue(EQueueType::Graphics).Sync();
        transparent_texture_handle = bindless_array->AllocateTexture(
            transparent_texture,
            Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
        );
        registered_images.try_emplace(transparent_texture, transparent_texture_handle);
    }

    uint8_t* pixels;

    int width, height;
    //MARK... this is freaking slow, it's build first called, we need a default data for it, and async load other fonts
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    //upload texture
    {
        const uint32_t alignment    = 256;
        uint32_t       upload_pitch = Moer::AlignUp(width * 4, alignment);
        uint32_t       upload_size  = height * upload_pitch;
        TextureRef     font_tex =
            rd_device.CreateTexture(
                MOER_TEXT("ImGUI::FontTexture"),
                Extent2D(width, height),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::SAMPLED
            );
        FenceRef       upload_fence = rd_device.CreateFence();

        CommandList cmd_list;
        cmd_list.CopyFrom(std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);
        cmd_list.Signal(upload_fence, 1);

        Array<CommandList> upload_cmd_lists{};
        upload_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(upload_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        upload_fence->Wait(1);
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

static void EndPlatformFrameIfNeeded() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && context->FrameCountEnded == context->FrameCount &&
        context->FrameCountPlatformEnded < context->FrameCount) {
        ImGui::UpdatePlatformWindows();
    }
}

ImGUIRenderBackend::~ImGUIRenderBackend() {
    ImGuiIO&   io   = ImGui::GetIO();
    ImGUIData* data = GetGUIBackendData();

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::DestroyPlatformWindows();
        ImGui::GetPlatformIO().ClearRendererHandlers();
    }

    {
        //delete main viewport data
        ImGuiViewport*   main_viewport = ImGui::GetMainViewport();
        GuiViewportData* viewport_data = (GuiViewportData*)main_viewport->RendererUserData;
        if (data && viewport_data) {
            for (GuiFrameRenderBuffers& render_buffers : viewport_data->render_buffers) {
                RetireRenderBuffers(*data, render_buffers);
            }
        }
        MoerDelete(viewport_data);
        main_viewport->RendererUserData = nullptr;
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    device.WaitIdle();

    io.BackendRendererName     = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    bindless_array = nullptr;
    MoerDelete(data);
}
void ImGUIRenderBackend::BeginGUIFrame() {
    ImGui_ImplGlfw_NewFrame();

    ImGui::NewFrame();
    input_snapshot = CaptureImGuiIOInput();
}

void ImGUIRenderBackend::EndGUIFrame() {
    ImGui::Render();
    EndPlatformFrameIfNeeded();
}

const ImGuiIOInputSnapshot& ImGUIRenderBackend::GetInputSnapshot() const {
    return input_snapshot;
}

static void PrepareDrawDataTextures(ImGUIRenderBackend& render_backend, ImDrawData* draw_data) {
    if (draw_data == nullptr) {
        return;
    }

    for (int list_idx = 0; list_idx < draw_data->CmdListsCount; ++list_idx) {
        const ImDrawList* draw_list = draw_data->CmdLists[list_idx];
        for (const ImDrawCmd& cmd : draw_list->CmdBuffer) {
            render_backend.ResolveTextureHandle(static_cast<uint64_t>(cmd.GetTexID()));
        }
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

void ImGUIRenderBackend::RenderGUI(CommandList& _cmd_list, const TextureView& _framebuffer) {
    ImGuiIO& io = ImGui::GetIO();
    DrainRenderOutputUpdates();

    // if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
    //     return;

    // if (!draw_data)
    //     return;

    ImDrawData* main_draw_data = ImGui::GetDrawData();
    const bool  b_window_minimized =
        main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

    if (!b_window_minimized) {
        PrepareDrawDataTextures(*this, main_draw_data);
        _cmd_list.UpdateBindlessArray(bindless_array);
        auto& rd_device = RenderDevice::Get();
        GUIRender(main_draw_data, _framebuffer, _cmd_list);
    }
    EndPlatformFrameIfNeeded();
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

UIRenderer::RenderOutputSlotHandle ImGUIRenderBackend::RegisterRenderOutputSlot(uint32_t imgui_id) {
    std::lock_guard lock(render_output_mutex);

    if (auto iter = render_output_slots_by_id.find(imgui_id); iter != render_output_slots_by_id.end()) {
        return iter->second;
    }

    const uint32_t slot_index = static_cast<uint32_t>(render_output_generations.size());
    UIRenderer::RenderOutputSlotHandle handle{
        .slot_index = slot_index,
        .generation = 1,
    };
    render_output_slots_by_id.emplace(imgui_id, handle);
    render_output_generations.push_back(handle.generation);
    render_output_snapshot.resize(render_output_generations.size());
    return handle;
}

uint64_t ImGUIRenderBackend::GetRenderOutputTextureId(UIRenderer::RenderOutputSlotHandle handle) const {
    return EncodeRenderOutputTextureId(handle);
}

void ImGUIRenderBackend::PublishRenderOutput(
    UIRenderer::RenderOutputSlotHandle handle,
    TextureView                        resource
) {
    if (!handle.IsValid()) {
        return;
    }

    std::lock_guard lock(render_output_mutex);
    pending_render_output_updates.push_back(PendingRenderOutputUpdate{
        .handle   = handle,
        .resource = resource,
    });
}

void ImGUIRenderBackend::DrainRenderOutputUpdates() {
    std::lock_guard lock(render_output_mutex);

    if (render_output_snapshot.size() < render_output_generations.size()) {
        render_output_snapshot.resize(render_output_generations.size());
    }

    for (const PendingRenderOutputUpdate& update : pending_render_output_updates) {
        const auto handle = update.handle;
        if (!handle.IsValid() || handle.slot_index >= render_output_generations.size()) {
            continue;
        }
        if (render_output_generations[handle.slot_index] != handle.generation) {
            continue;
        }
        render_output_snapshot[handle.slot_index] = RenderOutputResource{
            .generation = handle.generation,
            .resource   = update.resource,
        };
    }
    pending_render_output_updates.clear();
}

uint ImGUIRenderBackend::ResolveTextureHandle(uint64_t texture_id) {
    if (!IsRenderOutputTextureId(texture_id)) {
        return static_cast<uint>(texture_id);
    }

    const UIRenderer::RenderOutputSlotHandle handle = DecodeRenderOutputTextureId(texture_id);
    if (!handle.IsValid() || handle.slot_index >= render_output_snapshot.size()) {
        return transparent_texture_handle;
    }

    const RenderOutputResource& output = render_output_snapshot[handle.slot_index];
    if (output.generation != handle.generation || !output.resource.GetTexture()) {
        return transparent_texture_handle;
    }

    auto iter = registered_images.try_emplace(output.resource.GetTexture(), 0);
    if (iter.second) {
        iter.first->second = bindless_array->AllocateTexture(
            output.resource,
            Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
        );
    }
    return iter.first->second;
}

} // namespace Moer::Render

void DestroyRenderBuffers(ImGUIData& _backend_data, GuiFrameRenderBuffers* _render_buffers);

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

    GuiFrameRenderBuffers* render_buffers =
        &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];
    viewport_data->frame_index++;

    if (render_buffers->vtx_buffer == nullptr ||
        render_buffers->vtx_buffer->GetNumElement() < total_size_vert) {
        RetireGuiBuffer(backend_data, render_buffers->vtx_buffer);
        uint32_t new_size          = 4096 + total_size_vert;
        render_buffers->vtx_buffer = device.CreateBuffer<ImDrawVert>(
            MOER_TEXT("GUI::ImGUI Vertex Buffer"),
            new_size,
            EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }
    if (render_buffers->idx_buffer == nullptr ||
        render_buffers->idx_buffer->GetNumElement() < total_size_idx) {
        RetireGuiBuffer(backend_data, render_buffers->idx_buffer);
        uint32_t new_size          = 8192 + total_size_idx;
        render_buffers->idx_buffer = device.CreateBuffer<ImDrawIdx>(
            MOER_TEXT("GUI::ImGUI Index Buffer"),
            new_size,
            EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST
        );
    }

    size_t            vertex_offset = 0;
    size_t            index_offset  = 0;
    Array<ImDrawVert> vertices(draw_data->TotalVtxCount);
    Array<ImDrawIdx>  indices(draw_data->TotalIdxCount);
    uint              total_cmd_cnt = 0;

    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        memcpy(
            vertices.data() + vertex_offset,
            cmd_list->VtxBuffer.Data,
            cmd_list->VtxBuffer.Size * sizeof(ImDrawVert)
        );
        memcpy(
            indices.data() + index_offset,
            cmd_list->IdxBuffer.Data,
            cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx)
        );
        vertex_offset += cmd_list->VtxBuffer.Size;
        index_offset += cmd_list->IdxBuffer.Size;
        total_cmd_cnt += cmd_list->CmdBuffer.Size;
    }
    Array<ImGUIArg> args;
    args.reserve(total_cmd_cnt);
    if (render_buffers->arg_buffer == nullptr ||
        render_buffers->arg_buffer->GetNumElement() < total_cmd_cnt) {
        RetireGuiBuffer(backend_data, render_buffers->arg_buffer);
        uint32_t new_size = 128 + total_cmd_cnt;
        render_buffers->arg_buffer = device.CreateBuffer<ImGUIArg>(
            MOER_TEXT("GUI::ImGUI Arg Buffer"), new_size, EBufferUsageFlags::TRANSFER_DST
        );
    }

    ImVec2 clip_off = draw_data->DisplayPos; // (0,0) unless using multi-viewports
    ImVec2 clip_scale =
        draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    Array<MeshDrawData> draw_meshes;
    VertexBuffer        vtx_buffers[] = {{render_buffers->vtx_buffer, 0}};
    IndexBuffer         idx_buffer = {render_buffers->idx_buffer->GetView(), EIndexElementType::IET_UINT16};
    draw_meshes.emplace_back(std::span<VertexBuffer>(vtx_buffers, 1), idx_buffer);

    MeshDrawData& batch = draw_meshes[0];

    batch.Reserve(total_cmd_cnt);

    int32_t global_vertex_offset = 0, global_index_offset = 0;

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

        memcpy(static_cast<void*>(&constant.mvp), mvp, sizeof(mvp));
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
                ImVec2 clip_min(
                    (cmd->ClipRect.x - clip_off.x) * clip_scale.x,
                    (cmd->ClipRect.y - clip_off.y) * clip_scale.y
                );
                ImVec2 clip_max(
                    (cmd->ClipRect.z - clip_off.x) * clip_scale.x,
                    (cmd->ClipRect.w - clip_off.y) * clip_scale.y
                );
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                uint texture_handle = render_backend.ResolveTextureHandle(static_cast<uint64_t>(cmd->GetTexID()));
                // arg_buffer[cmd_offset].min_xy = {clip_min.x, clip_min.y};
                // arg_buffer[cmd_offset].max_xy = {clip_max.x, clip_max.y};
                args.emplace_back(
                    ImVec2{clip_min.x, clip_min.y}, ImVec2{clip_max.x, clip_max.y}, texture_handle
                );

                uint32_t elem_count = cmd->ElemCount;
                uint32_t vtx_offset = cmd->VtxOffset + global_vertex_offset;
                uint32_t idx_offset = cmd->IdxOffset + global_index_offset;

                batch.EmplaceDrawIndexed(idx_offset, elem_count, vtx_offset, cmd_offset);
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
    _cmdlist.CopyFrom(
        std::span<Moer::byte>((Moer::byte*)vertices.data(), vertices.size() * sizeof(ImDrawVert)), vtx_view
    );
    _cmdlist.CopyFrom(
        std::span<Moer::byte>((Moer::byte*)indices.data(), indices.size() * sizeof(ImDrawIdx)), idx_view
    );
    _cmdlist.CopyFrom(
        std::span<Moer::byte>((Moer::byte*)args.data(), args.size() * sizeof(ImGUIArg)), arg_view
    );
    // _cmdlist.CopyFrom(arg_view, std::span<Moer::byte>((Moer::byte*)copy_back_args.data(), copy_back_args.size() * sizeof(ImGUIArg)));
    assert(backend_data.rast_psos.contains(_frame_buffer.format) && "Unsupported GUI format");
    _cmdlist
        .Gfx(
            backend_data.rast_psos[_frame_buffer.format],
            render_buffers->arg_buffer,
            render_backend.bindless_array,
            constant
        )
        .Draw(MOER_TEXT("ImGui Draws"),
            {0,
             0,
             (uint)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x),
             uint(draw_data->DisplaySize.y * draw_data->FramebufferScale.y)},
            std::move(draw_meshes),
            ColorAttachment(_frame_buffer.GetTexture(), EAttachmentAction::AC_LOAD_STORE)
        );

    _cmdlist.AddCallback([vtx(std::move(vertices)), idx(std::move(indices)), arg(std::move(args))
                          //   copy_back_args(std::move(copy_back_args))
    ]() {});
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

void DestroyRenderBuffers(ImGUIData& _backend_data, GuiFrameRenderBuffers* _render_buffers) {
    RetireRenderBuffers(_backend_data, *_render_buffers);
}

void GuiCreateWindow(ImGuiViewport* _viewport) {
    ImGUIData&       backend_data  = *GetGUIBackendData();
    GuiViewportData* viewport_data = MoerNew(GuiViewportData)(backend_data.num_frames_in_flight);
    using namespace Moer::Render;
    ImGUIRenderBackend& render_backend = *backend_data.render_backend;
    RenderDevice&       rd_device      = render_backend.device;

    _viewport->RendererUserData = viewport_data;

    if (_viewport->Size.x <= 0.0f || _viewport->Size.y <= 0.0f) {
        return;
    }

    Moer::WindowHandle handle{(Moer::WindowType*)(_viewport->PlatformHandle ? _viewport->PlatformHandle :
                                                                              _viewport->PlatformHandleRaw)};
    viewport_data->surface = MakeUnique<PresentationSurface>(
        rd_device,
        PresentationSurfaceDesc{
            .window            = handle,
            .size              = {(uint)_viewport->Size.x, (uint)_viewport->Size.y},
            .back_buffer_count = backend_data.num_frames_in_flight,
            .preferred_format  = PF_R8G8B8A8_SRGB,
            .debug_name        = Printf(MOER_TEXT("ImGui Window {}"), viewport_data->viewport_index),
        }
    );
    viewport_data->surface->EnsureFrameBuffer(
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );
}

void GuiDestroyWindow(ImGuiViewport* _viewport) {
    ImGUIData& backend_data = *GetGUIBackendData();
    using namespace Moer::Render;
    using namespace Moer;

    if (GuiViewportData* viewport_data = (GuiViewportData*)_viewport->RendererUserData) {
        if (viewport_data->surface) {
            viewport_data->surface->Sync();
            viewport_data->surface = nullptr;
        }
        for (uint32_t i = 0; i < backend_data.num_frames_in_flight; i++)
            DestroyRenderBuffers(backend_data, &viewport_data->render_buffers[i]);
        MoerDelete(viewport_data);
    }
    _viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* _viewport, ImVec2 _size) {
    GuiViewportData* viewport_data = GetGuiViewportData(_viewport);
    if (!viewport_data) {
        return;
    }

    auto& rd_device = GetGUIBackendData()->render_backend->device;
    if (_size.x <= 0.0f || _size.y <= 0.0f)
        return;

    const Extent2D new_size{(uint)_size.x, (uint)_size.y};
    if (viewport_data->surface && viewport_data->surface->GetSize().x == new_size.x &&
        viewport_data->surface->GetSize().y == new_size.y) {
        return;
    }

    if (!viewport_data->surface) {
        Moer::WindowHandle handle{(Moer::WindowType*)(_viewport->PlatformHandle ? _viewport->PlatformHandle :
                                                                                  _viewport->PlatformHandleRaw)};
        viewport_data->surface = MakeUnique<PresentationSurface>(
            rd_device,
            PresentationSurfaceDesc{
                .window            = handle,
                .size              = new_size,
                .back_buffer_count = GetGUIBackendData()->num_frames_in_flight,
                .preferred_format  = PF_R8G8B8A8_SRGB,
                .debug_name        = Printf(MOER_TEXT("ImGui Window {}"), viewport_data->viewport_index),
            }
        );
    } else {
        viewport_data->surface->Resize(new_size);
    }
    viewport_data->surface->EnsureFrameBuffer(
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );
}
void GuiRenderWindow(ImGuiViewport* _viewport, void* _cmd_list) {
    GuiViewportData* viewport_data = GetGuiViewportData(_viewport);
    if (!viewport_data || !viewport_data->surface || _viewport == nullptr || _cmd_list == nullptr) {
        return;
    }

    TextureView frame_buffer = viewport_data->surface->GetFrameBufferView();
    if (!frame_buffer.GetTexture())
        return;
    GUIRender(_viewport->DrawData, frame_buffer, *(CommandList*)(_cmd_list));
    // gfx_queue.Execute(std::move(cmd_list.Submit()));
    // gfx_queue.Sync();
}

void GuiSwapbuffer(ImGuiViewport* _viewport, void*) {
    GuiViewportData* viewport_data = GetGuiViewportData(_viewport);
    if (!viewport_data || !viewport_data->surface) {
        return;
    }

    TextureView frame_buffer = viewport_data->surface->GetFrameBufferView();
    if (!frame_buffer.GetTexture()) {
        return;
    }

    RHIPresentRequest present_request = viewport_data->surface->CreatePresentRequest(frame_buffer);

    ImDrawData* draw_data = _viewport ? _viewport->DrawData : nullptr;
    const bool has_draw_data = draw_data != nullptr && draw_data->DisplaySize.x > 0.0f &&
                               draw_data->DisplaySize.y > 0.0f && draw_data->CmdListsCount > 0;

    if (!has_draw_data) {
        viewport_data->surface->Sync();
        Array<CommandList> present_cmd_lists{};
        RHIExecutor::Get().Submit(
            std::move(present_cmd_lists), ERHIExecSubmitFlags::FlushGPU, &present_request
        );
        return;
    }

    CommandList cmd_list(EQueueType::Graphics);
    cmd_list.UpdateBindlessArray(GetGUIBackendData()->render_backend->bindless_array);
    GuiRenderWindow(_viewport, &cmd_list);
    cmd_list.DeleteResources();

    Array<CommandList> present_cmd_lists{};
    present_cmd_lists.emplace_back(std::move(cmd_list));
    RHIExecutor::Get().Submit(
        std::move(present_cmd_lists), ERHIExecSubmitFlags::FlushGPU, &present_request
    );
}
