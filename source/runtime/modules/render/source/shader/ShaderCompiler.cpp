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
#include <execution>
#include <format>
#include <functional>
#include <stdint.h>
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

#include "Core.h"

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
ShaderCompilerOutput* ShaderCompiler::Compile(const ShaderCompilerInput& input) {
    //todo: need to consider include dependencies
    return compiler->Compile(input);
}

struct ShaderCompileBatch {

    Moer::Array<ShaderCompilerInput>   inputs;
    Moer::Array<ShaderCompilerOutput*> outputs;

    ShaderCompilerEnvironment environment;

    void Reserve(uint32_t size) {
        inputs.reserve(size);
        outputs.reserve(size);
    }

    void AddInput(
        ShaderTargetInfo                target_info,
        uint32_t                        mutation_id,
        std::string_view                entry_point,
        std::string_view                relative_source_file_path,
        std::string_view                shader_name,
        uint32_t                       shader_name_hash,
        const ShaderParametersMetadata* param_meta_data) {
        inputs.emplace_back(
            ShaderCompilerInput{
                target_info,
                mutation_id,
                entry_point,
                relative_source_file_path,
                shader_name,
                shader_name_hash,
                environment,
                param_meta_data});
    }

    void AddOutput(ShaderCompilerOutput* output) {
        outputs.push_back(output);
    }

    void CompileBatch() {
        std::for_each(inputs.begin(), inputs.end(), [this](ShaderCompilerInput& input) {
            ShaderMetaType* meta_type = ShaderMetaType::GetShaderMetaType(input.shader_name_hash);
            if (meta_type->ShouldCompileMutation({(EShaderPlatform)input.target_info.shader_platform, input.mutation_id})) {
                meta_type->SetCompileEnvironment(ShaderMutationParameters{(EShaderPlatform)input.target_info.shader_platform, input.mutation_id}, input.environment);
                auto* output = ShaderCompiler::Compile(input);
                AddOutput(output);
            }
        });
    }
};
class ShaderCompileJob::Impl {
    static constexpr uint32_t max_count_per_batch = 100;

public:
    Impl(const ShaderCompileJobInput& input)
        : total_mutation_count(input.mutation_count),
          target_info(input.target_info),
          entry_point(input.entry_point),
          relative_source_file_path(input.relative_source_file_path),
          shader_name(input.shader_name),
          shader_name_hash(input.shader_name_hash){

        SetBasePlatformEnvironment();
    }
    void DispatchAndExecute(const std::function<void(ShaderCompilerOutput*)>& post_process_func) {
        //call after registration
        meta_type = ShaderMetaType::GetShaderMetaType(shader_name_hash);
        //construct valid mutations
        uint32_t start = 0;
        uint32_t end   = total_mutation_count / max_count_per_batch + 1;

        Moer::Array<uint32_t>                           indices(end);
        Moer::Array<Moer::Array<ShaderCompilerOutput*>> output_temp_array(end);

        //parallel generate batches
        std::for_each(std::execution::par, indices.begin(), indices.end(), [this, &post_process_func, &output_temp_array](int i) {
            uint32_t start_index = i * max_count_per_batch;
            uint32_t end_index   = Moer::Min((i + 1) * max_count_per_batch, total_mutation_count);

            ShaderCompileBatch batch;

            for (uint32_t index = start_index; index < end_index; index++) {
                uint32_t current_index = index + start_index;

                ShaderCompilerEnvironment env = environment;

                batch.environment = env;
                batch.AddInput(
                    target_info,
                    current_index,
                    entry_point,
                    relative_source_file_path,
                    shader_name,
                    shader_name_hash,
                    meta_type->GetParameterMetaData());
            }

            batch.CompileBatch();
            output_temp_array[i].swap(batch.outputs);
        });
        for_each(output_temp_array.begin(), output_temp_array.end(), [this](auto& output_array) {
            outputs.insert(outputs.end(), output_array.begin(), output_array.end());
        });

        //post process
        std::for_each(outputs.begin(), outputs.end(), post_process_func);
    }
    void ExportOutput(Moer::Array<ShaderCompilerOutput*>& _outputs) {
        _outputs.swap(this->outputs);
    }

    void SetBasePlatformEnvironment() {
        switch (target_info.shader_platform) {
            case SP_WIN_D3D_SM6:
                environment.SetCompileArg("DXIL", true);
                break;
            case SP_VULKAN_SM6:
                environment.SetCompileArg("VULKAN_HLSL", true);
                break;
            default:
                break;
        }
        //MARK... RHI configuration
    }

    std::atomic_uint32_t actual_mutation_count = 0;
    std::atomic_uint32_t batch_count           = 0;
    uint32_t             total_mutation_count  = 0;

    ShaderTargetInfo          target_info;
    ShaderCompilerEnvironment environment;
    std::string_view          entry_point;
    std::string_view          relative_source_file_path;
    std::string_view          shader_name;
    uint32_t                  shader_name_hash;
    ShaderMetaType*           meta_type;

    Moer::Array<ShaderCompilerOutput*>
                                     outputs;
    Moer::Array<ShaderCompilerInput> inputs;
};

void ShaderCompileJob::Finalize(const ShaderCompileJobInput& input) {
    impl = new Impl(input);
}

void ShaderCompileJob::ExportOutput(Moer::Array<ShaderCompilerOutput*>& _outputs) {
    assert(impl && "shader compile job not finalized");
    impl->ExportOutput(_outputs);
}

ShaderCompileJob::~ShaderCompileJob() {
    if (impl) {
        delete impl;
    }
}
void ShaderCompileJob::DispatchAndExecute(const std::function<void(ShaderCompilerOutput*)>& post_process_func) {
    assert(impl && "shader compile job not finalized");
    impl->DispatchAndExecute(post_process_func);
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

    std::for_each(works.begin(), works.end(), [](const ShaderCompileJobInput& input) {

    });
}