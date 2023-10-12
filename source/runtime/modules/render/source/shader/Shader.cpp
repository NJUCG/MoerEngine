#include "shader/Shader.h"

class GlobalShaderMap;
class TestShaderClass : Shader {
    DEFINE_SHADER_TYPE(TestShaderClass, Global, RENDER_CORE_API)
};

IMPLEMENT_SHADER_TYPE(TestShaderClass, "shader/testVert.vert", "main", EShaderType::ST_VERTEX)
