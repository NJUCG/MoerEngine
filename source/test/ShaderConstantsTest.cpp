#include "config/ConfigManager.h"
#include "rhi/ShaderConstantLayout.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/test/ShaderConstants.h"
#include "shaderheaders/shared/raster/lighting_pass/Constants.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace Moer;
using namespace Moer::Render;

namespace {

class BindlessConstants : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessConstants);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_CONSTANT(float, scale);
    DEFINE_SHADER_ARGS(bdls, index, scale);
};

class BindlessSingleConstant : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessSingleConstant);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_ARGS(bdls, index);
};

class BindlessOnly : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessOnly);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls);
};

class LightingConstants : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(LightingConstants);
    DEFINE_SHADER_BUFFER(lighting_data);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANTS(LIGHTING_PASS_CONSTANTS);
    DEFINE_SHADER_ARGS(lighting_data, bdls, param, extra_ambient_intensity);
};
class BilateralConstants : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BilateralConstants);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(BilateralFilterDenoiserPipelineBindlessParam, param);
    DEFINE_SHADER_ARGS(bdls, param);
};
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
    return {.argument_metadata = MixedConstants::GetArgumentMetadata()};
}

void CheckPacking() {
    const auto metadata = MixedConstants::GetArgumentMetadata();
    const auto layouts  = metadata.GetConstantLayouts();
    Require(metadata.GetConstantByteSize() == 116 && layouts.size() == 5, "Unexpected packed constant size");
    const uint offsets[] = {0, 16, 32, 48, 112};
    for (uint index = 0; index < 5; ++index) {
        Require(layouts[index].byte_offset == offsets[index], "Unexpected constant offset");
    }
    Require(layouts[1].argument_index == 2, "Resource argument consumed constant storage");
    Require(layouts[2].byte_size == sizeof(float3), "Vector payload size was rounded up");
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
    Require(
        SingleConstant::GetArgumentMetadata().GetConstantByteSize() == sizeof(data),
        "Single-struct layout changed"
    );
    const auto single = SingleConstant::SetArgs(data);
    Require(std::memcmp(single.constants.data(), &data, sizeof(data)) == 0, "Single-struct constant upload changed");
    ArrayArguments short_data(6, 0, false);
    RequireRejected([&] { MixedConstants::InnerArgs::SetParam<float, MixedConstants::scale>(2.0f, short_data); });
    PipelineShaderInfo empty{.argument_metadata = NoConstants::GetArgumentMetadata()};
    Require(ValidatePipelineConstants(empty, 128) == 0, "Empty pipeline validation failed");
}

