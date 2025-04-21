#ifndef MOERENGINE_SHADER_COMMON_H
#define MOERENGINE_SHADER_COMMON_H
#include "misc/Hash.h"
#include "misc/STL.h"
// #include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include "resources/vertexfactory/VertexAttributes.h"
#include "misc/MacroUtils.h"
#include "serialize/Serializer.h"

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
class ShaderParametersMetadata {
public:
    /** A member of a shader parameter structure. */
    class Member {
    public:
        /** Initialization constructor. */
        Member(
            std::string_view                _name,
            std::string_view                _binding_type,
            uint32_t                        _struct_offset,
            uint32_t                        _stride,
            EShaderBindingBaseType          _base_type,
            EShaderPrecisionModifier        _type_precision,
            uint32_t                        _num_elements,
            const ShaderParametersMetadata* _p_struct_meta_data)
            : name(_name),
              binding_type(_binding_type),
              struct_offset(_struct_offset),
              stride(_stride),
              base_type(_base_type),
              type_precision(_type_precision),
              num_elements(_num_elements),
              p_struct_meta_data(_p_struct_meta_data) {
        }
        inline friend bool operator<(const Member& lhs, const uint32_t& rhs) {
            return lhs.struct_offset < rhs;
        }
        inline friend bool operator<(const uint32_t& lhs, const Member& rhs) {
            return lhs < rhs.struct_offset;
        }
        /** Returns the string of the name of the element or name of the array of elements. */
        std::string_view GetName() const { return name; }

        /** Returns the string of the type. */
        std::string_view GetShaderBindingTypeStr() const { return binding_type; }

        /** Returns the offset of the element in the shader parameter struct in bytes. */
        uint32_t GetOffset() const { return struct_offset; }

        /** Returns the stride of the element in the shader parameter struct in bytes. */
        uint32_t GetStride() const { return stride; }

        /** Returns the type of the elements, int, UAV... */
        EShaderBindingBaseType GetBaseType() const { return base_type; }

        /** Floating point the element is being stored. */
        EShaderPrecisionModifier GetPrecision() const { return type_precision; }

        /** Returns the number of elements in array, or 0 if this is not an array. */
        uint32_t GetNumElements() const { return num_elements; }

        /** Returns the metadata of the struct. */
        const ShaderParametersMetadata* GetStructMetadata() const { return p_struct_meta_data; }

        inline bool IsVariableNativeType() const {
            return base_type == SBT_INT32 ||
                   base_type == SBT_UINT32 ||
                   base_type == SBT_FLOAT32;
        }

        //        /** Returns the size of the member. */
        //        inline uint32_t GetMemberSize() const {
        //            uint32_t ElementSize = sizeof(uint32_t) * num_rows * num_columns;
        //
        //            /** If this an array, the alignment of the element are changed. */
        //            if (num_elements > 0) {
        //                return ((ElementSize - 1) / SHADER_PARAMETER_STRUCTURE_ALIGNMENT + 1) * SHADER_PARAMETER_STRUCTURE_ALIGNMENT * num_elements;
        //            }
        //            return ElementSize;
        //        }

        //        static RENDER_API void GenerateShaderParameterType(
        //            std::string&             Result,
        //            bool                     bSupportsPrecisionModifier,
        //            EShaderBindingBaseType   BaseType,
        //            EShaderPrecisionModifier PrecisionModifier,
        //            uint32_t                 NumRows,
        //            uint32_t                 NumColumns);
        //        RENDER_API void GenerateShaderParameterType(std::string& Result, bool bSupportsPrecisionModifier) const;
        //        RENDER_API void GenerateShaderParameterType(std::string& Result, EShaderPlatform ShaderPlatform) const;

    private:
        std::string_view name;
        std::string_view binding_type;

        EShaderBindingBaseType   base_type;
        EShaderPrecisionModifier type_precision;

        uint32_t                        struct_offset;
        uint32_t                        num_elements;
        const ShaderParametersMetadata* p_struct_meta_data;
        uint32_t                        stride;
    };

    RENDER_API ShaderParametersMetadata(
        EShaderParameterUseCase    _use_case,
        std::string_view           _struct_name,
        uint32_t                   _size,
        const Moer::Array<Member>& _members,
        bool                       _b_force_complete_initialization = false);

    RENDER_API virtual ~ShaderParametersMetadata();

    std::string_view GetStructTypeName() const { return struct_name; }

