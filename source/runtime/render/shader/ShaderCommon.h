#ifndef MOERENGINE_SHADER_COMMON_H
#define MOERENGINE_SHADER_COMMON_H
#include "misc/Hash.h"
#include "misc/STL.h"
// #include "rhi/RHI.h"
#include "misc/MacroUtils.h"
#include "resources/vertexfactory/VertexAttributes.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "serialize/Serializer.h"
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

// extern const char* g_global_shader_resource_root_dir;
// extern const char* g_global_shader_resource_output_dir;

struct ShaderCompiledInitializer;
using ShaderTypeKey = uint32_t;

enum class EShaderPrecisionModifier : uint8_t {
    FLOAT,
    HALF,
    FIXED,
    INVALID
};
ENUM_BIT_OP_IMPL(EShaderPrecisionModifier)

/** The use case of parameter struct. */
enum class EShaderParameterUseCase : uint8_t {
    /** Stand alone shader parameter struct used for render passes and shader parameters. */
    SHADER_ROOT_PARAMETERS,

    /** For Structured buffer creation */
    SHADER_CONSTANT_STRUCT
};
/**
 * @brief Shader Param meta data for Shader Root Parameters and Structured Buffer Struct
 * 
 */

//compiled shader platform and type information

struct ShaderReflectInfo {
    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
};

namespace Moer::Render {
struct ShaderEntry {
    EShaderType     type;
    EShaderPlatform platform;
    Array<uint8_t>  blob_data;

    OutputStream& operator<<(OutputStream& _stream) const {
        _stream << type << platform << blob_data;
        return _stream;
    }

    InputStream& operator>>(InputStream& _stream) {
        _stream >> type >> platform >> blob_data;
        return _stream;
    }
};
struct ShaderEntryKey {
    uint64 hash;

    bool operator==(const ShaderEntryKey& _rhs) const noexcept {
        return hash == _rhs.hash;
    }
};
}; // namespace Moer::Render
static bool
operator==(const Moer::Render::ShaderEntryKey& _lhs, const Moer::Render::ShaderEntryKey& _rhs) noexcept {
    return _lhs.hash == _rhs.hash;
}
namespace std {
template<>
struct hash<Moer::Render::ShaderEntryKey> {
    size_t operator()(const Moer::Render::ShaderEntryKey& _key) const {
        return _key.hash;
    }
};
} // namespace std
//Shader Compiled Hash
namespace Moer::Render {

// 记录一个 shader 源文件（主文件或 include 文件）的路径和编译时时间戳，
// 用于缓存失效判断：当文件时间戳比缓存中的更新时，对应 shader 需要重编译。
struct ShaderFileDependency {
    std::string path;
    long long   timestamp = 0;

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << path << timestamp;
        return _stream;
    }
    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> path >> timestamp;
        return _stream;
    }
};

struct Shader {
    ShaderParametersInfoMap            reflection;
    uint32_t                           mutation_id;
    EShaderType                        type;
    Moer::StaticArray<Moer::uint64, 2> compiled_hash;
    Moer::uint64                       shader_name_hash;
    std::string                        entry_name;
    std::string                        shader_path;
    ShaderEntryKey                     shader_key;

    // 主文件 + 所有 #include 文件的路径和编译时时间戳
    Moer::Array<ShaderFileDependency>  source_dependencies;

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << compiled_hash << type << mutation_id << shader_name_hash << entry_name << shader_path
                << shader_key;
        _stream << reflection.reflect_map;
        _stream << source_dependencies;
        return _stream;
    }

    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> compiled_hash >> type >> mutation_id >> shader_name_hash >> entry_name >> shader_path >>
            shader_key;
        _stream >> reflection.reflect_map;
        _stream >> source_dependencies;
        return _stream;
    }
};

static inline bool operator==(const Shader& _lhs, const Shader& _rhs) {
    return _lhs.compiled_hash == _rhs.compiled_hash && _lhs.type == _rhs.type &&
           _lhs.mutation_id == _rhs.mutation_id && _lhs.shader_name_hash == _rhs.shader_name_hash &&
           _lhs.entry_name == _rhs.entry_name && _lhs.shader_path == _rhs.shader_path;
}

struct ShaderCompilerOutput {

    ShaderCompilerOutput() :
        num_instructions(0),
        num_samplers(0),
        compiled_time(0.0),
        preprocessing_time(0.0),
        b_succeeded(false) {}

    ShaderParametersInfoMap  parameter_map;
    Moer::Array<std::string> errors;
    Moer::Array<std::string> pragma;

    ShaderTargetInfo target_info;

    Moer::Array<uint8_t> shader_code;
    uint64_t             compiled_hash1;
    uint64_t             compiled_hash2;
    uint32_t             num_instructions;
    uint32_t             num_samplers;
    uint32_t             mutation_id;
    double               compiled_time;
    double               preprocessing_time;
    bool                 b_succeeded;
    uint32_t             shader_name_hash;

