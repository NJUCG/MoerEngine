#include "Editor.h"

#include "Engine.h"

#include "EditorUI.h"

#include <cassert>
#include <nfd.hpp>

// namespace
using namespace Moer::Render;

namespace Moer {

Editor::Editor() {}

Editor::~Editor() {}

void Editor::Init(int argc, const char** argv) {
    m_engine = MakeUnique<Engine>();
    m_engine->Init(argc, argv);
}

void Editor::Run() {
    Run(ExtraHooks{});
}

void Editor::Run(const ExtraHooks& extra_hooks) {
    // init
    m_editor_ui = MakeUnique<EditorUI>(
        MakeUnique<Render::UIRenderer>(RenderDevice::Get()),
        m_engine->GetEditorConfig(),
        m_engine->GetRemoteModuleController()
    );

    // run
    m_engine->Run(
        EngineHooks{
            // Common
            .on_tick_test = extra_hooks.on_tick_test,
            .on_tick_ui =
                [this](Scene& scene) {
                    m_editor_ui->TickUI(scene);
                },
            .on_render_gui =
                [this](CommandList& cmd_list, TextureRef output_image) {
                    m_editor_ui->RenderGUI(cmd_list, output_image);
                },
            .on_present_windows =
                [this]() {
                    m_editor_ui->PresentWindows();
                },
            .on_is_need_reload =
                [this]() {
                    return m_editor_ui->IsNeedReload();
                },
            .on_ui_combine_pass =
                [this](
                    UiCombinePass* ui_combine_pass,
                    CommandList&   cmd_list,
                    TextureView    input_color_texture,
                    TextureView    input_ui_texture, // TODO: is this necessary?
                    TextureView    default_output_texture
                ) {
                    return ui_combine_pass->Process(
                        cmd_list,
                        m_editor_ui->IsSeperateWindow(),
                        m_editor_ui->GetConfig()->GetResolution(),
                        m_editor_ui->GetSceneColorPos(),
                        m_editor_ui->GetSceneColorResolution(),
                        m_editor_ui->GetWindowFrameBuffer(),
                        input_color_texture,
                        input_ui_texture,
                        default_output_texture
                    );
                },
            .on_capture_ui_composition =
                [this]() {
                    return UiCompositionFrameData{
                        .enabled                = true,
                        .separate_window        = m_editor_ui->IsSeperateWindow(),
                        .output_resolution      = m_editor_ui->GetConfig()->GetResolution(),
                        .scene_color_position   = m_editor_ui->GetSceneColorPos(),
                        .scene_color_resolution = m_editor_ui->GetSceneColorResolution(),
                        .window_frame_buffer    = m_editor_ui->GetWindowFrameBuffer()
                    };
                },
            .on_register_ui_func =
                [this](std::string name, std::function<void(void)> lambda) {
                    m_editor_ui->RegisterUIFunc(name, std::move(lambda));
                },
            .on_unregister_ui_func =
                [this](std::string name) {
                    m_editor_ui->UnregisterUIFunc(name);
                },
            .on_show_config_sub_ui =
                [this]() {
                    m_editor_ui->SetShowRenderConfigSubUI(true);
                },

            // Raster
            .on_raster_register_frame_buffers =
                [this](const Array<TextureView>& textures) {
                    m_editor_ui->m_raster_ui.RegisterFrameBuffers(textures);
                }
        }
    );

    // release
    m_editor_ui.reset(); // 释放EditorUI资源
}

void Editor::ShutDown() {
    m_engine->ShutDown();
    m_engine.reset();
}

Engine& Editor::GetEngine() {
    return *m_engine;
}

const Engine& Editor::GetEngine() const {
    return *m_engine;
}

} // namespace Moer
