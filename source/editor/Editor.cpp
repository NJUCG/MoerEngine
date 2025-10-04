#include "Editor.h"

#include "Engine.h"
#include "config/ConfigManager.h"

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
    // init
    auto             ui_renderer = MakeUnique<Render::UIRenderer>(RenderDevice::Get());
    SharedPtr<uint2> resolution  = m_engine->GetResolution();

    auto editor_ui = MakeUnique<EditorUI>(std::move(ui_renderer), resolution, m_engine->GetEditorConfig());

    // run
    m_engine->Run(
        EngineHooks{
            // Common
            .on_tick_ui =
                [&editor_ui]() {
                    editor_ui->TickUI();
                },
            .on_render_gui =
                [&editor_ui](CommandList& cmd_list, TextureRef output_image) {
                    editor_ui->RenderGUI(cmd_list, output_image);
                },
            .on_present_windows =
                [&editor_ui]() {
                    editor_ui->PresentWindows();
                },
            .on_is_need_reload =
                [&editor_ui]() {
                    return editor_ui->IsNeedReload();
                },
            .on_ui_combine_pass =
                [&editor_ui](
                    UiCombinePass* ui_combine_pass,
                    CommandList&   cmd_list,
                    TextureView    input_color_texture,
                    TextureView    input_ui_texture, // TODO: is this necessary?
                    TextureView    default_output_texture
                ) {
                    return ui_combine_pass->Process(
                        cmd_list,
                        editor_ui->IsSeperateWindow(),
                        editor_ui->GetConfig()->resolution,
                        editor_ui->GetSceneColorPos(),
                        editor_ui->GetSceneColorResolution(),
                        editor_ui->GetWindowFrameBuffer(),
                        input_color_texture,
                        input_ui_texture,
                        default_output_texture
                    );
                },
            .on_register_ui_func =
                [&editor_ui](std::string name, std::function<void(void)> lambda) {
                    editor_ui->RegisterUIFunc(name, std::move(lambda));
                },
            .on_unregister_ui_func =
                [&editor_ui](std::string name) {
                    editor_ui->UnregisterUIFunc(name);
                },
            // Raster
            .on_raster_register_frame_buffers =
                [&editor_ui](const Array<TextureView>& textures) {
                    editor_ui->m_raster_ui.RegisterFrameBuffers(textures);
                }
        }
    );

    // release
    m_editor_ui.reset(); // 释放EditorUI资源
}

void Editor::ShutDown() {

    m_engine.reset();
}

} // namespace Moer