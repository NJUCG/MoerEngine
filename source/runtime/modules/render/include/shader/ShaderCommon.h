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

enum class EShaderParameterType : uint8_t {
    LOOSE_DATA,
    CBV,
    SAMPLER,
    SRV,
    UAV,

    BINDLESS_RESOURCE_INDEX,
    BINDLESS_SAMPLER_INDEX,

    Num
};
BEGIN_ENUM_STR_DEFINITION(EShaderParameterType)

ENUM_STR_ELEMENT(LOOSE_DATA)
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

/** The use case of the global buffer structures. */
enum class EShaderParameterUseCase : uint8_t {
    /** Stand alone shader parameter struct used for render passes and shader parameters. */
    SHADER_ROOT_PARAMETERS,

    SHADER_CONSTANTS,

    SHADER_SRV_TABLE,

    SHADER_UAV_TABLE,

    SHADER_CBV_TABLE,

    SHADER_UNIFORM_STRUCT
};

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
            EShaderBindingBaseType          _base_type,
            EShaderPrecisionModifier        _type_precision,
            uint32_t                        _num_elements,
            const ShaderParametersMetadata* _p_struct_meta_data)
            : name(_name),
              binding_type(_binding_type),
              struct_offset(_struct_offset),
              base_type(_base_type),
              type_precision(_type_precision),
              num_elements(_num_elements),
              p_struct_meta_data(_p_struct_meta_data) {
        }

        /** Returns the string of the name of the element or name of the array of elements. */
        const char* GetName() const { return name; }

        /** Returns the string of the type. */
        const char* GetShaderBindingTypeStr() const { return binding_type; }

        /** Returns the offset of the element in the shader parameter struct in bytes. */
        uint32_t GetOffset() const { return struct_offset; }

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

        uint32_t                 struct_offset;
        EShaderBindingBaseType   base_type;
        EShaderPrecisionModifier type_precision;

        uint32_t                        num_elements;
        const ShaderParametersMetadata* p_struct_meta_data;
    };

    RHI_API ShaderParametersMetadata(
        EShaderParameterUseCase           _use_case,
        EGlobalBufferBindingFlags         _binding_flags,
        const char*                       _struct_name,
        uint32_t                          _size,
        const std::vector<Member>&        _members,
        bool                              _b_force_complete_initialization = false,
        RHIGlobalBufferLayoutInitializer* _out_layout_initializer          = nullptr);

    RHI_API virtual ~ShaderParametersMetadata();

    RHI_API void GetNestedStructs(std::vector<const ShaderParametersMetadata*>& _out_nested_structs) const;

    const char* GetStructTypeName() const { return struct_name; }

    EGlobalBufferBindingFlags GetBindingFlags() const { return binding_flags; }

    EGlobalBufferBindingFlags GetPreferredBindingFlag() const {
        // Decay to static when both binding flags are specified.
        return binding_flags != EGlobalBufferBindingFlags::ALL ? binding_flags : EGlobalBufferBindingFlags::STATIC;
    }

    uint32_t                GetSize() const { return size; }
    EShaderParameterUseCase GetUseCase() const { return use_case; }

    const RHIGlobalBufferLayout& GetLayout() const {
        assert(IsLayoutInitialized());
        return *layout;
    }
    const RHIGlobalBufferLayout* GetLayoutPtr() const {
        assert(IsLayoutInitialized());
        return layout;
    }
    const std::vector<Member>& GetMembers() const { return members; }

    /** Find a member for a given offset. */
    RHI_API void FindMemberFromOffset(
        uint16_t                                 MemberOffset,
        const ShaderParametersMetadata**         OutContainingStruct,
        const ShaderParametersMetadata::Member** OutMember,
        int32_t*                                 ArrayElementId,
        std::string*                             NamePrefix) const;

    /** Returns the full C++ member name from it's byte offset in the structure. */
    RHI_API std::string GetFullMemberCodeName(uint16_t MemberOffset) const;

    //    static RHI_API TLinkedList<FShaderParametersMetadata*>*& GetStructList();
    //    /** Speed up finding the uniform buffer by its name */
    //    static RHI_API std::map<SHA256Hash, FShaderParametersMetadata*>& GetNameStructMap();
    inline bool IsLayoutInitialized() const { return layout != nullptr; }

    /** Iterate recursively over all FShaderParametersMetadata. */
    template<typename TParameterFunction>
    void IterateStructureMetadataDependencies(TParameterFunction Lambda) const {
        for (const ShaderParametersMetadata::Member& Member : members) {
            const ShaderParametersMetadata* NewParametersMetadata = Member.GetStructMetadata();

            if (NewParametersMetadata) {
                NewParametersMetadata->IterateStructureMetadataDependencies(Lambda);
            }
        }

        Lambda(this);
    }

