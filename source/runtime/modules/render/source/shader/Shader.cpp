#include "shader/Shader.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
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
      compiled_hash(initializer.compiled_hash){
    //truncated hashkey for other usages
    memcpy(&hash_key, &compiled_hash, sizeof(hash_key));
    ConstructRootParameterLayoutInfo(initializer.parameter_map);
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

void Shader::ConstructRootParameterLayoutInfo(const ShaderParametersInfoMap& _param_map){
    const auto& parameter_meta_data = type->GetParameterMetaData();
    std::vector<ShaderParameterLayoutInfo> layout_infos;
    const auto& reflect_map = _param_map.GetShaderParameterInfoMap();
    for(const auto& member : parameter_meta_data->GetMembers()){
        int16_t slot = -1, space = -1, num = 0;EShaderParameterType param_type = EShaderParameterType::UNKNOWN;
        bool b_valid = reflect_map.count(member.GetName()) > 0;
        if(b_valid){
            const auto& iter = reflect_map.find(member.GetName());
            slot = iter->second.slot;
            space = iter->second.space;
            num = iter->second.num;
            param_type = iter->second.type;
        }
        uint32_t step = 0;
        step = (num > 0 ? (member.GetStride() / num) : member.GetStride());
        for (size_t i = 0; i < ((num==0)? 1: num); ++i) {
            layout_infos.emplace_back(ShaderParameterLayoutInfo(member.GetOffset() + step * i,
            step,
            slot++,
            space,
            param_type
            ));
        }

        
    }
    param_layout_info.layout_infos.swap(layout_infos);

}