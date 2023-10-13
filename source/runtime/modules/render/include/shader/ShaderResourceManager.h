#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "rhi/RHIResource.h"
#include "shader/ShaderMap.h"
class ShaderResourceManager {
public:
    static ShaderResourceManager& GetInstance();
    void                          PrepareGlobalShaderResources();
    void                          UpdateGlobalShaderResources();

    template<typename ShaderType>
    static RHIShaderRef GetGlobalShader() {
    }
};
#endif