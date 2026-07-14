#include "config/GlobalConfig.h"

#include <toml++/toml.hpp>

namespace Moer::Config {

GlobalConfig GlobalConfig::LoadConfigFromTomlFile(const std::string_view& toml_path) {
    GlobalConfig c{};

    auto config = toml::parse_file(toml_path.data());

    // TODO: auto generate the following code

    // Editor

    c.editor.width      = config.at_path("editor.width").value_or(1920);
    c.editor.height     = config.at_path("editor.height").value_or(1080);
    c.editor.fullscreen = config.at_path("editor.fullscreen").value_or(false);
    c.editor.vsync      = config.at_path("editor.vsync").value_or(false);

    c.editor.lock_frame_rate = config.at_path("editor.lock_frame_rate").value_or(false);
    c.editor.fps             = config.at_path("editor.fps").value_or(60);
    c.editor.max_fps         = config.at_path("editor.max_fps").value_or(170);
    c.editor.font_size       = config.at_path("editor.font_size").value_or(16.f);

    c.editor.preset_imgui_config_path =
        config.at_path("editor.preset_imgui_config_path").value_or("./asset/preset_imgui.ini");

    // Engine

    c.engine.threading.render_thread =
        config.at_path("engine.threading.render_thread").value_or(false);
    c.engine.threading.rhi_thread = config.at_path("engine.threading.rhi_thread").value_or(false);
    c.engine.threading.rhi_bypass = config.at_path("engine.threading.rhi_bypass").value_or(true);
    c.engine.threading.max_frame_lag =
        config.at_path("engine.threading.max_frame_lag").value_or(uint{0});

    c.engine.rhi.type =
        config.at_path("engine.rhi.type").value_or("Not Specified"); // Use this to warn user to set it
    c.engine.rhi.max_frame_in_flight = config.at_path("engine.rhi.max_frame_in_flight").value_or(3);
    c.engine.rhi.api_version         = config.at_path("engine.rhi.api_version").value_or("1.3");

    c.engine.render.default_render_method =
        config.at_path("engine.render.default_render_method").value_or("Raster");
    c.engine.render.raster.enable_shadow =
        config.at_path("engine.render.raster.enable_shadow").value_or(true);
    c.engine.render.raster.low_quality_mode =
        config.at_path("engine.render.raster.low_quality_mode").value_or(false);

    c.engine.scene.scene_path =
        config.at_path("engine.scene.scene_path").value_or("./asset/scenes/sponza/Sponza.gltf");
    c.engine.scene.enable_cache = config.at_path("engine.scene.enable_cache").value_or(true); // 默认启用cache

    c.engine.scene.material_info_log_lines =
        config.at_path("engine.scene.material_info_log_lines").value_or(-1); // -1 to log all

    c.engine.remote.enable         = config.at_path("engine.remote.enable").value_or(false);
    c.engine.remote.bind_address   = config.at_path("engine.remote.bind_address").value_or("127.0.0.1");
    c.engine.remote.http_port      = config.at_path("engine.remote.http_port").value_or(18080);
    c.engine.remote.websocket_port = config.at_path("engine.remote.websocket_port").value_or(18081);

    return c;
}

} // namespace Moer::Config
