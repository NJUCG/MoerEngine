// 定义与 MoerEngine.toml 配置项对应的强类型数据结构。
#pragma once

#include "API_Macro.h"
#include "misc/Traits.h"

// TODO: 后续可根据 MoerEngine.toml 自动生成本文件及其加载代码。

namespace Moer::Config {

/**
 * 配置结构仅负责保证字段类型正确，不负责校验字段值是否合法。
 *
 * 修改配置定义时，需要同步更新本结构、加载代码与 MoerEngine.toml。
 * 字段尽量使用基础类型；枚举等业务类型应由使用方负责转换，避免配置层依赖上层模块。
 */
struct CORE_API GlobalConfig {

    struct Editor {
        uint width;
        uint height;
        bool fullscreen;
        bool vsync;

        bool  lock_frame_rate;
        uint  fps;
        uint  max_fps;
        float font_size;

        std::string preset_imgui_config_path;
    } editor;

    struct Engine {
        struct Threading {
            bool render_thread           = false;
            bool rhi_thread              = false;
            bool rhi_bypass              = true;
            bool profile_logging         = false;
            bool parallel_recording      = false;
            uint parallel_record_workers = 0;
            bool parallel_record_verify  = false;
            bool parallel_record_profile = false;
            uint parallel_record_min_work_units_per_job = 64;
            uint submission_batch_window = 2;
            uint max_frame_lag           = 0;
        } threading;

        struct ProfileDump {
            bool        enabled          = false;
            std::string output_path      = "./profile/MoerProfile.mpd";
            bool        replace_existing = true;
        } profile_dump;

        struct RHI {
            std::string type;
            std::string api_version;
            uint        max_frame_in_flight;
        } rhi;

        struct Render {
            std::string default_render_method;

            struct Raster {
                bool enable_shadow;
                bool low_quality_mode;
                bool render_graph;
                bool render_graph_debug_dump;
                bool render_graph_parallel_recording;
            } raster;

            struct Raytracing {
                bool render_graph;
                bool render_graph_debug_dump;
                bool render_graph_parallel_recording;
            } raytracing;

        } render;

        struct Scene {
            std::string scene_path;
            bool        enable_cache;
            int         material_info_log_lines; // -1 表示输出全部材质日志
        } scene;

        struct Remote {
            bool        enable;
            std::string bind_address;
            uint        http_port;
            uint        websocket_port;
        } remote;

    } engine;

    static GlobalConfig LoadConfigFromTomlFile(const std::string_view& toml_path);
};

} // namespace Moer::Config