    uint32_t                GetSize() const { return size; }
    EShaderParameterUseCase GetUseCase() const { return use_case; }

    const RHIShaderRootParameterLayout& GetLayout() const {
        assert(IsLayoutInitialized());
        return *layout;
    }
    const RHIShaderRootParameterLayout* GetLayoutPtr() const {
        assert(IsLayoutInitialized());
        return layout;
    }

    const Moer::Array<Member>& GetMembers() const { return members; }

    /** Returns the full C++ member name from it's byte offset in the structure. */
    RENDER_API std::string_view GetMemberNameByOffset(uint16_t _member_offset) const;

    inline bool IsLayoutInitialized() const { return layout != nullptr; }

    void InitializeLayout();

private:
    /** Name of the structure type in C++ and shader code. */
    std::string_view struct_name;

    /** Size of the entire struct in bytes. */
    const uint32_t size;

    /** The use case of this shader parameter struct. */
    const EShaderParameterUseCase use_case;

    /** Layout of all the resources in the shader parameter struct. */
    RHIShaderRootParameterLayout* layout{};

    /** List of all members. */
    Moer::Array<Member> members;
};
namespace std {
    template<>
    struct less<ShaderParametersMetadata::Member> {
        bool operator()(const ShaderParametersMetadata::Member& lhs, const ShaderParametersMetadata::Member& rhs) const {
            return lhs.GetOffset() < rhs.GetOffset();
        }
        bool operator()(const ShaderParametersMetadata::Member& lhs, const uint32_t& rhs) const {
            return lhs.GetOffset() < rhs;
        }
        bool operator()(const uint32_t& lhs, const ShaderParametersMetadata::Member& rhs) const {
            return lhs < rhs.GetOffset();
        }
    };
}// namespace std
//compiled shader platform and type information

typedef uint32_t ShaderTypeIndex;
struct ShaderMutationParameters;
struct ShaderCompilerEnvironment;
/**
 * @brief Shader Type Meta Data
 * 
 */
class ShaderMetaType {
public:
    using ConstructShaderInstanceProc = std::function<class Shader*(const ShaderCompiledInitializer&)>;
    using ShouldCompileMutationProc   = std::function<bool(const ShaderMutationParameters&)>;
    using SetCompileEnvironmentProc   = std::function<void(const ShaderMutationParameters&, ShaderCompilerEnvironment&)>;

    RENDER_API ShaderMetaType(
        std::string_view                _type_name,
        std::string_view                _file_name,
        std::string_view                _entry_point,
        EShaderType                     _shader_type,
        uint32_t                        _type_size,
        const ShaderParametersMetadata* _parameter_data,
        uint32_t                        _total_mutation_count,
        ConstructShaderInstanceProc     _shader_type_constructor,
        ShouldCompileMutationProc       _should_compile_mutation,
        SetCompileEnvironmentProc       _set_compile_environment);
    RENDER_API ~ShaderMetaType();
    void OnRegistration();

    struct Parameters {
    };
    static RENDER_API void RegistrateShaderMetaType(ShaderMetaType* type);

    //may lead to unexpected behavior when name hash collision, only use for debug
    static RENDER_API ShaderMetaType* GetShaderMetaType(std::string_view _type_name);

    //standard function
    static RENDER_API ShaderMetaType* GetShaderMetaType(uint32_t _type_name_hash);

    /**
     * @brief Get the Shader Type Enum
     * 
     * @return EShaderType 
     */
    EShaderType GetShaderType() const { return shader_type; }

    std::string_view                GetName() const { return type_name; }
    uint32_t                        GetNameHash() const { return type_name_hash; }
    std::string_view                GetFileName() const { return file_name; }
    std::string_view                GetEntryPoint() const { return entry_point; }
    const ShaderParametersMetadata* GetParameterMetaData() const { return parameter_meta_data; }
    uint32_t                        GetTotalMutationCount() const { return total_mutation_count; }

    Shader* ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) { return construct_shader_instance(_initializer); }

    bool ShouldCompileMutation(const ShaderMutationParameters& _parameters) const { return should_compile_mutation(_parameters); }

    void SetCompileEnvironment(const ShaderMutationParameters& _parameters, ShaderCompilerEnvironment& _environment) {
        set_compile_environment(_parameters, _environment);
    };