void CheckMetadataOwnership() {
    static_assert(!std::is_aggregate_v<PipelineArgumentMetadata>);
    static_assert(!std::is_constructible_v<
                  PipelineArgumentMetadata,
                  std::span<const PipelineArgumentInfo>,
                  std::span<const ShaderConstantArgumentLayout>,
                  uint>);
    static_assert(std::is_same_v<
                  decltype(std::declval<PipelineArgumentMetadata&>().GetArguments()),
                  std::span<const PipelineArgumentInfo>>);
    static_assert(std::is_same_v<
                  decltype(std::declval<PipelineArgumentMetadata&>().GetConstantLayouts()),
                  std::span<const ShaderConstantArgumentLayout>>);

    auto original = MixedConstants::GetArgumentMetadata();
    auto copy     = original;
    auto moved    = std::move(original);
    Require(
        original.GetArguments().empty() && original.GetConstantLayouts().empty() &&
            original.GetConstantByteSize() == 0,
        "Moved-from metadata is not a valid empty layout"
    );
    original = std::move(moved);
    Require(
        moved.GetArguments().empty() && moved.GetConstantLayouts().empty() &&
            moved.GetConstantByteSize() == 0,
        "Move assignment left inconsistent metadata"
    );
    copy = NoConstants::GetArgumentMetadata();
    Require(
        original.GetConstantByteSize() == 116 && original.GetArguments().size() == 6 &&
            original.GetConstantLayouts().size() == 5,
        "Metadata ownership was not preserved"
    );
    Require(
        copy.GetArguments().empty() && copy.GetConstantByteSize() == 0,
        "Replacing metadata did not replace the complete layout"
    );
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
    const auto& metadata = info.argument_metadata;
    input.environment.SetDefine("MOER_PC_BYTE_SIZE", metadata.GetConstantByteSize());
    for (const auto& argument : metadata.GetConstantLayouts()) {
        const auto name = metadata.GetArguments()[argument.argument_index].name;
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
        return std::get<ReflectParamInfo::Constant>(found->second.GetSpirvReflection().data)
                   .custom_flag.active != 0;
    };
    Require(active(vs, "scale") && active(ps, "scale"), "Shared constant lost stage visibility");
    Require(!active(vs, "tint") && active(ps, "tint"), "Unused stage constant marked active");
    Require(!ps.parameter_map.reflect_map.contains("transform"), "Subset shader included an undeclared constant");
    RequireRejected([&] { ValidatePipelineConstants(info, 64); });
    auto       wrong_reflection  = ps.parameter_map;
    const auto reject_reflection = [&] {
        auto wrong = info;
        // The first stage already uses constants; the later stage must still be validated.
        wrong.shaders.back().shader_param_map = &wrong_reflection;
        RequireRejected([&] {
            ValidatePipelineConstants(wrong, 128);
        });
    };
    std::get<ReflectParamInfo::Constant>(wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data)
        .offset = 4;
    reject_reflection();
    wrong_reflection = ps.parameter_map;
    std::get<ReflectParamInfo::Constant>(wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data)
        .size = 8;
    reject_reflection();
    wrong_reflection = ps.parameter_map;
    std::get<ReflectParamInfo::Constant>(wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data)
        .size = 0;
    reject_reflection();
    wrong_reflection = ps.parameter_map;
    wrong_reflection.reflect_map["undeclared"].GetSpirvReflection().data =
        ReflectParamInfo::Constant{.offset = 0, .size = 4, .custom_flag = {.active = 1}};
    reject_reflection();
    wrong_reflection = ps.parameter_map;
    wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data =
        ReflectParamInfo::Resource{.set = 0, .binding = 0, .count = 1, .custom_flag = {.active = 1}};
    reject_reflection();

    // Binding kinds must match even when the shader does not use the parameter.
    std::get<ReflectParamInfo::Resource>(wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data)
        .custom_flag.active = 0;
    reject_reflection();

    // Unused constants may have no meaningful reflected offset or size.
    wrong_reflection = ps.parameter_map;
    auto& unused_constant = std::get<ReflectParamInfo::Constant>(
        wrong_reflection.reflect_map.at("scale").GetSpirvReflection().data
    );
    unused_constant.offset                         = 4;
    unused_constant.size                           = 0;
    unused_constant.custom_flag.active             = 0;
    auto partially_used                            = info;
    partially_used.shaders.back().shader_param_map = &wrong_reflection;
    Require(ValidatePipelineConstants(partially_used, 128) == 116, "Unused constant layout was validated");

    auto unused = info;
    unused.shaders.clear();
    Require(ValidatePipelineConstants(unused, 64) == 0, "Unused constants required an upload");

    const auto input = MakeInput(ST_COMPUTE, "ComputeMain", platform);
    auto shifted = input;
    shifted.environment.SetDefine("MOER_PC_OFFSET_bias", uint(120));
    Require(!(input == shifted), "Layout offsets did not affect cache identity");
    shifted = input;
    shifted.environment.SetDefine("MOER_PC_LAYOUT_VERSION", uint(2));
    Require(!(input == shifted), "Layout version did not affect cache identity");
}

