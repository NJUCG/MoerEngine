#ifndef MOERENGINE_SHADER_MAP_H
#define MOERENGINE_SHADER_MAP_H
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include <array>

//Compile

class ShaderResourceMap {
};
class GlobalShaderMap {
    friend class ShaderResourceManager;

public:
private:
    template<typename TGlobalShader>
    RHIShader GetRHIShader() {
        LOG_ERROR("RHI not intialized");
        assert(g_rhi && "RHI not initialzed");

        EShaderPlatform platform = GetShaderPlatformByRHIType(g_rhi->GetType());
        const Shader*   shader   = GetShader(TGlobalShader::GetMetaType());
    }

    const Shader* GetShader(const ShaderMetaType* _shader_meta_type);

    std::array<ShaderResourceMap, EShaderPlatform::SP_Num> resources_maps;
};
#endif