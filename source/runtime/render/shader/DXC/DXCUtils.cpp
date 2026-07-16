// 实现 DXC 编译和反射阶段使用的枚举、格式与路径转换辅助函数。
#include "DXCUtils.h"
#include "PixelFormat.h"
#include "shader/ShaderCommon.h"
#include "spirv.hpp"
#include <format>

ERHIPipelineStageFlags ToPipelineStageFlag(spv::ExecutionModel _stage) {
    switch (_stage) {
        case spv::ExecutionModelVertex:
            return ERHIPipelineStageFlags::PS_VERTEX_SHADER;
        case spv::ExecutionModelFragment:
            return ERHIPipelineStageFlags::PS_FRAGMENT_SHADER;
        case spv::ExecutionModelGLCompute:
            return ERHIPipelineStageFlags::PS_COMPUTE_SHADER;
        case spv::ExecutionModelGeometry:
            return ERHIPipelineStageFlags::PS_GEOMETRY_SHADER;
        case spv::ExecutionModelTaskNV:
            return ERHIPipelineStageFlags::PS_TASK_SHADER;
        case spv::ExecutionModelMeshNV:
            return ERHIPipelineStageFlags::PS_MESH_SHADER;
        case spv::ExecutionModelRayGenerationKHR:
        case spv::ExecutionModelIntersectionKHR:
        case spv::ExecutionModelAnyHitKHR:
        case spv::ExecutionModelClosestHitKHR:
        case spv::ExecutionModelMissKHR:
        case spv::ExecutionModelCallableKHR:
            return ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER;
        defualt:
            break;
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
        case ST_Num:
            break;
        default:
            break;
    }
    return L"";
}

EPixelFormat ToPixelFormat(spv::ImageFormat _format) {
    switch (_format) {
        case spv::ImageFormat::ImageFormatRgba32f:
            return EPixelFormat::PF_R32G32B32A32_SFLOAT;

        case spv::ImageFormat::ImageFormatRgba16f:
            return EPixelFormat::PF_R16G16B16A16_SFLOAT;
        case spv::ImageFormat::ImageFormatR32f:
            return EPixelFormat::PF_R32_SFLOAT;
        case spv::ImageFormat::ImageFormatRgba8:
            return EPixelFormat::PF_R8G8B8A8_UNORM;
        case spv::ImageFormat::ImageFormatRgba8Snorm:
            return EPixelFormat::PF_R8G8B8A8_SNORM;
        case spv::ImageFormat::ImageFormatRg32f:
            return EPixelFormat::PF_R32G32_SFLOAT;
        case spv::ImageFormat::ImageFormatRg16f:
            return EPixelFormat::PF_R16G16_SFLOAT;
        case spv::ImageFormat::ImageFormatR11fG11fB10f:
            return EPixelFormat::PF_B10G11R11_UFLOAT_PACK32;
        case spv::ImageFormat::ImageFormatR16f:
            return EPixelFormat::PF_R16_SFLOAT;
        case spv::ImageFormat::ImageFormatRgba16:
            return EPixelFormat::PF_R16G16B16A16_UNORM;
        case spv::ImageFormat::ImageFormatRgb10A2:
            return EPixelFormat::PF_A2B10G10R10_UNORM_PACK32;
        case spv::ImageFormat::ImageFormatRg16:
            return EPixelFormat::PF_R16G16_UNORM;
        case spv::ImageFormat::ImageFormatRg8:
            return EPixelFormat::PF_R8G8_UNORM;
        case spv::ImageFormat::ImageFormatR16:
            return EPixelFormat::PF_R16_UNORM;
        case spv::ImageFormat::ImageFormatR8:
            return EPixelFormat::PF_R8_UNORM;
        case spv::ImageFormat::ImageFormatRgba16Snorm:
            return EPixelFormat::PF_R16G16B16A16_SNORM;
        case spv::ImageFormat::ImageFormatRg16Snorm:
            return EPixelFormat::PF_R16G16_SNORM;
        case spv::ImageFormat::ImageFormatRg8Snorm:
            return EPixelFormat::PF_R8G8_SNORM;
        case spv::ImageFormat::ImageFormatR16Snorm:
            return EPixelFormat::PF_R16_SNORM;
        case spv::ImageFormat::ImageFormatR8Snorm:
            return EPixelFormat::PF_R8_SNORM;
        case spv::ImageFormat::ImageFormatRgba32i:
            return EPixelFormat::PF_R32G32B32A32_SINT;
        case spv::ImageFormat::ImageFormatRgba16i:
            return EPixelFormat::PF_R16G16B16A16_SINT;
        case spv::ImageFormat::ImageFormatRgba8i:
            return EPixelFormat::PF_R8G8B8A8_SINT;
        case spv::ImageFormat::ImageFormatR32i:
            return EPixelFormat::PF_R32_SINT;
        case spv::ImageFormat::ImageFormatRg32i:
            return EPixelFormat::PF_R32G32_SINT;
        case spv::ImageFormat::ImageFormatRg16i:
            return EPixelFormat::PF_R16G16_SINT;
        case spv::ImageFormat::ImageFormatRg8i:
            return EPixelFormat::PF_R8G8_SINT;
        case spv::ImageFormat::ImageFormatR16i:
            return EPixelFormat::PF_R16_SINT;
        case spv::ImageFormat::ImageFormatR8i:
            return EPixelFormat::PF_R8_SINT;
        case spv::ImageFormat::ImageFormatRgba32ui:
            return EPixelFormat::PF_R32G32B32A32_UINT;
        case spv::ImageFormat::ImageFormatRgba16ui:
            return EPixelFormat::PF_R16G16B16A16_UINT;
        case spv::ImageFormat::ImageFormatRgba8ui:
            return EPixelFormat::PF_R8G8B8A8_UINT;
        case spv::ImageFormat::ImageFormatR32ui:
            return EPixelFormat::PF_R32_UINT;
        case spv::ImageFormat::ImageFormatRg32ui:
            return EPixelFormat::PF_R32G32_UINT;
        case spv::ImageFormat::ImageFormatRg16ui:
            return EPixelFormat::PF_R16G16_UINT;
        case spv::ImageFormat::ImageFormatRgb10a2ui:
            return EPixelFormat::PF_A2B10G10R10_UINT_PACK32;
        case spv::ImageFormat::ImageFormatRg8ui:
            return EPixelFormat::PF_R8G8_UINT;
        case spv::ImageFormat::ImageFormatR16ui:
            return EPixelFormat::PF_R16_UINT;
        case spv::ImageFormat::ImageFormatR8ui:
            return EPixelFormat::PF_R8_UINT;
        case spv::ImageFormat::ImageFormatR64ui:
            return EPixelFormat::PF_R64_UINT;
        case spv::ImageFormat::ImageFormatR64i:
            return EPixelFormat::PF_R64_SINT;
        default:
            break;
    }
    return EPixelFormat::PF_Num;
}

const auto* GetShaderModel(EShaderPlatform _type) {
    switch (_type) {

        case SP_WIN_D3D_SM6:
        case SP_VULKAN_SM6:
            return L"6_7";
        case SP_Num:
        case SP_NumBits:
            break;
    }
    return L"";
}
std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform) {
    const auto* type     = GetShaderTypeWChar(_type);
    const auto* platform = GetShaderModel(_platform);
    auto        k        = std::format(L"{}_{}", type, platform);
    return k;
}

std::wstring SearchValidShaderPath(const std::string&) {
    // 预留给需要按平台搜索 Shader 的调用方；当前编译流程直接使用配置中的根目录。
    return {};
}