ShaderCompilerOutput CompileBindlessTest(
    const PipelineArgumentMetadata& metadata, EShaderPlatform platform,
    EShaderType stage, const char* entry, bool single = false, bool address_only = false,
    const char* source = "test/ShaderBindlessAddress.hlsl"
) {
    ShaderCompilerInput input{};
    input.target_info = {stage, platform};
    input.entry_point = entry;
    input.relative_source_file_path = source;
    input.shader_name = entry;
    input.environment.SetDefine("MOER_PC_BYTE_SIZE", metadata.GetConstantByteSize());
    input.environment.SetDefine("MOER_TEST_SINGLE_CONSTANT", uint(single));
    input.environment.SetDefine("MOER_TEST_ADDRESS_ONLY", uint(address_only));
    for (const auto& layout : metadata.GetConstantLayouts()) {
        const auto name = metadata.GetArguments()[layout.argument_index].name;
        input.environment.SetDefine("MOER_PC_OFFSET_" + std::string(name), layout.byte_offset);
    }
    auto output = ShaderCompiler::Compile(std::move(input));
    for (const auto& error : output.errors) std::cerr << error << '\n';
    Require(output.b_succeeded, "Bindless address shader failed to compile");
    return output;
}

void CheckBindlessAddress(EShaderPlatform platform) {
    const bool vulkan = platform == SP_VULKAN_SM6;
    const auto check = [&](const PipelineArgumentMetadata& metadata, bool single, bool address_only) {
        auto output = CompileBindlessTest(metadata, platform, ST_COMPUTE, "ComputeMain", single, address_only);
        const auto* bindings = output.parameter_map.reflect_map.at(std::string(ReflectParamInfo::bdls_name))
            .GetSpirvReflection().TryGetBindlessArray();
        Require(bindings && bindings->buffer && bindings->image && bindings->sampler,
                "Bindless descriptor arrays disappeared");
        Require(bindings->buffer->binding == (vulkan ? 0 : 1), "Wrong buffer descriptor binding");
        Require(bindings->array.has_value() == !vulkan, "Vulkan retained the index-table descriptor");
        Require(bindings->index_table_address.has_value() == vulkan, "Wrong index-table access mechanism");
        const uint user_size = metadata.GetConstantByteSize();
        const uint total_size = ((user_size + 7u) & ~7u) + 8u;
        PipelineShaderInfo info{
            .shaders = {{.name = "bindless", .shader_type = ST_COMPUTE, .shader_param_map = &output.parameter_map}},
            .argument_metadata = metadata
        };
        Require(ValidatePipelineConstants(info, total_size) == user_size, "Address leaked into user upload");
        if (!vulkan) return;
        Require(bindings->index_table_address->offset == total_size - 8 &&
                bindings->index_table_address->size == 8, "Address alignment or size is wrong");
        RequireRejected([&] { ValidatePipelineConstants(info, total_size - 1); });
        auto wrong = output.parameter_map;
        auto& address = std::get<ReflectParamInfo::BindlessArray>(
            wrong.reflect_map.at(std::string(ReflectParamInfo::bdls_name)).GetSpirvReflection().data
        ).index_table_address.value();
        info.shaders.push_back({.name = "wrong-stage", .shader_type = ST_FRAGMENT, .shader_param_map = &wrong});
        address.offset += 8;
        RequireRejected([&] { ValidatePipelineConstants(info, 256); });
        address.offset -= 8;
        address.size = 4;
        RequireRejected([&] { ValidatePipelineConstants(info, 256); });
        info.shaders.pop_back();
        info.argument_metadata = NoConstants::GetArgumentMetadata();
        RequireRejected([&] { ValidatePipelineConstants(info, 256); });
    };
    check(BindlessOnly::GetArgumentMetadata(), false, true);
    check(BindlessSingleConstant::GetArgumentMetadata(), true, false);
    check(BindlessConstants::GetArgumentMetadata(), false, false);

    const auto metadata = BindlessConstants::GetArgumentMetadata();
    auto vs = CompileBindlessTest(metadata, platform, ST_VERTEX, "VertexMain");
    auto ps = CompileBindlessTest(metadata, platform, ST_FRAGMENT, "PixelMain");
    PipelineShaderInfo graphics{
        .shaders = {{.name = "vs", .shader_type = ST_VERTEX, .shader_param_map = &vs.parameter_map},
                    {.name = "ps", .shader_type = ST_FRAGMENT, .shader_param_map = &ps.parameter_map}},
        .argument_metadata = metadata
    };
    Require(ValidatePipelineConstants(graphics, 16) == 8, "Graphics address layout disagrees across stages");
    auto unused = CompileBindlessTest(metadata, platform, ST_COMPUTE, "NoBindlessMain");
    graphics.shaders = {{.name = "unused", .shader_type = ST_COMPUTE, .shader_param_map = &unused.parameter_map}};
    Require(ValidatePipelineConstants(graphics, 8) == 8, "Unused address consumed push constant capacity");
    if (vulkan) {
        const auto only_metadata = BindlessOnly::GetArgumentMetadata();
        auto index_only = CompileBindlessTest(only_metadata, platform, ST_COMPUTE, "IndexOnlyMain", false, true);
        const auto& bindings = *index_only.parameter_map.reflect_map.at(std::string(ReflectParamInfo::bdls_name))
            .GetSpirvReflection().TryGetBindlessArray();
        // DXC preserves declared interfaces; unused tables may remain in reflection.
        const auto inactive = [](const std::optional<ReflectParamInfo::Bindless>& binding) {
            return !binding || !binding->custom_flag.active;
        };
        Require(bindings.index_table_address && bindings.index_table_address->custom_flag.active &&
                !bindings.array && inactive(bindings.buffer) && inactive(bindings.image) &&
                inactive(bindings.sampler) && inactive(bindings.acceleration_structure),
                "Index-only reads activate descriptor tables");
        PipelineShaderInfo info{
            .shaders = {{.name = "index-only", .shader_type = ST_COMPUTE, .shader_param_map = &index_only.parameter_map}},
            .argument_metadata = only_metadata
        };
        Require(ValidatePipelineConstants(info, 8) == 0, "Address-only upload requires user constant data");
    }

    auto lighting_metadata = LightingConstants::GetArgumentMetadata();
    auto lighting = CompileBindlessTest(
        lighting_metadata, platform, ST_FRAGMENT, "main", false, false,
        "pipelines/raster/deferred/lighting/RasterLightingPass.frag.hlsl"
    );
    PipelineShaderInfo lighting_info{
        .shaders = {{.name = "lighting", .shader_type = ST_FRAGMENT, .shader_param_map = &lighting.parameter_map}},
        .argument_metadata = lighting_metadata
    };
    Require(ValidatePipelineConstants(lighting_info, 128) == lighting_metadata.GetConstantByteSize(),
            "Lighting pipeline's user constants changed");
    const auto bilateral_metadata = BilateralConstants::GetArgumentMetadata();
    const auto bilateral = CompileBindlessTest(
        bilateral_metadata, platform, ST_FRAGMENT, "main", false, false,
        "pipelines/postprocess/denoise/BilateralFilterDenoiser.hlsl"
    );
    const auto* bilateral_bindings = bilateral.parameter_map.reflect_map.at(std::string(ReflectParamInfo::bdls_name))
        .GetSpirvReflection().TryGetBindlessArray();
    Require(bilateral_bindings && (!vulkan || bilateral_bindings->index_table_address),
            "Bilateral filter lost its bindless index-table address");
}

} // namespace

int main() {
    try {
        ConfigManager::GetInstance().Init(std::filesystem::path(MOER_TEST_WORKSPACE));
        ShaderCompiler::Init();
        CheckPacking();
        CheckMetadataOwnership();
        CheckReflection(SP_VULKAN_SM6);
        CheckBindlessAddress(SP_VULKAN_SM6);
#if defined(__APPLE__)
        CheckReflection(SP_METAL_MSL);
        CheckBindlessAddress(SP_METAL_MSL);
#endif
        std::cout << "Shader constant packing, reflection and validation: success\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
