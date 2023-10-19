#include "shader/Shader.h"
#include "misc/Hash.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"
#include <vcruntime_typeinfo.h>

class TestShaderClass : Shader {
    DEFINE_SHADER_TYPE(TestShaderClass, Global, RENDER_CORE_API)
};

IMPLEMENT_SHADER_TYPE(TestShaderClass, "shader/testVert.vert", "main", EShaderType::ST_VERTEX)
IMPLEMENT_SHADER_TYPE(TestReflectionShader, "TestVert.vert", "main", EShaderType::ST_VERTEX);

Shader::Shader(){

};

Shader::Shader(const ShaderCompiledInitializer& initializer)
    : type(initializer.type_info),
      target_info(initializer.target_info),
      code_size(initializer.code_size),
      compiled_hash(initializer.compiled_hash),
      param_map(initializer.parameter_map) {
    //truncated hashkey for other usages
    memcpy(&hash_key, &compiled_hash, sizeof(hash_key));
};

Shader::~Shader(){

};

const ShaderCodeEntry* Shader::GetCodeEntry() const {
    if (type != nullptr) {
        return ShaderResourceManager::GetInstance().GetShaderCodeMap().GetCodeEntry(type->GetName());
    }
    return nullptr;
}

const Hash64City& Shader::GetCompiledHash() const {
    return compiled_hash;
};