private:
    static RENDER_API Moer::UnorderedMap<ShaderTypeKey, ShaderMetaType*>& GetNameToTypeMap();

    // shader type name in cpp
    std::string_view type_name;
    uint32_t         type_name_hash;
    // shader file name/relative path
    std::string_view file_name;
    // shader entry point
    std::string_view entry_point;
    // shader type enum, Vertex, Fragment .etc
    EShaderType shader_type;

    uint32_t total_mutation_count;

    // shader root parameter meta data
    const ShaderParametersMetadata* parameter_meta_data;
    // shader construct function, for sub class creation
    ConstructShaderInstanceProc construct_shader_instance;

    ShouldCompileMutationProc should_compile_mutation;

    SetCompileEnvironmentProc set_compile_environment;
};
/**
 * @brief Registrate All used shader types on launching,
    collect Shader meta data create function for later 
    shader type creation.
 * 
 */
class ShaderTypeRegistration {

public:
    RENDER_API                                            ShaderTypeRegistration(std::function<ShaderMetaType&()>);
    static Moer::Array<std::function<ShaderMetaType&()>>& GetRegistrations();
    static void                                           CollectRegistration(std::function<ShaderMetaType&()> _registration_func);

    static void SubmitRegistrations();
};

struct ShaderReflectInfo {
    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
    Moer::Array<RHIVertexInputInfo>                vertex_input_info;
};

namespace Moer {
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
};// namespace Moer
static bool operator==(const Moer::ShaderEntryKey& _lhs, const Moer::ShaderEntryKey& _rhs) noexcept {
    return _lhs.hash == _rhs.hash;
}
namespace std {
    template<>
    struct hash<Moer::ShaderEntryKey> {
        size_t operator()(const Moer::ShaderEntryKey& _key) const {
            return _key.hash;
        }
    };
}// namespace std
//Shader Compiled Hash
struct Shader {
    ShaderParametersInfoMap            reflection;
    uint32_t                           mutation_id;
    EShaderType                        type;
    Moer::StaticArray<Moer::uint64, 2> compiled_hash;
    Moer::uint64                       shader_name_hash;
    std::string                        entry_name;
    std::string                        shader_path;
    Moer::ShaderEntryKey               shader_key;
    //need included files to validate cache

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << compiled_hash << type << mutation_id << shader_name_hash << entry_name << shader_path << shader_key;
        _stream << reflection.reflect_map;
        return _stream;
    }

    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> compiled_hash >> type >> mutation_id >> shader_name_hash >> entry_name >> shader_path >> shader_key;
        _stream >> reflection.reflect_map;
        return _stream;
    }
};

static inline bool operator==(const Shader& _lhs, const Shader& _rhs) {
    return _lhs.compiled_hash == _rhs.compiled_hash && _lhs.type == _rhs.type && _lhs.mutation_id == _rhs.mutation_id && _lhs.shader_name_hash == _rhs.shader_name_hash && _lhs.entry_name == _rhs.entry_name && _lhs.shader_path == _rhs.shader_path;
}

namespace std {
    template<>
    struct hash<Shader> {
        size_t operator()(const Shader& _shader) const {
            size_t hash = _shader.compiled_hash[0];
            HashCombine(hash, _shader.compiled_hash[1]);
            return hash;
        }
    };
}// namespace std

struct ShaderCompilerOutput {

    ShaderCompilerOutput()
        : num_instructions(0),
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

    // ShaderCompilerOutput(ShaderCompilerOutput&&)                 = default;
    // ShaderCompilerOutput(const ShaderCompilerOutput&)            = default;
    // ShaderCompilerOutput& operator=(ShaderCompilerOutput&&)      = default;
    // ShaderCompilerOutput& operator=(const ShaderCompilerOutput&) = default;
};

/**
 * @brief ALL Compiled information needed for Shader Type Creation
 * 
 */
struct ShaderCompiledInitializer {
    const ShaderMetaType*          type_info;
    ShaderTargetInfo               target_info;
    const Moer::Array<uint8_t>&    compiled_code;
    const ShaderParametersInfoMap& parameter_map;
    const Hash64City&              compiled_hash;

    uint32_t   code_size;
    RENDER_API ShaderCompiledInitializer(
        const ShaderMetaType*       _shader_type,
        const ShaderCompilerOutput& _compiled_output
        //        const FVertexFactoryType* InVertexFactoryType
    );
};

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
    friend class ShaderCompilerEnvironment;
    Moer::UnorderedMap<std::string, std::string> defines;
};

