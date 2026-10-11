#include "config/ConfigManager.h"
#include "rhi/ShaderConstantLayout.h"
#include "serialize/Serializer.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"

#include <filesystem>
#include <iostream>
#include <sstream>

using namespace Moer;
using namespace Moer::Render;

namespace {

class ReservedNameConstant : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ReservedNameConstant);
    DEFINE_SHADER_CONSTANT(uint, bdls_114514);
    DEFINE_SHADER_ARGS(bdls_114514);
};

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Fn>
void RequireRejected(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid shader reflection was accepted");
}

std::string Serialize(const ReflectParamInfo& parameter) {
    std::ostringstream bytes(std::ios::binary);
    OutputStream       stream(bytes);
    stream << parameter;
    return bytes.str();
}

ReflectParamInfo Deserialize(const std::string& bytes) {
    std::istringstream input(bytes, std::ios::binary);
    InputStream        stream(input);
    ReflectParamInfo   parameter;
    stream >> parameter;
    Require(!input.fail(), "Truncated shader reflection was accepted");
    return parameter;
}

void CheckTypedAccess() {
    ReflectParamInfo parameter;
    auto&            spirv_reflection = parameter.GetSpirvReflection();
    Require(
        spirv_reflection.TryGetResource() != nullptr && spirv_reflection.TryGetConstant() == nullptr &&
            spirv_reflection.TryGetBindlessArray() == nullptr,
        "Default reflection has no valid resource type"
    );
    spirv_reflection.data = ReflectParamInfo::Constant{.size = 4, .custom_flag = {.active = 1}};
    Require(
        spirv_reflection.TryGetConstant() != nullptr && spirv_reflection.TryGetResource() == nullptr &&
            spirv_reflection.TryGetBindlessArray() == nullptr,
        "Constant reflection has the wrong type"
    );
    try {
        spirv_reflection.Get<ReflectParamInfo::Resource>("value", "Resource");
        throw std::runtime_error("Wrong reflection type was accepted");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        Require(
            message.find("value") != std::string::npos && message.find("Resource") != std::string::npos,
            "Type error did not identify the parameter and expected type"
        );
    }

    ShaderParametersInfoMap reflection;
    reflection.reflect_map.emplace(std::string(ReflectParamInfo::bdls_name), parameter);
    reflection.reflect_map["arbitrary_table_name"].GetSpirvReflection().data =
        ReflectParamInfo::BindlessArray{};
    reflection.reflect_map["ordinary_resource"].GetSpirvReflection().data = ReflectParamInfo::Resource{};
    PipelineShaderInfo info{
        .shaders           = {{.name = "typed", .shader_param_map = &reflection}},
        .argument_metadata = ReservedNameConstant::GetArgumentMetadata()
    };
    Require(ValidatePipelineConstants(info, 128) == 4, "A parameter name was used as its type tag");

    auto copy = reflection;
    copy.reflect_map.at(std::string(ReflectParamInfo::bdls_name)).GetSpirvReflection().data =
        ReflectParamInfo::BindlessArray{};
    info.shaders.front().shader_param_map = &copy;
    RequireRejected([&] {
        ValidatePipelineConstants(info, 128);
    });
    Require(
        reflection.reflect_map.at(std::string(ReflectParamInfo::bdls_name))
                .GetSpirvReflection()
                .TryGetConstant() != nullptr,
        "Copying reflection did not preserve ownership"
    );
}

