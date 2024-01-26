#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResource.h"

ShaderResourceManager::ShaderResourceManager() {
}

void ShaderResourceManager::Init(EShaderPlatform _platform) {
    auto& t_ptr = GetInstance().type_resources;
    t_ptr       = new ShaderTypeResourceMap(_platform);

    auto& s_ptr = GetInstance().shader_resources;
    s_ptr       = new ShaderResourceMap();
}

void ShaderResourceManager::ShutDown() {
    for (uint32_t index = 0; index < EShaderPlatform::SP_Num; index++) {

        ShaderTypeResourceMap* t_ptr = GetInstance().type_resources;
        if (t_ptr != nullptr) {
            t_ptr->~ShaderTypeResourceMap();
            GetInstance().type_resources = nullptr;
        }

        ShaderResourceMap* s_ptr = GetInstance().shader_resources;
        if (s_ptr != nullptr) {
            s_ptr->~ShaderResourceMap();
            GetInstance().shader_resources = nullptr;
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

    static auto post_process = [this](const ShaderCompilerOutput& output) {
        if (!output.b_succeeded) {
            std::string error_msg = std::format("Shader {} compilation failed.", output.shader_name);

            std::for_each(output.errors.begin(), output.errors.end(), [&error_msg](const std::string& error) {
                error_msg += error + "\n";
            });

            LOG_ERROR(error_msg);
            return;
        }
        auto& resource_map = GetShaderResourceMap();
        resource_map.AddShaderCompilerOutput(ShaderResourceKey{output.shader_name, output.mutation_id}, output);

        ShaderMetaType* meta_type = ShaderMetaType::GetNameToTypeMap().at(output.shader_name);

        Shader* shader = meta_type->ConstructShaderInstance(ShaderCompiledInitializer(
            meta_type,
            output));

        GetShaderTypeMap().AddShader(output.shader_name, shader);
    };
    //todo: parallel compiling
    std::for_each(works.begin(), works.end(), [](const ShaderCompileJobInput& input) {
        ShaderResourceManager& self = GetInstance();
        ShaderCompileJob       job;
        job.Finalize(input);
        job.DispatchAndExecute(post_process);
    });
}

RHIShaderRef ShaderResourceManager::GetShader(const ShaderMetaType& _meta_type, uint32_t _mutation_id) {
    //sync problem
    assert(type_resources != nullptr);
    Shader* shader = type_resources->FindOrAddShader(_meta_type.GetName(), nullptr);
    if (shader == nullptr) return nullptr;

    ShaderResourceKey key{shader->GetShaderMetaType()->GetName(), _mutation_id};
    return shader_resources->GetRHIShader(key, shader);
}