template<typename TMaroType>
concept MacroType = requires(TMaroType _type) {
    std::is_same_v<TMaroType, uint32_t> ||
        std::is_same_v<TMaroType, int32_t> ||
        std::is_same_v<TMaroType, bool> ||
        std::is_same_v<TMaroType, float> ||
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

    const Moer::UnorderedMap<std::string, std::variant<uint32_t, int32_t, bool, float, std::string>>& GetCompilerArgs() const {
        return compiler_args;
    }

    static std::wstring GetVariantWStr(const std::variant<uint32_t, int32_t, bool, float, std::string>& _value) {
        return std::visit([]<typename T>(const T& e) {
            if constexpr (std::is_same_v<T, std::string>) {
                return std::wstring(e.begin(), e.end());
            } else {// float/int
                return std::to_wstring(e);
            }
        },
                          _value);
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

    const ShaderParametersMetadata* param_meta_data;
};

namespace Moer::Render {
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
        RENDER_API const VertexAttributesBitmask GetVertexAttributes() const { return mask; };
        RENDER_API bool                          IsShadowDepthPass() const { return is_shadow_depth_pass; };

    private:
        mutable VertexStream    stream{};
        VertexAttributesBitmask mask                 = 0;
        bool                    is_shadow_depth_pass = false;
    };

    static inline bool operator==(const VertexFactory& _lhs, const VertexFactory& _rhs) {
        return _lhs.GetVertexAttributes() == _rhs.GetVertexAttributes() && _lhs.IsShadowDepthPass() == _rhs.IsShadowDepthPass();
    }
}// namespace Moer::Render

namespace std {
    template<>
    struct hash<Moer::Render::VertexFactory> {
        size_t operator()(const Moer::Render::VertexFactory& _key) const {
            return GetHash(_key.GetVertexAttributes()) ^ GetHash(_key.IsShadowDepthPass());
        }
    };
}// namespace std

namespace Moer::Render {
    class RENDER_API VertexShader {
    public:
        template<typename TMacro>
        VertexShader(std::string_view _path, TMacro _mutation = {}, std::string_view _entry_name = "main")
            : shader_path(_path), entry_name(_entry_name) {
            _mutation.SetCompileEnvironment(src_environment);
            mutation_id = _mutation.GetMutationID();
        }

        VertexShader(std::string_view _path, std::string_view _entry_name = "main")
            : shader_path(_path), entry_name(_entry_name) {
        }

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
}// namespace Moer::Render

struct ShaderCompilerInput {
    ShaderTargetInfo          target_info;
    uint32_t                  mutation_id;
    std::string               entry_point;
    std::string               relative_source_file_path;
    std::string               shader_name;
    uint32_t                  shader_name_hash;
    ShaderCompilerEnvironment environment;

    Moer::OutputStream& operator<<(Moer::OutputStream& _stream) const {
        _stream << target_info << mutation_id << entry_point << relative_source_file_path << shader_name_hash << environment;
        return _stream;
    }

    Moer::InputStream& operator>>(Moer::InputStream& _stream) {
        _stream >> target_info >> mutation_id >> entry_point >> relative_source_file_path >> shader_name_hash >> environment;
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

namespace std {
    template<>
    struct hash<ShaderCompilerInput> {
        size_t operator()(const ShaderCompilerInput& _input) const {
            return std::hash<std::string_view>()(_input.relative_source_file_path) ^ std::hash<std::string_view>()(_input.entry_point) ^ std::hash<uint32_t>()(_input.mutation_id);
        }
    };
}// namespace std

FORCEINLINE EShaderPlatform GetShaderPlatformByRHIType(ERHIType _type) {
    switch (_type) {

        case ERHIType::Vulkan:
            return EShaderPlatform::SP_VULKAN_SM6;
        case ERHIType::D3D12:
            return EShaderPlatform::SP_WIN_D3D_SM6;
            break;
        default: assert(false && "not supported rhi");
    }
    return EShaderPlatform::SP_VULKAN_SM6;
}

/**
 * @brief for registrate shader compile jobs
 * 
 */
class ShaderCompileRegistration {
public:
    static void RegistrateCompileWorkIfNeed(const ShaderMetaType& _shader_type);

    static Moer::Array<ShaderCompileJobInput>& RetrieveShaderCompileWorks();
};

#endif//MOERENGINE_SHADER_COMMON_H
