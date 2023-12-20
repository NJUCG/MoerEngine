#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/IVulkanRHI.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <format>
#include <functional>
#include <utility>

#include "platform/Platform.h"

#include "shader/ShaderCompiler.h"
#include "shader/Shader.h"
#include "spirv_reflect.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "log/LogSystem.h"
#include "shader/ShaderCommon.h"
#include "rhi/RHI.h"

#include "DXC/DirectXShaderCompiler.h"

IShaderCompiler* ShaderCompiler::compiler = nullptr;
void             ShaderCompiler::Init() {
    //currently use dxc for all compile missions
    compiler = &(DXCompiler::GetInstance());
}

class FakeRHI : public IVulkanRHI {
public:
    FakeRHI() {
        rhi_type = ERHIType::Vulkan;
    }
};
/**
 * @brief cross-compile shader
 * 
 * @param input ShaderCompilerInput: contains all information compiler needs
 * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
 */
void ShaderCompiler::Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) {
    //todo: need to consider include dependencies
    compiler->Compile(input, output);
}
bool IsResource(EShaderBindingBaseType base_type) {
    return base_type == SBT_CBV ||
           base_type == SBT_SRV ||
           base_type == SBT_UAV ||
           base_type == SBT_SAMPLER;
}
void ShaderCompiler::ShaderCompileTest() {
    g_rhi = new FakeRHI;
    TestReflectionShader::GetMetaType();
    ShaderTypeRegistration::SubmitRegistrations();

    const auto& works = ShaderCompileRegistration::RetrieveShaderCompileWorks();

    ShaderCompiler::Init();
    ShaderResourceManager::Init(GetShaderPlatformByRHIType(g_rhi->GetType()));

    std::for_each(works.begin(), works.end(), [](const ShaderCompilerInput& input) {
        ShaderCompilerOutput output;
        //todo: check file cached
        ShaderCompiler::Compile(input, output);

        if (!output.b_succeeded) {
            std::for_each(output.errors.begin(), output.errors.end(), [](const std::string& error) {
                LOG_ERROR(error);
            });
            return;
        }
        auto& code_map = ShaderResourceManager::GetInstance().GetShaderCodeMap();
        // std::string     pragmas;
        // std::for_each(output.pragma.begin(), output.pragma.end(), [&pragmas](const std::string& pragma) {
        //     pragmas.append(std::format("{}\n", pragma));
        // });
        code_map.AddShaderCompilerOutput(std::format("{}", input.shader_name), output);

        Shader* shader = new Shader(ShaderCompiledInitializer(
            ShaderMetaType::GetNameToTypeMap().at(input.shader_name),
            output));

        ShaderResourceManager::GetInstance().GetShaderTypeMap().AddShader(input.shader_name.c_str(), shader);
        //test code
        const auto& param_map = output.parameter_map;

        auto& map = ShaderResourceManager::GetInstance().GetShaderCodeMap();

        TestReflectionShader::Parameters params;
        RHIBatchedShaderParameters       batched_params;

        const ShaderParametersMetadata*     meta_data   = TestReflectionShader::GetParametersMetaData();
        const RHIShaderRootParameterLayout& root_layout = TestReflectionShader::GetParametersMetaData()->GetLayout();
        for (const auto& parameter : root_layout.resource_parameters) {
            std::string param_name = meta_data->GetMemberNameByOffset(parameter.offset);
            if (param_map.param_map.count(param_name) > 0) {

                //param info includes slots/space and array size
                const auto& param_info = param_map.param_map.find(param_name)->second;
                //vkDescriptorWrite()
                RHIResource* resource = (RHIResource*)((uint8_t*)&params + parameter.offset);

                switch (param_info.type) {

                    case EShaderParameterType::CBV:
                        LOG_INFO("set cbv {}", param_name);
                        break;
                    case EShaderParameterType::SAMPLER:
                        LOG_INFO("set sampler {}", param_name);
                        break;
                    case EShaderParameterType::SRV:
                        LOG_INFO("set srv {}", param_name);
                        break;
                    case EShaderParameterType::UAV:
                        LOG_INFO("set uav {}", param_name);
                        break;
                    case EShaderParameterType::BINDLESS_RESOURCE_INDEX:
                    case EShaderParameterType::BINDLESS_SAMPLER_INDEX:
                    default: break;
                }
            }
        }
    });
}