#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"
#include "resources/vertexfactory/VertexAttributes.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

namespace Moer::Render {

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
class RENDER_API [[deprecated]] GeometryPassPsoManager {
public:
    ~GeometryPassPsoManager();
    GeometryPassPsoManager(const GeometryPassPsoManager&)            = delete;
    GeometryPassPsoManager& operator=(const GeometryPassPsoManager&) = delete;

    // 获取单例并且自动初始化（线程安全）
    static GeometryPassPsoManager& Get();
    // 显式销毁单例（线程安全）
    static void ShutDown();

    // GeometryPassPipeline& GetGeometryPso(const VertexAttributesBitmask& bitmask);

    // ShadowDepthPassPipeline& GetShadowPso(const VertexAttributesBitmask& bitmask);

private:
    GeometryPassPsoManager();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

} // namespace Moer::Render