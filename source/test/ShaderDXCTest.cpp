#include <filesystem>
#include <array>
#include <iostream>
#include <string_view>

#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderCompiler.h"

namespace {

using namespace Moer;
using namespace Moer::Render;

struct ShaderCompileCase {
    std::string_view case_name;
    std::string_view relative_path;
    std::string_view entry_point;
    EShaderType      shader_type;
    EShaderPlatform  shader_platform;
};

int CompileCase(
    std::string_view case_name,
    std::string_view relative_path,
    std::string_view entry_point,
    EShaderType      shader_type,
    EShaderPlatform  shader_platform
) {
    ShaderCompilerInput input{};
    input.target_info               = ShaderTargetInfo(shader_type, shader_platform);
    input.mutation_id               = 0;
    input.entry_point               = std::string(entry_point);
    input.relative_source_file_path = std::string(relative_path);
    input.shader_name               = std::string(relative_path);
    input.shader_name_hash          = static_cast<uint32_t>(GetHash(relative_path));

    LOG_INFO(
        "[ShaderDXCTest][BEGIN] case={}, path={}, entry={}, type={}, platform={}",
        case_name,
        relative_path,
        entry_point,
        static_cast<uint32_t>(shader_type),
        static_cast<uint32_t>(shader_platform)
    );

    ShaderCompilerOutput output = ShaderCompiler::Compile(std::move(input));
    if (!output.b_succeeded) {
        for (const auto& error : output.errors) {
            LOG_ERROR("[ShaderDXCTest][ERROR] case={} :: {}", case_name, error);
        }
        return 1;
    }

    if (output.shader_code.empty()) {
        LOG_ERROR("[ShaderDXCTest][ERROR] case={} produced empty shader blob", case_name);
        return 1;
    }

    LOG_INFO(
        "[ShaderDXCTest][PASS] case={}, blob_size={}, hash1={}, hash2={}",
        case_name,
        output.shader_code.size(),
        output.compiled_hash1,
        output.compiled_hash2
    );
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path path = (argc > 0) ? std::filesystem::path(argv[0]) : std::filesystem::current_path();
    if (path.extension() == ".exe") {
        path = path.parent_path();
    }

    Diagnostics::SetEnsureFailureEscalation(true);
    Diagnostics::ResetEnsureFailures();
    ConfigManager::GetInstance().Init(path);
    LogSystem::Init();
    ShaderCompiler::Init();

    constexpr std::array kCases = {
        ShaderCompileCase{"BindlessBindingsOnly.Vulkan", "tests/BindlessBindingsOnly.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessDirectArrayBuffer.Vulkan", "tests/BindlessDirectArrayBuffer.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessDirectTexture.Vulkan", "tests/BindlessDirectTexture.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessArrayBuffer.Vulkan", "tests/BindlessArrayBuffer.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessSampler.Vulkan", "tests/BindlessSampler.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessTexture.Vulkan", "tests/BindlessTexture.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessMinimal.Vulkan", "tests/BindlessMinimal.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"RayQueryMinimal.Vulkan", "tests/RayQueryMinimal.comp.hlsl", "main", ST_COMPUTE, SP_VULKAN_SM6},
        ShaderCompileCase{"RayGenMinimal.Vulkan", "tests/RayGenMinimal.rgen.hlsl", "main", ST_RAY_GEN, SP_VULKAN_SM6},
        ShaderCompileCase{"BindlessMinimal.DXIL", "tests/BindlessMinimal.comp.hlsl", "main", ST_COMPUTE, SP_WIN_D3D_SM6},
        ShaderCompileCase{"GuiVert.Vulkan", "features/ui/GuiVert.hlsl", "main", ST_VERTEX, SP_VULKAN_SM6},
        ShaderCompileCase{"GuiFrag.Vulkan", "features/ui/GuiFrag.hlsl", "main", ST_FRAGMENT, SP_VULKAN_SM6},
    };

    int failed_cases = 0;
    for (const auto& test_case : kCases) {
        failed_cases += CompileCase(
            test_case.case_name,
            test_case.relative_path,
            test_case.entry_point,
            test_case.shader_type,
            test_case.shader_platform
        );
    }

    if (failed_cases != 0) {
        LOG_ERROR("[ShaderDXCTest] failed_cases={}", failed_cases);
        return 1;
    }

    if (Diagnostics::HasEnsureFailures()) {
        LOG_ERROR("[ShaderDXCTest] observed escalated ensure failures");
        return 1;
    }

    LOG_INFO("[ShaderDXCTest] all cases passed");
    return 0;
}