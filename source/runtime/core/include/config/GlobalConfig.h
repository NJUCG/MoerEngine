#pragma once

#include "API_Macro.h"
#include "misc/Traits.h"

// TODO: auto generate this file according to `MoerEngine.toml`

namespace Moer::Config {

/**
     * 更安全的Config
     * 简洁、安全、优雅
     * 
     * 如果需要修改配置文件定义，只需要修改：
     * 1. 本文件的数据结构
     * 2. 本文件的Loader
     * 3. 配置文件（即 MoerEngine.toml）
     * 
     * 本文件的数据类型尽量使用基本数据类型；如果需要进行数据转换，请在使用配置文件时进行转换。
     * 例如：在Editor.cpp中，将std::string default_render_method转换为ERenderMethod，而不是在本文件中定义ERenderMethod
     * 
     * 本文件只确保数据类型的正确性，不确保数据的合法性。
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
            bool render_thread = false;
            bool rhi_thread    = false;
            bool rhi_bypass    = true;
            uint max_frame_lag = 0;
        } threading;

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
            } raster;

        } render;

        struct Scene {
            std::string scene_path;
            bool        enable_cache;
            int         material_info_log_lines; // -1 to log all
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
