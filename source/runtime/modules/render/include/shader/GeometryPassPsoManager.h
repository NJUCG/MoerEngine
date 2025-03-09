#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "resources/vertexfactory/VertexAttributes.h"

namespace Moer::Render {

    struct GeometryPassBindlessParam {
        float4     color;
        uint       texture_handle;
        uint       buffer_handle;
        uint       instance_buffer_handle;
        uint       geometry_data_handle;
        uint       geometry_instance_handle;
        Matrix4x4f camera_view_proj;
    };

    class GeometryPassPipeline : public RasterPipeline {
    public:
        DEFINE_RASTER_PIPELINE_CLASS(GeometryPassPipeline);
        DEFINE_SHADER_BUFFER(view_param_buffer);
        DEFINE_SHADER_BINDLESS_ARRAY(bdls);
        DEFINE_SHADER_CONSTANT_STRUCT(GeometryPassBindlessParam, param);
        DEFINE_SHADER_ARGS(view_param_buffer, bdls, param);
    };

    struct GeometryPassPsoRecord {
        VertexAttributesBitmask   vertex_attributes_bitmask;
        std::string_view          vertex_shader_path;
        std::string_view          pixel_shader_path;
        std::string_view          vertex_shader_entry       = "main";
        std::string_view          pixel_shader_entry        = "main";
        ShaderCompilerEnvironment vertex_shader_environment = {};
        ShaderCompilerEnvironment pixel_shader_environment  = {};
    };

    // A singleton class to manage PSO for geometry pass
    class RENDER_API GeometryPassPsoManager {
    public:
        ~GeometryPassPsoManager();
        GeometryPassPsoManager(const GeometryPassPsoManager&)            = delete;
        GeometryPassPsoManager& operator=(const GeometryPassPsoManager&) = delete;

        // 获取单例并且自动初始化
        static GeometryPassPsoManager& Get();
        // 显式销毁单例
        static void ShutDown();

        GeometryPassPipeline& GetPso(const VertexAttributesBitmask& bitmask);

    private:
        GeometryPassPsoManager();

    private:
        class Impl;
        Impl* m_impl = nullptr;
    };

}// namespace Moer::Render