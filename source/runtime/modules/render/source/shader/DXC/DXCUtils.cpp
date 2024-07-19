#include "DXCUtils.h"
#include "config/ConfigManager.h"
#include "platform/Platform.h"
#include <format>
#include "shader/ShaderCommon.h"
#include "spirv_reflect.h"

EShaderParameterType ToShaderParameterType(SpvReflectResourceType _type, SpvReflectDescriptorType _desc_type) {
    switch (_type) {

        case SPV_REFLECT_RESOURCE_FLAG_UNDEFINED:
            return EShaderParameterType::Num;
        case SPV_REFLECT_RESOURCE_FLAG_SAMPLER:
            return EShaderParameterType::SAMPLER;
        case SPV_REFLECT_RESOURCE_FLAG_CBV:
            if (_desc_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                return EShaderParameterType::CBUFFER;

        case SPV_REFLECT_RESOURCE_FLAG_SRV:
            if (_desc_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE || _desc_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                return EShaderParameterType::TEXTURE;

            if (_desc_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                return EShaderParameterType::CBUFFER;
            break;
        case SPV_REFLECT_RESOURCE_FLAG_UAV:

            return EShaderParameterType::UAV;
            break;
    }
    return EShaderParameterType::Num;
}

ERHIPipelineStageFlags ToPipelineStageFlag(SpvReflectShaderStageFlagBits _stage) {
    switch (_stage) {

        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
            return ERHIPipelineStageFlags::PS_VERTEX_SHADER;

        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
            return ERHIPipelineStageFlags::PS_GEOMETRY_SHADER;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
            return ERHIPipelineStageFlags::PS_FRAGMENT_SHADER;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
            return ERHIPipelineStageFlags::PS_COMPUTE_SHADER;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_NV:
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_NV:
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR: break;
    }
    return ERHIPipelineStageFlags::PS_NONE;
}

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type) {
    switch (_type) {

        case SBT_INVALID:
        case SBT_BOOL:
        case SBT_INT32:
        case SBT_UINT32:
        case SBT_FLOAT32:
            return EShaderParameterType::Num;
        case SBT_CBV:

            return EShaderParameterType::CBV;
        case SBT_CONST_STRUCT:
            return EShaderParameterType::CONSTANT_STRUCT;
        case SBT_SRV:
            return EShaderParameterType::SRV;
        case SBT_UAV:
            return EShaderParameterType::UAV;
        case SBT_SAMPLER:
            return EShaderParameterType::SAMPLER;

        defualt:
            break;
    }
    return EShaderParameterType::Num;
}
const auto* GetShaderTypeWChar(EShaderType _type) {
    switch (_type) {
        case ST_VERTEX:
            return L"vs";
        case ST_GEOMETRY:
            return L"gs";
        case ST_FRAGMENT:
            return L"ps";
        case ST_COMPUTE:
            return L"cs";
        case ST_MESH:
            return L"ms";
        case ST_AMPLIFICATION:
            return L"as";
        case ST_RAY_GEN:
            return L"lib";
        case ST_RAY_MISS:
            return L"lib";
        case ST_RAY_CLOSESTHIT:
            return L"lib";
        case ST_RAY_CALLABLE:
            return L"lib";
        case ST_RAY_INTERSECTION:
            return L"lib";
        case ST_RAY_ANYHIT:
            return L"lib";
        case ST_Num: break;
        default: break;
    }
    return L"";
}

const auto* GetShaderModel(EShaderPlatform _type) {
    switch (_type) {

        case SP_WIN_D3D_SM6:
        case SP_VULKAN_SM6:
            return L"6_7";
        case SP_Num:
        case SP_NumBits: break;
    }
    return L"";
}
std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform) {
    const auto* type     = GetShaderTypeWChar(_type);
    const auto* platform = GetShaderModel(_platform);
    auto        k        = std::format(L"{}_{}", type, platform);
    return k;
}

std::wstring SearchValidShaderPath(const std::string& _relative_shader_path) {
    std::string           shader_path    = _relative_shader_path;
    Moer::ConfigManager&  config_manager = Moer::ConfigManager::GetInstance();
    std::filesystem::path shader_dir     = config_manager.GetEngineShaderPath();
    //TODO: shader dir
    return L"";
}