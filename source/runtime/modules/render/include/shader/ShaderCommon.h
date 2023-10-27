#ifndef MOERENGINE_SHADER_COMMON_H
#define MOERENGINE_SHADER_COMMON_H
#include "misc/Hash.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include <cstdint>
#include <functional>
#include <list>
#include <vector>
#include "misc/MacroUtils.h"

extern const char* g_global_shader_resource_root_dir;
extern const char* g_global_shader_resource_output_dir;

struct ShaderCompiledInitializer;

/**
 * @brief Binding Parameter Enum
 * 
 */
enum class EShaderParameterType : uint8_t {
    UNKNOWN,
    CONSTANT_STRUCT,
    CBV,
    SAMPLER,
    SRV,
    UAV,

    BINDLESS_RESOURCE_INDEX,
    BINDLESS_SAMPLER_INDEX,

    Num,
    NumBits = 4
};
inline bool IsParameterResource(EShaderParameterType _base_type) {

    switch (_base_type) {

        case EShaderParameterType::CBV:
        case EShaderParameterType::SAMPLER:
        case EShaderParameterType::SRV:
        case EShaderParameterType::UAV:
            return true;
        case EShaderParameterType::BINDLESS_RESOURCE_INDEX:
        case EShaderParameterType::BINDLESS_SAMPLER_INDEX:
        default: break;
    }
    return false;
}
BEGIN_ENUM_STR_DEFINITION(EShaderParameterType)

ENUM_STR_ELEMENT(UNKNOWN)
ENUM_STR_ELEMENT(CBV)
ENUM_STR_ELEMENT(SAMPLER)
ENUM_STR_ELEMENT(SRV)
ENUM_STR_ELEMENT(UAV)
ENUM_STR_ELEMENT(BINDLESS_RESOURCE_INDEX)
ENUM_STR_ELEMENT(BINDLESS_SAMPLER_INDEX)
END_ENUM_STR_DEFINITION(EShaderParameterType)

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
            const char*                     _name,
            const char*                     _binding_type,
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
        const char* GetName() const { return name; }

        /** Returns the string of the type. */
        const char* GetShaderBindingTypeStr() const { return binding_type; }

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

        //        static RHI_API void GenerateShaderParameterType(
        //            std::string&             Result,
        //            bool                     bSupportsPrecisionModifier,
        //            EShaderBindingBaseType   BaseType,
        //            EShaderPrecisionModifier PrecisionModifier,
        //            uint32_t                 NumRows,
        //            uint32_t                 NumColumns);
        //        RHI_API void GenerateShaderParameterType(std::string& Result, bool bSupportsPrecisionModifier) const;
        //        RHI_API void GenerateShaderParameterType(std::string& Result, EShaderPlatform ShaderPlatform) const;

    private:
        const char* name;
        const char* binding_type;

        EShaderBindingBaseType   base_type;
        EShaderPrecisionModifier type_precision;

        uint32_t                        struct_offset;
        uint32_t                        num_elements;
        const ShaderParametersMetadata* p_struct_meta_data;
        uint32_t                        stride;
    };

    RHI_API ShaderParametersMetadata(
        EShaderParameterUseCase    _use_case,
        const char*                _struct_name,
        uint32_t                   _size,
        const std::vector<Member>& _members,
        bool                       _b_force_complete_initialization = false);

    RHI_API virtual ~ShaderParametersMetadata();

    const char* GetStructTypeName() const { return struct_name; }

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

    const std::vector<Member>& GetMembers() const { return members; }

    /** Returns the full C++ member name from it's byte offset in the structure. */
    RHI_API std::string GetMemberNameByOffset(uint16_t _member_offset) const;

    inline bool IsLayoutInitialized() const { return layout != nullptr; }

    void InitializeLayout();

private:
    /** Name of the structure type in C++ and shader code. */
    const char* const struct_name;

    /** Size of the entire struct in bytes. */
    const uint32_t size;

    /** The use case of this shader parameter struct. */
    const EShaderParameterUseCase use_case;

    /** Layout of all the resources in the shader parameter struct. */
    RHIShaderRootParameterLayout* layout{};

    /** List of all members. */
    std::vector<Member> members;
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
struct alignas(4) ShaderTargetInfo {
    EShaderType     shader_type : ST_NumBits;
    EShaderPlatform shader_platform : SP_NumBits;
};

typedef uint32_t ShaderTypeIndex;

/**
 * @brief Shader Type Meta Data
 * 
 */
class ShaderMetaType {
public:
    using ConstructShaderInstanceProc = std::function<class Shader*(const ShaderCompiledInitializer&)>;

