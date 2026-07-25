// 将 MoerEngine.toml 中的字段加载到强类型全局配置结构中。
#include "config/GlobalConfig.h"

#include <toml++/toml.hpp>

namespace Moer::Config {

GlobalConfig GlobalConfig::LoadConfigFromTomlFile(const std::string_view& toml_path) {
    GlobalConfig loaded_config{};

    const auto toml_config = toml::parse_file(toml_path.data());

    // TODO: 后续可根据 GlobalConfig 的结构自动生成以下字段加载代码。

    // 编辑器配置

    loaded_config.editor.width      = toml_config.at_path("editor.width").value_or(1920);
    loaded_config.editor.height     = toml_config.at_path("editor.height").value_or(1080);
    loaded_config.editor.fullscreen = toml_config.at_path("editor.fullscreen").value_or(false);
    loaded_config.editor.vsync      = toml_config.at_path("editor.vsync").value_or(false);

    loaded_config.editor.lock_frame_rate =
        toml_config.at_path("editor.lock_frame_rate").value_or(false);
    loaded_config.editor.fps       = toml_config.at_path("editor.fps").value_or(60);
    loaded_config.editor.max_fps   = toml_config.at_path("editor.max_fps").value_or(170);
    loaded_config.editor.font_size = toml_config.at_path("editor.font_size").value_or(16.f);

    loaded_config.editor.preset_imgui_config_path =
        toml_config.at_path("editor.preset_imgui_config_path").value_or("./asset/preset_imgui.ini");

    // 引擎配置

    loaded_config.engine.threading.render_thread =
        toml_config.at_path("engine.threading.render_thread").value_or(false);
    loaded_config.engine.threading.rhi_thread =
        toml_config.at_path("engine.threading.rhi_thread").value_or(false);
    loaded_config.engine.threading.rhi_bypass =
        toml_config.at_path("engine.threading.rhi_bypass").value_or(true);
    loaded_config.engine.threading.profile_logging =
        toml_config.at_path("engine.threading.profile_logging").value_or(false);
    loaded_config.engine.threading.parallel_recording =
        toml_config.at_path("engine.threading.parallel_recording").value_or(false);
    loaded_config.engine.threading.parallel_record_workers =
        toml_config.at_path("engine.threading.parallel_record_workers").value_or(uint{0});
    loaded_config.engine.threading.parallel_record_verify =
        toml_config.at_path("engine.threading.parallel_record_verify").value_or(false);
    loaded_config.engine.threading.parallel_record_profile =
        toml_config.at_path("engine.threading.parallel_record_profile").value_or(false);
    loaded_config.engine.threading.parallel_record_min_work_units_per_job =
        toml_config.at_path("engine.threading.parallel_record_min_work_units_per_job")
            .value_or(uint{64});
    loaded_config.engine.threading.submission_batch_window =
        toml_config.at_path("engine.threading.submission_batch_window").value_or(uint{2});
    loaded_config.engine.threading.max_frame_lag =
        toml_config.at_path("engine.threading.max_frame_lag").value_or(uint{0});

    loaded_config.engine.rhi.type = toml_config.at_path("engine.rhi.type").value_or("Not Specified");
    loaded_config.engine.rhi.max_frame_in_flight =
        toml_config.at_path("engine.rhi.max_frame_in_flight").value_or(3);
    loaded_config.engine.rhi.api_version =
        toml_config.at_path("engine.rhi.api_version").value_or("1.3");

    loaded_config.engine.render.default_render_method =
        toml_config.at_path("engine.render.default_render_method").value_or("Raster");
    loaded_config.engine.render.raster.enable_shadow =
        toml_config.at_path("engine.render.raster.enable_shadow").value_or(true);
    loaded_config.engine.render.raster.low_quality_mode =
        toml_config.at_path("engine.render.raster.low_quality_mode").value_or(false);
    loaded_config.engine.render.raster.render_graph =
        toml_config.at_path("engine.render.raster.render_graph").value_or(false);
    loaded_config.engine.render.raster.render_graph_debug_dump =
        toml_config.at_path("engine.render.raster.render_graph_debug_dump").value_or(false);
    loaded_config.engine.render.raster.render_graph_parallel_recording =
        toml_config.at_path("engine.render.raster.render_graph_parallel_recording").value_or(false);
    loaded_config.engine.render.raytracing.render_graph =
        toml_config.at_path("engine.render.raytracing.render_graph").value_or(false);
    loaded_config.engine.render.raytracing.render_graph_debug_dump =
        toml_config.at_path("engine.render.raytracing.render_graph_debug_dump").value_or(false);
    loaded_config.engine.render.raytracing.render_graph_parallel_recording =
        toml_config.at_path("engine.render.raytracing.render_graph_parallel_recording").value_or(false);

    loaded_config.engine.scene.scene_path =
        toml_config.at_path("engine.scene.scene_path").value_or("./asset/scenes/sponza/Sponza.gltf");
    loaded_config.engine.scene.enable_cache =
        toml_config.at_path("engine.scene.enable_cache").value_or(true);

    loaded_config.engine.scene.material_info_log_lines =
        toml_config.at_path("engine.scene.material_info_log_lines").value_or(-1);

    loaded_config.engine.remote.enable =
        toml_config.at_path("engine.remote.enable").value_or(false);
    loaded_config.engine.remote.bind_address =
        toml_config.at_path("engine.remote.bind_address").value_or("127.0.0.1");
    loaded_config.engine.remote.http_port =
        toml_config.at_path("engine.remote.http_port").value_or(18080);
    loaded_config.engine.remote.websocket_port =
        toml_config.at_path("engine.remote.websocket_port").value_or(18081);

    return loaded_config;
}

} // namespace Moer::Config
