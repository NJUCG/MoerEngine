#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResource.h"
#include <array>
ShaderResourceManager::ShaderResourceManager() {
}

void ShaderResourceManager::Init(EShaderPlatform _platform) {
    auto& c_ptr = GetInstance().code_resources;
    c_ptr       = new ShaderCodeResourceMap(_platform);

    auto& t_ptr = GetInstance().type_resources;
    t_ptr       = new ShaderTypeResourceMap(_platform);
}

void ShaderResourceManager::ShutDown() {
    for (uint32_t index = 0; index < EShaderPlatform::SP_Num; index++) {
        ShaderCodeResourceMap* c_ptr = GetInstance().code_resources;
        if (c_ptr != nullptr) {
            c_ptr->~ShaderCodeResourceMap();
            GetInstance().code_resources = nullptr;
        }
        ShaderTypeResourceMap* t_ptr = GetInstance().type_resources;
        if (t_ptr != nullptr) {
            t_ptr->~ShaderTypeResourceMap();
            GetInstance().type_resources = nullptr;
        }
    }
}

ShaderResourceManager& ShaderResourceManager::GetInstance() {
    static ShaderResourceManager manager;
    return manager;
}

void ShaderResourceManager::PrepareGlobalShaderResources() {

    //submit registrated shader types
    ShaderTypeRegistration::SubmitRegistrations();

    //retrieve all possible compile works
    const auto& works = ShaderCompileRegistration::RetrieveShaderCompileWorks();

    //todo: parallel compiling
    std::for_each(works.begin(), works.end(), [](const ShaderCompilerInput& input) {
        ShaderResourceManager& self = GetInstance();
        ShaderCompilerOutput   output;
        //todo: check file cached
        ShaderCompiler::Compile(input, output);

        if (!output.b_succeeded) {
            std::for_each(output.errors.begin(), output.errors.end(), [](const std::string& error) {
                LOG_ERROR(error);
            });
            return;
        }
        EShaderPlatform platform = input.target_info.shader_platform;
        auto&           code_map = self.GetShaderCodeMap();
        // std::string     pragmas;
        // std::for_each(output.pragma.begin(), output.pragma.end(), [&pragmas](const std::string& pragma) {
        //     pragmas.append(std::format("{}\n", pragma));
        // });
        code_map.AddShaderCompilerOutput(std::format("{}", input.shader_name), output);
        ShaderMetaType* meta_type = ShaderMetaType::GetNameToTypeMap().at(input.shader_name);

        Shader* shader = meta_type->ConstructShaderInstance(ShaderCompiledInitializer(
            meta_type,
            output));

        self.GetShaderTypeMap().AddShader(input.shader_name.c_str(), shader);
        //test code
        const auto& param_map = output.parameter_map;

        auto& map = ShaderResourceManager::GetInstance().GetShaderCodeMap();
    });
}

Shader* ShaderResourceManager::GetShader(const ShaderMetaType& _meta_type) {
    assert(type_resources != nullptr);
    return type_resources->FindOrAddShader(_meta_type.GetName(), nullptr);
}