private:
    /** Name of the structure type in C++ and shader code. */
    const char* const struct_name;

    /** Size of the entire struct in bytes. */
    const uint32_t size;

    /** The use case of this shader parameter struct. */
    const EShaderParameterUseCase use_case;

    /** The binding model used by this parameter struct. */
    const EGlobalBufferBindingFlags binding_flags;

    /** Layout of all the resources in the shader parameter struct. */
    RHIGlobalBufferLayoutRef layout{};

    /** List of all members. */
    std::vector<Member> members;

    RHI_API void InitializeLayout(RHIGlobalBufferLayoutInitializer* OutLayoutInitializer = nullptr);
};

//compiled shader platform and type information
struct alignas(4) ShaderTargetInfo {
    EShaderType     shader_type : ST_NumBits;
    EShaderPlatform shader_platform : SP_NumBits;
};

typedef uint32_t ShaderTypeIndex;

//contains parameter info
class ShaderMetaType {
public:
    ShaderMetaType(
        const char*                     _type_name,
        const char*                     _file_name,
        const char*                     _entry_point,
        EShaderType                     _shader_type,
        uint32_t                        _type_size,
        const ShaderParametersMetadata* _parameter_data);
    ~ShaderMetaType();
    void OnRegistration();

    struct Parameters {
    };
    static RENDER_CORE_API std::unordered_map<std::string, ShaderMetaType*>& GetNameToTypeMap();

    const HashedName GetNameHash() const { return hash_type_name; }
    EShaderType      GetShaderType() const { return shader_type; }
    const HashedName GetFileNameHash() const { return hash_file_name; }

    const char*                     GetName() const { return type_name; }
    const char*                     GetFileName() const { return file_name; }
    const char*                     GetEntryPoint() const { return entry_point; }
    const ShaderParametersMetadata* GetParameterMetaData() const { return parameter_meta_data; }

private:
    //todo: currently only support one file one shader, this field for multiple shader single file
    const char*                     type_name;
    HashedName                      hash_type_name;
    const char*                     file_name;
    HashedName                      hash_file_name;
    const char*                     entry_point;
    EShaderType                     shader_type;
    ShaderTypeIndex                 type_index;
    const ShaderParametersMetadata* parameter_meta_data;
};

class ShaderTypeRegistration {

public:
    ShaderTypeRegistration(std::function<ShaderMetaType&()>);
    static std::vector<std::function<ShaderMetaType&()>>& GetRegistrations();
    static void                                           CollectRegistration(std::function<ShaderMetaType&()> _registration_func);

    static void SubmitRegistrations();
};

class ShaderPipelineType {
    enum class Type : uint8_t {
        Graphics,
        Mesh
    };

public:
    virtual ~ShaderPipelineType() {}
    ShaderPipelineType(
        const char*           _name,
        const ShaderMetaType* _vertex_shader_info,
        const ShaderMetaType* _geometry_shader_info,
        const ShaderMetaType* _fragment_shader_info);

    ShaderPipelineType(
        const char*           _name,
        const ShaderMetaType* _vertex_shader_info,
        const ShaderMetaType* _geometry_shader_info);

    friend uint32_t GetHash(const ShaderPipelineType* _p_value) { return _p_value == nullptr ? 0 : _p_value->hash_index; }

    //todo: currently support only global shader
    bool              IsGlobalShaderPipeline() const { return shader_stages[0]; }
    const Hash64City& GetSourceCodehash(EShaderPlatform _platform) const;

    const char* GetName() const { return name; }
    Hash64City  hash_name;
    Hash64City  hash_file_name;

    std::array<ShaderMetaType*, ST_Num> shader_stages;

    uint32_t    hash_index;
    bool        b_initialized;
    const char* name;
};
// per parameter allocation in global map
struct ParameterInfo {
    int16_t                slot : 8  = -1;
    int16_t                space : 8 = -1;
    EShaderParameterType   type{EShaderParameterType::Num};
    ERHIPipelineStageFlags stage{ERHIPipelineStageFlags::PS_NONE};
};
struct ShaderParametersInfoMap {
    friend class ShaderCompiler;

public:
    const std::unordered_map<std::string, ParameterInfo>& GetShaderParameterInfoMap() {
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

struct ShaderCompiledInfo {
    const ShaderMetaType*          type_info;
    ShaderTargetInfo               target_info;
    const std::vector<uint8_t>&    compiled_code;
    const ShaderParametersInfoMap& parameter_map;
    const Hash64City&              output_hash;
    Hash64City                     material_shader_map_hash;
    const ShaderPipelineType*      shader_pipeline;
    //    const VertexFactoryType* VertexFactoryType;
    uint32_t num_instructions;
    uint32_t num_texture_samplers;
    uint32_t code_size;
    int32_t  permutation_id;

    RENDER_CORE_API ShaderCompiledInfo(
        const ShaderMetaType*       _shader_type,
        const ShaderCompilerOutput& _compiled_output,
        const Hash64City&           _material_shader_map_hash,
        const ShaderPipelineType*   _shader_pipeline_type
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
}

class ShaderCompileRegistration {
public:
    static void RegistrateCompileWorkIfNeed(const ShaderMetaType& _shader_type);

    static std::vector<ShaderCompilerInput>& RetrieveShaderCompileWorks();
};
#endif//MOERENGINE_SHADER_COMMON_H
