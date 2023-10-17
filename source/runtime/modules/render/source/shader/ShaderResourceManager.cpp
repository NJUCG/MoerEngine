#include "shader/ShaderResourceManager.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderResource.h"
#include <array>

void ShaderResourceManager::PrepareGlobalShaderResources() {
}

void ShaderResourceManager::UpdateGlobalShaderResources() {
}

std::array<ShaderCodeResourceMap, EShaderPlatform::SP_Num> ShaderResourceManager::code_resources{CreateArray<SP_Num, ShaderCodeResourceMap>({})};