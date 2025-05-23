#pragma once

#include "misc/Traits.h"

#include <toml++/toml.hpp>

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
    struct GlobalConfig {

        struct Editor {
            uint width;
            uint height;
            bool fullscreen;
            bool vsync;

            bool  lock_frame_rate;
            uint  fps;
            uint  max_fps;
            float font_size;
        } editor;

        struct Engine {
            struct RHI {
                std::string rhi;
                uint        max_frame_in_flight;

                struct Vulkan {
                    std::string api_version;
                } vulkan;

                struct D3d12 {
                    bool allow_pix_attach;
                } d3d12;
            } rhi;

            struct Render {
                std::string default_render_method;
            } render;

            struct Scene {
                std::string scene_path;
            } scene;

        } engine;

        static GlobalConfig LoadConfigFromTomlFile(const std::string_view& toml_path) {
            GlobalConfig c;

            auto config = toml::parse_file(toml_path.data());

            // Editor

            c.editor.width      = config.at_path("editor.width").value_or(1920);
            c.editor.height     = config.at_path("editor.height").value_or(1080);
            c.editor.fullscreen = config.at_path("editor.fullscreen").value_or(false);
            c.editor.vsync      = config.at_path("editor.vsync").value_or(false);

            c.editor.lock_frame_rate = config.at_path("editor.lock_frame_rate").value_or(false);
            c.editor.fps             = config.at_path("editor.fps").value_or(60);
            c.editor.max_fps         = config.at_path("editor.max_fps").value_or(170);
            c.editor.font_size       = config.at_path("editor.font_size").value_or(16.f);

            // Engine

            c.engine.rhi.rhi                 = config.at_path("engine.rhi.rhi").value_or("vulkan");
            c.engine.rhi.max_frame_in_flight = config.at_path("engine.rhi.max_frame_in_flight").value_or(3);
            c.engine.rhi.vulkan.api_version  = config.at_path("engine.rhi.vulkan.api_version").value_or("1.3");

            c.engine.rhi.d3d12.allow_pix_attach = config.at_path("engine.rhi.d3d12.allow_pix_attach").value_or(false);

            c.engine.render.default_render_method = config.at_path("engine.render.default_render_method").value_or("Raster");

            c.engine.scene.scene_path = config.at_path("engine.scene.scene_path").value_or("./asset/scenes/sponza/Sponza.gltf");

            return c;
        }
    };

}// namespace Moer::Config
