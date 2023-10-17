#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/vulkan/IVulkanRHI.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include <algorithm>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>

#include "platform/Platform.h"

#include "shader/ShaderCompiler.h"
#include "shader/Shader.h"
#include "shader/ShaderMap.h"
#include "spirv_reflect.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "log/LogSystem.h"
#include "shader/ShaderCommon.h"
#include <utility>
#include <vector>
#include "rhi/RHI.h"

#include "DirectXShaderCompiler.h"

IShaderCompiler* ShaderCompiler::compiler = nullptr;
void             ShaderCompiler::Init() {
    //currently use dxc for all compile missions
    compiler = &(DXCompiler::GetInstance());
}
class TestReflectionShader : public Shader {
    DEFINE_SHADER_TYPE(TestReflectionShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    //constant
    DEFINE_SHADER_PARAM(Moer::Vector4f, color)
    DEFINE_SHADER_PARAM_SRV(Buffer, bar)
    //Ubo set
    DEFINE_SHADER_PARAM_UAV(RWBuffer, dataLog)

    DEFINE_SHADER_PARAM_SAMPLER_ARRAY(Sampler[2], samp, 2)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, aniso)
    //srv set
    DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D[5], foo, 5)
    //uav set
    DEFINE_SHADER_PARAM_CBV(ConstantBuffer<UBO>, ubo)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestReflectionShader, "TestVert.vert", "main", EShaderType::ST_VERTEX);

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

void ShaderCompiler::ShaderConductorTest() {
    g_rhi = new FakeRHI;
    TestReflectionShader::GetMetaType();

    ShaderTypeRegistration::SubmitRegistrations();

    const auto& works = ShaderCompileRegistration::RetrieveShaderCompileWorks();

    ShaderCompiler::Init();

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
        EShaderPlatform platform = input.target_info.shader_platform;
        auto&           code_map = ShaderResourceManager::GetShaderCodeMap(platform);
        // std::string     pragmas;
        // std::for_each(output.pragma.begin(), output.pragma.end(), [&pragmas](const std::string& pragma) {
        //     pragmas.append(std::format("{}\n", pragma));
        // });
        code_map.AddShaderCompilerOutput(std::format("{}", input.shader_name), output);

        //construct shader
    });

    auto& map = ShaderResourceManager::GetShaderCodeMap(EShaderPlatform::SP_VULKAN_SM6);
    map.shader_code_entries.size();
}