    bool      cached                      = false;
    long long source_file_last_write_time = 0;

    // 主文件 + 所有 #include 文件的路径和编译时时间戳（由 DXC TrackingIncludeHandler 收集）
    Moer::Array<ShaderFileDependency> source_dependencies;
};

/**
 * @brief ALL Compiled information needed for Shader Type Creation
 * 
 */

class ShaderCompilerDefines {
public:
    ShaderCompilerDefines() {
        defines.reserve(32);
    }

    void SetDefine(std::string_view _key, const std::string& _value) {
        auto& value = defines[_key.data()];
        value       = _value;
    }

    void SetDefine(std::string_view _key, const char* _value) {
        auto& value = defines[_key.data()];
        value       = _value;
    }

    void SetDefine(std::string_view _key, const int32_t _value) {
        auto& value = defines[_key.data()];
        value       = std::to_string(_value);
    }

    void SetDefine(std::string_view _key, const uint32_t _value) {
        auto& value = defines[_key.data()];
        value       = std::to_string(_value);
    }

    void SetDefine(std::string_view _key, const float _value) {
        auto& value = defines[_key.data()];
        value       = std::to_string(_value);
    }

    void SetDefine(std::string_view _key, const bool _value) {
        auto& value = defines[_key.data()];
        value       = std::to_string(_value);
    }

    void Merge(const ShaderCompilerDefines& _other) {

        defines.insert(_other.defines.begin(), _other.defines.end());
    }

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << defines;
        return _stream;
    }
    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> defines;
        return _stream;
    }

    bool operator==(const ShaderCompilerDefines& _other) const {
        return defines == _other.defines;
    }

private:
    friend struct ShaderCompilerEnvironment;
    Moer::UnorderedMap<std::string, std::string> defines;
};

template<typename TMaroType>
concept MacroType = requires(TMaroType _type) {
    std::is_same_v<TMaroType, uint32_t> || std::is_same_v<TMaroType, int32_t> ||
        std::is_same_v<TMaroType, bool> || std::is_same_v<TMaroType, float> ||
        std::is_same_v<TMaroType, std::string>;
};
struct ShaderCompilerEnvironment {
public:
    ShaderCompilerEnvironment() {
        compiler_args.reserve(32);
    }
    template<MacroType TValue>
    void SetDefine(std::string_view _key, const TValue& _value) {
        macro_defines.SetDefine(_key, _value);
    }
    void Merge(const ShaderCompilerEnvironment& _other) {
        compiler_args.insert(_other.compiler_args.begin(), _other.compiler_args.end());
        macro_defines.Merge(_other.macro_defines);
    }
    template<MacroType TValue>
    void SetCompileArg(std::string_view _key, const TValue& _value) {
        compiler_args[_key.data()] = _value;
    }

    bool HasCompileArg(std::string_view _key) const {
        return compiler_args.count(_key.data()) > 0;
    }
    const Moer::UnorderedMap<std::string, std::string>& GetDefines() const {
        return macro_defines.defines;
    }

    const Moer::UnorderedMap<std::string, std::variant<uint32_t, int32_t, bool, float, std::string>>&
    GetCompilerArgs() const {
        return compiler_args;
    }

    static std::wstring GetVariantWStr(const std::variant<uint32_t, int32_t, bool, float, std::string>& _value
    ) {
        return std::visit(
            []<typename T>(const T& e) {
                if constexpr (std::is_same_v<T, std::string>) {
                    return std::wstring(e.begin(), e.end());
                } else { // float/int
                    return std::to_wstring(e);
                }
            },
            _value
        );
    }

    std::string ToString() const {
        std::string str = "Defines: ";
        for (auto& [key, value] : macro_defines.defines) {
            str += "[" + key + " " + value + "] ";
        }
        return str;
    }

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << compiler_args << macro_defines;
        return _stream;
    }

    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> compiler_args >> macro_defines;
        return _stream;
    }

    bool operator==(const ShaderCompilerEnvironment& _other) const {
        return GetDefines() == _other.GetDefines() && GetCompilerArgs() == _other.GetCompilerArgs();
    }

private:
    ShaderCompilerDefines macro_defines;

    Moer::UnorderedMap<std::string, std::variant<uint32_t, int32_t, bool, float, std::string>> compiler_args;
};

struct ShaderCompileJobInput {
    ShaderTargetInfo target_info;
    std::string_view entry_point;
    std::string_view relative_source_file_path;
    std::string_view shader_name;
    uint32_t         shader_name_hash;
    uint32_t         mutation_count;
};

struct VertexFactory {

public:
    RENDER_API VertexFactory(VertexAttributesBitmask _mask, bool _is_shadow_depth_pass);

