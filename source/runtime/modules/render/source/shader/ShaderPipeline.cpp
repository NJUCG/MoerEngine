#include "resources/GpuScene.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderResourceManager.h"
#include <shader/ShaderPipeline.h>

namespace Moer::Render {
    void VertexFactory::SetCompileEnvironment(ShaderCompilerEnvironment& _env) {
        if (VertexAttributesTool::HasAttribute(mask, EVertexAttributes::VA_TANGENT)) {
            _env.SetDefine("HAS_TANGENT", 1);
        }
        if (VertexAttributesTool::HasAttribute(mask, EVertexAttributes::VA_NORMAL)) {
            _env.SetDefine("HAS_NORMAL", 1);
        }
        if (VertexAttributesTool::HasAttribute(mask, EVertexAttributes::VA_TEXCOORD0)) {
            _env.SetDefine("HAS_TEXCOORD0", 1);
        }
        _env.SetDefine("SHADOW_DEPTH_PASS", IsShadowDepthPass() ? 1 : 0);
    }

    const VertexStream& VertexFactory::GetVertexStream() const {
        if (stream.bindings.empty() && mask != 0) {
            // Initialize the vertex stream
            const auto& attrs = VertexAttributesTool::GetArrayFromBitmask(mask);
            for (const auto& attr : attrs) {
                const auto& pixel_format = VertexAttributesTool::GetPixelFormat(attr);
                stream.EmplacePerVertex({Moer::Render::VertexElement(pixel_format)});
            }
            return stream;
        }
        return stream;
    }

    VertexFactory::VertexFactory(VertexAttributesBitmask _flags, bool _is_shadow_depth_pass) : mask(_flags), is_shadow_depth_pass(_is_shadow_depth_pass) {
    }

    Shader& VertexShader::GetShader(Moer::Render::VertexFactory* _factory) {
        if (auto iter = shader_map.find(*_factory); iter != shader_map.end()) {
            return iter->second;
        }
        return shader_map.emplace(*_factory, ShaderManager::Get().CompileVertexShader(shader_path, _factory, entry_name, src_environment, mutation_id)).first->second;
    }
}// namespace Moer::Render