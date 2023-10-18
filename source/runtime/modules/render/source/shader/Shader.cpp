#include "shader/Shader.h"
#include "misc/Hash.h"
#include "shader/ShaderCommon.h"

class GlobalShaderMap;
class TestShaderClass : Shader {
    DEFINE_SHADER_TYPE(TestShaderClass, Global, RENDER_CORE_API)
};

IMPLEMENT_SHADER_TYPE(TestShaderClass, "shader/testVert.vert", "main", EShaderType::ST_VERTEX)

Shader::Shader(){

};

Shader::Shader(const ShaderCompiledInitializer& intializer)
    : type(intializer.type_info),
      target_info(intializer.target_info),
      code_size(intializer.code_size) {
    //truncated hashkey for other usages
    memcpy(&hash_key, &compiled_hash, sizeof(hash_key));
};

Shader::~Shader(){

};

const Hash64City& Shader::GetCompiledHash() const {
    return compiled_hash;
};