    ShaderMetaType(
        const char*                     _type_name,
        const char*                     _file_name,
        const char*                     _entry_point,
        EShaderType                     _shader_type,
        uint32_t                        _type_size,
        const ShaderParametersMetadata* _parameter_data,
        ConstructShaderInstanceProc     _shader_type_constructor);
    ~ShaderMetaType();
    void OnRegistration();

    struct Parameters {
    };
    static RENDER_CORE_API std::unordered_map<std::string, ShaderMetaType*>& GetNameToTypeMap();

    /**
     * @brief Get the Shader Type Enum
     * 
     * @return EShaderType 
     */
    EShaderType GetShaderType() const { return shader_type; }

    const char*                     GetName() const { return type_name; }
    const char*                     GetFileName() const { return file_name; }
    const char*                     GetEntryPoint() const { return entry_point; }
    const ShaderParametersMetadata* GetParameterMetaData() const { return parameter_meta_data; }

    Shader* ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) { return construct_shader_instance(_initializer); }

private:
    // shader type name in cpp
    const char* type_name;
    // shader file name/relative path
    const char* file_name;
    // shader entry point
    const char* entry_point;
    // shader type enum, Vertex, Fragment .etc
    EShaderType shader_type;
    // shader root parameter meta data
    const ShaderParametersMetadata* parameter_meta_data;
    // shader construct function, for sub class creation
    ConstructShaderInstanceProc construct_shader_instance;
};
/**
 * @brief Registrate All used shader types on launching,
    collect Shader meta data create function for later 
    shader type creation.
 * 
 */
class ShaderTypeRegistration {

public:
    ShaderTypeRegistration(std::function<ShaderMetaType&()>);
    static std::vector<std::function<ShaderMetaType&()>>& GetRegistrations();
    static void                                           CollectRegistration(std::function<ShaderMetaType&()> _registration_func);

    static void SubmitRegistrations();
};

/**
 * @brief Shader Root Parameter info
 * 
 */
struct ParameterInfo {
    // source pipeline stage
    ERHIPipelineStageFlags stage{ERHIPipelineStageFlags::PS_NONE};
    // slot in dx12 while binding in Vulkan
    int16_t slot : 8 = -1;
    // space in dx12 while set in Vulkan
    int16_t space : 8 = -1;
    // array size, invalid for root cbv
    int8_t num = 0;
    // parameter type enum
    EShaderParameterType type{EShaderParameterType::Num};
};
static_assert(sizeof(ParameterInfo) == 8);

/**
 * @brief Shader Reflected ParameterInfo Container,
    index with parameter name
 * 
 */
struct ShaderParametersInfoMap {
    friend class ShaderCompiler;
    friend class DXCompiler;

public:
    const std::unordered_map<std::string, ParameterInfo>& GetShaderParameterInfoMap() const {
        return param_map;
    }

private:
    std::unordered_map<std::string, ParameterInfo> param_map;
};
struct ShaderCompilerOutput {

    ShaderCompilerOutput()
        : num_instructions(0),
          num_samplers(0),
          compiled_time(0.0),
          preprocessing_time(0.0),
          b_succeeded(false) {}

    ShaderParametersInfoMap  parameter_map;
    std::vector<std::string> errors;
    std::vector<std::string> pragma;

    ShaderTargetInfo target_info;

    std::vector<uint8_t> shader_code;
    Hash64City           compiled_hash;
    uint32_t             num_instructions;
    uint32_t             num_samplers;
    double               compiled_time;
    double               preprocessing_time;
    bool                 b_succeeded;

    ShaderCompilerOutput(ShaderCompilerOutput&&)                 = default;
    ShaderCompilerOutput(const ShaderCompilerOutput&)            = default;
    ShaderCompilerOutput& operator=(ShaderCompilerOutput&&)      = default;
    ShaderCompilerOutput& operator=(const ShaderCompilerOutput&) = default;
};
/**
 * @brief ALL Compiled information needed for Shader Type Creation
 * 
 */
struct ShaderCompiledInitializer {
    const ShaderMetaType*          type_info;
    ShaderTargetInfo               target_info;
    const std::vector<uint8_t>&    compiled_code;
    const ShaderParametersInfoMap& parameter_map;
    const Hash64City&              compiled_hash;

    uint32_t        code_size;
    RENDER_CORE_API ShaderCompiledInitializer(
        const ShaderMetaType*       _shader_type,
        const ShaderCompilerOutput& _compiled_output
        //        const FVertexFactoryType* InVertexFactoryType
    );
};

struct ShaderCompilerInput {

    ShaderTargetInfo target_info;
    std::string      entry_point;
    std::string      relative_source_file_path;
    std::string      shader_name;

    const ShaderParametersMetadata* param_meta_data;
};

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

    static std::vector<ShaderCompilerInput>& RetrieveShaderCompileWorks();
};
#endif//MOERENGINE_SHADER_COMMON_H
