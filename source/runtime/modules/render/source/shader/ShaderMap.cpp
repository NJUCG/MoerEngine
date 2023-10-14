#include "shader/ShaderMap.h"
#include "misc/Hash.h"

const Shader* GlobalShaderMap::GetShader(const ShaderMetaType* _shader_meta_type) {

    assert(_shader_meta_type != nullptr);

    HashedName name = _shader_meta_type->GetName();
    return nullptr;
}