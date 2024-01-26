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
void ShaderCompiler::Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) {
    //todo: need to consider include dependencies
    compiler->Compile(input, output);
}

struct ShaderCompileBatch {

    Moer::Array<ShaderCompilerInput>  inputs;
    Moer::Array<ShaderCompilerOutput> outputs;

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
        const ShaderParametersMetadata* param_meta_data) {
        inputs.emplace_back(
            ShaderCompilerInput{
                target_info,
                mutation_id,
                entry_point,
                relative_source_file_path,
                shader_name,
                environment,
                param_meta_data});
    }

    void AddOutput(const ShaderCompilerOutput& output) {
        outputs.push_back(output);
    }

    void CompileBatch() {
        std::for_each(inputs.begin(), inputs.end(), [this](ShaderCompilerInput& input) {
            ShaderCompilerOutput output;
            ShaderMetaType*      meta_type = ShaderMetaType::GetNameToTypeMap().at(input.shader_name);
            if (meta_type->ShouldCompileMutation({input.target_info.shader_platform, input.mutation_id})) {
                meta_type->SetCompileEnvironment(ShaderMutationParameters{input.target_info.shader_platform, input.mutation_id}, input.environment);
                ShaderCompiler::Compile(input, output);
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
          shader_name(input.shader_name) {
    }
    void DispatchAndExecute(const std::function<void(const ShaderCompilerOutput&)>& post_process_func) {
        //call after registration
        meta_type = ShaderMetaType::GetNameToTypeMap().at(shader_name);
        //construct valid mutations
        uint32_t start = 0;
        uint32_t end   = total_mutation_count / max_count_per_batch + 1;

        Moer::Array<uint32_t> indices(end);

        //parallel generate batches
        std::for_each(std::execution::par, indices.begin(), indices.end(), [this, &post_process_func](int i) {
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
                    meta_type->GetParameterMetaData());
            }

            batch.CompileBatch();
            for_each(batch.outputs.begin(), batch.outputs.end(), [&post_process_func](const ShaderCompilerOutput& output) { post_process_func(output); });
        });
    }

    std::atomic_uint32_t actual_mutation_count = 0;
    std::atomic_uint32_t batch_count           = 0;
    uint32_t             total_mutation_count  = 0;

    ShaderTargetInfo          target_info;
    ShaderCompilerEnvironment environment;
    std::string               entry_point;
    std::string               relative_source_file_path;
    std::string               shader_name;
    ShaderMetaType*           meta_type;

    Moer::Array<ShaderCompilerOutput>
                                     outputs;
    Moer::Array<ShaderCompilerInput> inputs;
};

void ShaderCompileJob::Finalize(const ShaderCompileJobInput& input) {
    impl = new Impl(input);
}

void ShaderCompileJob::DispatchAndExecute(const std::function<void(const ShaderCompilerOutput&)>& post_process_func) {
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