    VertexFactory()                                = default;
    VertexFactory(const VertexFactory&)            = default;
    VertexFactory& operator=(const VertexFactory&) = default;

    VertexFactory(VertexFactory&&)            = default;
    VertexFactory& operator=(VertexFactory&&) = default;

    RENDER_API void                          SetCompileEnvironment(ShaderCompilerEnvironment& _env);
    RENDER_API const VertexStream&           GetVertexStream() const;
    RENDER_API const VertexAttributesBitmask GetVertexAttributes() const {
        return mask;
    };
    RENDER_API bool IsShadowDepthPass() const {
        return is_shadow_depth_pass;
    };

private:
    mutable VertexStream    stream{};
    VertexAttributesBitmask mask                 = 0;
    bool                    is_shadow_depth_pass = false;
};

static inline bool operator==(const VertexFactory& _lhs, const VertexFactory& _rhs) {
    return _lhs.GetVertexAttributes() == _rhs.GetVertexAttributes() &&
           _lhs.IsShadowDepthPass() == _rhs.IsShadowDepthPass();
}
} // namespace Moer::Render

namespace std {
template<>
struct hash<Moer::Render::VertexFactory> {
    size_t operator()(const Moer::Render::VertexFactory& _key) const {
        return GetHash(_key.GetVertexAttributes()) ^ GetHash(_key.IsShadowDepthPass());
    }
};
} // namespace std
namespace std {
template<>
struct hash<Shader> {
    size_t operator()(const Moer::Render::Shader& _shader) const {
        size_t hash = _shader.compiled_hash[0];
        HashCombine(hash, _shader.compiled_hash[1]);
        return hash;
    }
};
} // namespace std

namespace Moer::Render {
template<typename T>
concept is_shader_mutation =
    requires(T _t) { _t.SetCompileEnvironment(std::declval<ShaderCompilerEnvironment&>()); };
class RENDER_API VertexShader {
public:
    template<is_shader_mutation TMacro>
    VertexShader(std::string_view _path, TMacro _mutation, std::string_view _entry_name = "main") :
        shader_path(_path),
        entry_name(_entry_name) {
        _mutation.SetCompileEnvironment(src_environment);
        mutation_id = _mutation.GetMutationID();
    }

    VertexShader(std::string_view _path, std::string_view _entry_name = "main") :
        shader_path(_path),
        entry_name(_entry_name) {}

    Shader& GetShader(Moer::Render::VertexFactory* _factory);

    template<typename TMacro>
    Shader& GetShader(Moer::Render::VertexFactory* _factory, TMacro _mut) {
        return GetShader(_factory, _mut, entry_name);
    }

private:
    std::string               shader_path;
    std::string               entry_name;
    ShaderCompilerEnvironment src_environment;
    uint                      mutation_id = 0;

    UnorderedMap<VertexFactory, Shader&> shader_map;
};

struct ShaderCompilerInput {
    ShaderTargetInfo          target_info;
    uint32_t                  mutation_id;
    std::string               entry_point;
    std::string               relative_source_file_path;
    std::string               shader_name;
    uint32_t                  shader_name_hash;
    ShaderCompilerEnvironment environment;

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << target_info << mutation_id << entry_point << relative_source_file_path << shader_name_hash
                << environment;
        return _stream;
    }

    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> target_info >> mutation_id >> entry_point >> relative_source_file_path >>
            shader_name_hash >> environment;
        return _stream;
    }
};

// clang-format off
static inline bool operator==(const ShaderCompilerInput& _lhs, const ShaderCompilerInput& _rhs) {
    return
        _lhs.relative_source_file_path == _rhs.relative_source_file_path
        && _lhs.entry_point == _rhs.entry_point
        && _lhs.shader_name_hash == _rhs.shader_name_hash
        && _lhs.mutation_id == _rhs.mutation_id
        && _lhs.environment == _rhs.environment;
}
// clang-format on
} // namespace Moer::Render

namespace std {
template<>
struct hash<Moer::Render::ShaderCompilerInput> {
    size_t operator()(const Moer::Render::ShaderCompilerInput& _input) const {
        return std::hash<std::string_view>()(_input.relative_source_file_path) ^
               std::hash<std::string_view>()(_input.entry_point) ^ std::hash<uint32_t>()(_input.mutation_id);
    }
};
} // namespace std

FORCEINLINE EShaderPlatform GetShaderPlatformByRHIType(ERHIType _type) {
    switch (_type) {

        case ERHIType::Vulkan:
            return EShaderPlatform::SP_VULKAN_SM6;
        case ERHIType::D3D12:
            return EShaderPlatform::SP_WIN_D3D_SM6;
            break;
        default:
            assert(false && "not supported rhi");
    }
    return EShaderPlatform::SP_VULKAN_SM6;
}

#endif //MOERENGINE_SHADER_COMMON_H
