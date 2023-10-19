#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMap.h"
#include "shader/ShaderResource.h"
#include <array>
#include <unordered_map>
#include <vector>
class ShaderResourceManager {
public:
    static void                   Init(EShaderPlatform platform);
    static void                   ShutDown();
    static ShaderResourceManager& GetInstance();

    template<typename ShaderType>
    static Shader* GetShader() {
        const ShaderMetaType& meta_type = ShaderType::GetMetaType();
        return GetInstance().GetShader(meta_type);
    }

    ShaderCodeResourceMap& GetShaderCodeMap(EShaderPlatform _platform) {
        return *code_resources;
    }
    ShaderTypeResourceMap& GetShaderTypeMap(EShaderPlatform _platform) {
        return *type_resources;
    }

    void PrepareGlobalShaderResources();

private:
    Shader* GetShader(const ShaderMetaType& _meta_type);
    ShaderResourceManager();
    ShaderCodeResourceMap* code_resources;
    ShaderTypeResourceMap* type_resources;
};
#endif