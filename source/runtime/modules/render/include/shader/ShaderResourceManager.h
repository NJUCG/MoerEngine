#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderMap.h"
#include "shader/ShaderResource.h"
#include <array>
#include <unordered_map>
class ShaderResourceManager {
public:
    static ShaderResourceManager& GetInstance();
    void                          PrepareGlobalShaderResources();
    void                          UpdateGlobalShaderResources();

    template<typename ShaderType>
    static RHIShaderRef GetGlobalShader() {
    }

    static ShaderCodeResourceMap& GetShaderCodeMap(EShaderPlatform _platform) {
        return code_resources[_platform];
    }

private:
    static std::array<ShaderCodeResourceMap, EShaderPlatform::SP_Num> code_resources;
};
#endif