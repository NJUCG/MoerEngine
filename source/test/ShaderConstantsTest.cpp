#include "config/ConfigManager.h"
#include "rhi/ShaderConstantLayout.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/test/ShaderConstants.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

namespace {

class MixedConstants : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(MixedConstants);
    DEFINE_SHADER_BUFFER(values);
    DEFINE_SHADER_CONSTANTS(SHADER_TEST_CONSTANTS)
    DEFINE_SHADER_ARGS(scale, values, data, tint, transform, bias);
};

class NoConstants : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(NoConstants);
    DEFINE_SHADER_ARGS();
};

class SingleConstant : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(SingleConstant);
    DEFINE_SHADER_CONSTANT(ConstantTestData, param);
    DEFINE_SHADER_ARGS(param);
};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Fn>
void RequireRejected(Fn&& fn) {
    try { fn(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid constant layout was accepted");
}

PipelineShaderInfo MakeInfo() {
    const auto arguments = MixedConstants::GetArgumentInfoArray();
    return {
        .arguments       = {arguments.begin(), arguments.end()},
        .constant_layout = MixedConstants::GetConstantLayout()
    };
}

void CheckPacking() {
    const auto layout = MixedConstants::GetConstantLayout();
    Require(layout.byte_size == 116 && layout.arguments.size() == 5, "Unexpected packed constant size");
    const uint offsets[] = {0, 16, 32, 48, 112};
    for (uint index = 0; index < 5; ++index) {
        Require(layout.arguments[index].byte_offset == offsets[index], "Unexpected constant offset");
    }
    Require(layout.arguments[1].argument_index == 2, "Resource argument consumed constant storage");
    const float scale = 2.0f;
    const ConstantTestData data{3, 4};
    const float3 tint{0.25f, 0.5f, 0.75f};
    const float4x4 transform = float4x4::Identity();
    const uint bias = 5;
    const auto packed = MixedConstants::SetArgs(scale, BufferView{}, data, tint, transform, bias);
    const auto* bytes = reinterpret_cast<const unsigned char*>(packed.constants.data());
    Require(std::memcmp(bytes, &scale, sizeof(scale)) == 0, "Scalar overwritten");
    Require(std::memcmp(bytes + 16, &data, sizeof(data)) == 0, "Struct overwritten");
    Require(std::memcmp(bytes + 32, &tint, sizeof(tint)) == 0, "Vector overwritten");
    Require(std::memcmp(bytes + 48, &transform, sizeof(transform)) == 0, "Matrix overwritten");
    Require(std::memcmp(bytes + 112, &bias, sizeof(bias)) == 0, "Last scalar overwritten");
    for (uint index = 4; index < 16; ++index) Require(bytes[index] == 0, "Padding was not initialized");
    Require(NoConstants::SetArgs().constants.empty(), "Zero-constant pipeline allocated data");
    Require(SingleConstant::GetConstantLayout().byte_size == sizeof(data), "Single-struct layout changed");
    const auto single = SingleConstant::SetArgs(data);
    Require(std::memcmp(single.constants.data(), &data, sizeof(data)) == 0, "Single-struct constant upload changed");
    ArrayArguments short_data(6, 0, false);
    RequireRejected([&] { MixedConstants::InnerArgs::SetParam<float, MixedConstants::scale>(2.0f, short_data); });
    const auto         empty_arguments = NoConstants::GetArgumentInfoArray();
    PipelineShaderInfo empty{
        .arguments       = {empty_arguments.begin(), empty_arguments.end()},
        .constant_layout = NoConstants::GetConstantLayout()
    };
    Require(ValidatePipelineConstants(empty, 128) == 0, "Empty pipeline validation failed");
}

ShaderCompilerInput MakeInput(EShaderType stage, const char* entry, EShaderPlatform platform) {
    ShaderCompilerInput input{};
    input.target_info.shader_type = stage;
    input.target_info.shader_platform = platform;
    input.entry_point = entry;
    input.relative_source_file_path = "test/ShaderConstants.hlsl";
    input.shader_name = entry;
    const auto info = MakeInfo();
    input.environment.SetDefine("MOER_PC_LAYOUT_VERSION", uint(1));
    input.environment.SetDefine("MOER_PC_BYTE_SIZE", info.constant_layout.byte_size);
    for (const auto& argument : info.constant_layout.arguments) {
        const auto name = info.arguments[argument.argument_index].name;
        input.environment.SetDefine("MOER_PC_OFFSET_" + std::string(name), argument.byte_offset);
        input.environment.SetDefine("MOER_PC_SIZE_" + std::string(name), argument.byte_size);
    }
    return input;
}

void CheckReflection(EShaderPlatform platform) {
    auto cs = ShaderCompiler::Compile(MakeInput(ST_COMPUTE, "ComputeMain", platform));
    auto vs = ShaderCompiler::Compile(MakeInput(ST_VERTEX, "VertexMain", platform));
    auto pixel_input = MakeInput(ST_FRAGMENT, "PixelMain", platform);
    pixel_input.relative_source_file_path = "test/ShaderConstantsSubset.hlsl";
    auto ps = ShaderCompiler::Compile(std::move(pixel_input));
    Require(cs.b_succeeded && vs.b_succeeded && ps.b_succeeded, "Constant test shaders failed to compile");
    auto info = MakeInfo();
    info.shaders = {{.name = "cs", .shader_type = ST_COMPUTE, .shader_param_map = &cs.parameter_map}};
    Require(ValidatePipelineConstants(info, 128) == 116, "Compute reflection layout mismatch");
    info.shaders = {{.name = "vs", .shader_type = ST_VERTEX, .shader_param_map = &vs.parameter_map},
                    {.name = "ps", .shader_type = ST_FRAGMENT, .shader_param_map = &ps.parameter_map}};
    Require(ValidatePipelineConstants(info, 128) == 116, "Graphics reflection layout mismatch");
    const auto active = [](const ShaderCompilerOutput& output, const char* name) {
        const auto found = output.parameter_map.reflect_map.find(name);
        if (found == output.parameter_map.reflect_map.end()) return false;
        return std::get<ReflectParamInfo::Constant>(found->second.spirv.resources.data).custom_flag.active != 0;
    };
    Require(active(vs, "scale") && active(ps, "scale"), "Shared constant lost stage visibility");
    Require(!active(vs, "tint") && active(ps, "tint"), "Unused stage constant marked active");
    Require(!ps.parameter_map.reflect_map.contains("transform"), "Subset shader included an undeclared constant");
    RequireRejected([&] { ValidatePipelineConstants(info, 64); });
    auto wrong = info;
    wrong.constant_layout.arguments[0].byte_offset = 4;
    RequireRejected([&] { ValidatePipelineConstants(wrong, 128); });
    wrong = info;
    wrong.constant_layout.arguments.pop_back();
    RequireRejected([&] { ValidatePipelineConstants(wrong, 128); });
    wrong = info;
    wrong.arguments[0].cpp_info.type = SDA_Buffer;
    RequireRejected([&] { ValidatePipelineConstants(wrong, 128); });

    const auto input = MakeInput(ST_COMPUTE, "ComputeMain", platform);
    auto shifted = input;
    shifted.environment.SetDefine("MOER_PC_OFFSET_bias", uint(120));
    Require(!(input == shifted), "Layout offsets did not affect cache identity");
    shifted = input;
    shifted.environment.SetDefine("MOER_PC_LAYOUT_VERSION", uint(2));
    Require(!(input == shifted), "Layout version did not affect cache identity");
}

} // namespace

int main() {
    try {
        ConfigManager::GetInstance().Init(std::filesystem::path(MOER_TEST_WORKSPACE));
        ShaderCompiler::Init();
        CheckPacking();
        CheckReflection(SP_VULKAN_SM6);
#if defined(__APPLE__)
        CheckReflection(SP_METAL_MSL);
#endif
        std::cout << "Shader constant packing, reflection and validation: success\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