void CheckSerialization() {
    ReflectParamInfo resource{ReflectParamInfo::SpirvParameterReflection{
        17,
        ReflectParamInfo::Resource{
            .set           = 2,
            .binding       = 3,
            .sampled       = 1,
            .desc_type     = 4,
            .resource_type = 5,
            .count         = 6,
            .custom_flag   = {.active = 1},
            .format        = PF_R32_SFLOAT
        }
    }};
    const auto       resource_bytes    = Serialize(resource);
    const auto       restored_resource = Deserialize(resource_bytes);
    const auto&      r                 = *restored_resource.GetSpirvReflection().TryGetResource();
    Require(
        restored_resource.GetSpirvReflection().stage_bits == 17 && r.set == 2 && r.binding == 3 &&
            r.sampled == 1 && r.desc_type == 4 && r.resource_type == 5 && r.count == 6 &&
            r.custom_flag.active && r.format == PF_R32_SFLOAT,
        "Resource cache lost fields"
    );
    Require(
        resource_bytes.size() == 38 && resource_bytes[0] == 0 && resource_bytes[1] == 0,
        "Resource cache does not use its fixed field format"
    );

    ReflectParamInfo constant{ReflectParamInfo::SpirvParameterReflection{
        19,
        ReflectParamInfo::Constant{.offset = 16, .size = 12, .padded_size = 16, .custom_flag = {.active = 1}}
    }};
    const auto       constant_bytes    = Serialize(constant);
    const auto       restored_constant = Deserialize(constant_bytes);
    const auto&      c                 = *restored_constant.GetSpirvReflection().TryGetConstant();
    Require(
        restored_constant.GetSpirvReflection().stage_bits == 19 && c.offset == 16 && c.size == 12 &&
            c.padded_size == 16 && c.custom_flag.active,
        "Constant cache lost fields"
    );
    Require(
        constant_bytes.size() == 22 && constant_bytes[0] == 0 && constant_bytes[1] == 1,
        "Constant cache does not use its fixed field format"
    );

    ReflectParamInfo table{ReflectParamInfo::SpirvParameterReflection{23, ReflectParamInfo::BindlessArray{}}};
    auto&            bindings = std::get<ReflectParamInfo::BindlessArray>(table.GetSpirvReflection().data);
    bindings.array                  = ReflectParamInfo::Bindless{2, 3, 4, 5, 6, 7, {.active = 1}};
    bindings.image                  = ReflectParamInfo::Bindless{8, 9, 10, 11, 12, 13, {.active = 0}};
    bindings.acceleration_structure = ReflectParamInfo::Bindless{14, 15, 16, 17, 18, 19, {.active = 1}};
    const auto  table_bytes         = Serialize(table);
    const auto  restored_table      = Deserialize(table_bytes);
    const auto& b                   = *restored_table.GetSpirvReflection().TryGetBindlessArray();
    Require(
        restored_table.GetSpirvReflection().stage_bits == 23 && b.array && b.image &&
            b.acceleration_structure && !b.buffer && !b.sampler,
        "Bindless cache lost array presence"
    );
    const ReflectParamInfo::Bindless* restored[] = {&*b.array, &*b.image, &*b.acceleration_structure};
    const ReflectParamInfo::Bindless* original[] = {
        &*bindings.array, &*bindings.image, &*bindings.acceleration_structure
    };
    for (size_t index = 0; index < 3; ++index) {
        const auto& lhs = *restored[index];
        const auto& rhs = *original[index];
        Require(
            lhs.set == rhs.set && lhs.binding == rhs.binding && lhs.count == rhs.count &&
                lhs.desc_type == rhs.desc_type && lhs.resource_type == rhs.resource_type &&
                lhs.stage_bits == rhs.stage_bits && lhs.custom_flag.active == rhs.custom_flag.active,
            "Bindless cache lost binding fields"
        );
    }
    Require(table_bytes[0] == 0 && table_bytes[1] == 2, "Bindless cache type tag changed");

    ReflectParamInfo dxil{ReflectParamInfo::DxilParameterReflection{
        .slot = 2, .space = 3, .count = 4, .type = 0xabcd, .byte_size = 0x1234
    }};
    const auto  dxil_bytes    = Serialize(dxil);
    const auto  restored_dxil = Deserialize(dxil_bytes);
    const auto&      d             = restored_dxil.GetDxilReflection();
    Require(
        d.slot == 2 && d.space == 3 && d.count == 4 && d.type == 0xabcd && d.byte_size == 0x1234,
        "DXIL cache lost fields"
    );
    Require(dxil_bytes.size() == 17 && dxil_bytes[0] == 1, "DXIL cache does not use its fixed field format");
    Require(
        std::get_if<ReflectParamInfo::SpirvParameterReflection>(&restored_dxil.backend_data) == nullptr,
        "DXIL reflection was read as SPIR-V"
    );

    // Exercise the serializer dispatch used by Shader::reflection.reflect_map.
    UnorderedMap<std::string, ReflectParamInfo> parameters{
        {"resource", resource}, {"constant", constant}, {"table", table}, {"dxil", dxil}
    };
    std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
    OutputStream      output(bytes);
    output << parameters;
    InputStream                                 input(bytes);
    UnorderedMap<std::string, ReflectParamInfo> round_trip;
    input >> round_trip;
    Require(!bytes.fail() && round_trip.size() == parameters.size(), "Reflection map cache failed");
    for (const auto& [name, parameter] : parameters) {
        Require(
            Serialize(round_trip.at(name)) == Serialize(parameter), "Reflection map cache changed a parameter"
        );
    }

    for (const auto& valid : {resource_bytes, constant_bytes, table_bytes, dxil_bytes}) {
        for (size_t length = 0; length < valid.size(); ++length) {
            bool rejected = false;
            try {
                Deserialize(valid.substr(0, length));
            } catch (const std::exception&) {
                rejected = true;
            }
            Require(rejected, "Truncated reflection cache was accepted");
        }
    }
    auto invalid = constant_bytes;
    invalid[0]   = char(0xff);
    RequireRejected([&] {
        Deserialize(invalid);
    });
    invalid    = constant_bytes;
    invalid[1] = char(0xff);
    RequireRejected([&] {
        Deserialize(invalid);
    });
    invalid    = table_bytes;
    invalid[6] = 2;
    RequireRejected([&] {
        Deserialize(invalid);
    });
}

void CheckCompilerReflection() {
    ConfigManager::GetInstance().Init(std::filesystem::path(MOER_TEST_WORKSPACE));
    ShaderCompiler::Init();
    ShaderCompilerInput input{};
    input.target_info.shader_type     = ST_COMPUTE;
    input.target_info.shader_platform = SP_VULKAN_SM6;
    input.entry_point                 = "ComputeMain";
    input.relative_source_file_path   = "test/ShaderParameterReflection.hlsl";
    input.shader_name                 = "typed_reflection";
    const auto output                 = ShaderCompiler::Compile(ShaderCompilerInput(input));
    Require(output.b_succeeded, "Reflection shader did not compile");
    const auto& parameters = output.parameter_map.reflect_map;
    Require(
        parameters.at("result").GetSpirvReflection().TryGetResource() != nullptr &&
            parameters.at("params").GetSpirvReflection().TryGetConstant() != nullptr,
        "Compiler did not assign resource and constant types"
    );
    const auto* bindings =
        parameters.at(std::string(ReflectParamInfo::bdls_name)).GetSpirvReflection().TryGetBindlessArray();
    Require(
        bindings && bindings->array && bindings->buffer && bindings->image && bindings->sampler,
        "Compiler did not aggregate bindless arrays"
    );
    Require(
        bindings->array->custom_flag.active && bindings->buffer->custom_flag.active &&
            bindings->image->custom_flag.active && bindings->sampler->custom_flag.active,
        "Compiler lost bindless array activity"
    );

    input.environment.SetDefine("MOER_TEST_REFLECTION_COLLISION", uint(1));
    RequireRejected([&] {
        ShaderCompiler::Compile(ShaderCompilerInput(input));
    });
}

} // namespace

int main() {
    try {
        CheckTypedAccess();
        CheckSerialization();
        CheckCompilerReflection();
        std::cout << "Shader parameter types, reflection and cache: success; sizeof(ReflectParamInfo)="
                  << sizeof(ReflectParamInfo) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
