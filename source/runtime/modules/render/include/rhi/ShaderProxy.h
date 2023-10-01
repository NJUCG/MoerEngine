#ifndef MOER_ENGINE_SHADER_PROXY_H
#define MOER_ENGINE_SHADER_PROXY_H
#include "RHIResource.h"
#include <vector>
#include "API_Macro.h"
#include "misc/Hash.h"
#include "unordered_map"
#include <cstring>
#define SHADER_PARAMETER_STRUCTURE_ALIGNMENT 16

#pragma region forward
class ShaderPipelineType;
class VertexFactoryType;
struct ShaderCompilerOutput;

#pragma endregion
enum class EShaderParameterType : uint8_t {
    LOOSE_DATA,
    UNIFORM_BUFFER,
    SAMPLER,
    SRV,
    UAV,

    BINDLESS_RESOURCE_INDEX,
    BINDLESS_SAMPLER_INDEX,

    Num
};

enum class EShaderPrecisionModifier : uint8_t {
    FLOAT,
    HALF,
    FIXED,
    INVALID
};
ENUM_BIT_OP_IMPL(EShaderPrecisionModifier, )

struct RHIUniformBufferResourceInitializer {

    uint16_t member_offset;

    /** Type of the member that allow (). */
    EShaderBindingBaseType member_type;
    uint8_t                padding;

    /** Compare two uniform buffer layout resources. */
    friend inline bool operator==(const RHIUniformBufferResourceInitializer& A, const RHIUniformBufferResourceInitializer& B) {
        return A.member_offset == B.member_offset && A.member_type == B.member_type;
    }
};
struct RHIUniformBufferLayoutInitializer {
    RHIUniformBufferLayoutInitializer() = default;
    explicit RHIUniformBufferLayoutInitializer(const char* _name, uint32_t _buffer_size) : name(_name), buffer_size(_buffer_size) {
    }
    uint32_t    buffer_size;
    const char* name;

private:
    void ComputeHash() {
        //todo
    }

    uint32_t hash = 0;

public:
    std::vector<RHIUniformBufferResourceInitializer> inline_resources;
    std::vector<RHIUniformBufferResourceInitializer> reference_resources;

    uint32_t                        constant_buffer_size;
    uint16_t                        attachments_offset = std::numeric_limits<uint16_t>::max();
    UniformBufferGlobalBindingPoint static_slot        = MAX_UNIFORM_BUFFER_GLOBAL_BINDING_POINT;

    EUniformBufferBindingFlags binding_flags = EUniformBufferBindingFlags::SHADER;

    friend inline bool operator==(const RHIUniformBufferLayoutInitializer& lhs, const RHIUniformBufferLayoutInitializer& rhs) {
        return lhs.constant_buffer_size == rhs.constant_buffer_size && lhs.static_slot == rhs.static_slot && lhs.binding_flags == rhs.binding_flags && lhs.inline_resources == rhs.inline_resources;
    }
};

// per parameter allocation in global map
struct ShaderParameterAllocationInfo {
    uint16_t             buffer_index = 0;
    uint16_t             base_index   = 0;
    uint16_t             size         = 0;
    EShaderParameterType type{EShaderParameterType::Num};
    mutable bool         b_bound = false;

    ShaderParameterAllocationInfo() = default;
    ShaderParameterAllocationInfo(
        uint16_t             _buffer_index,
        uint16_t             _base_index,
        uint16_t             _size,
        EShaderParameterType _type) : buffer_index(_buffer_index),
                                      base_index(_base_index),
                                      size(_size),
                                      type(_type) {
    }
};
class ShaderParameterMap {
public:
    ShaderParameterMap() = default;

    std::optional<ShaderParameterAllocationInfo> FindShaderParameterAllocation(const std::string& _param_name) const;
    void                                         AddShaderParameterAllocation(const char* _param_name, uint16_t _buffer_index, uint16_t _base_index, uint16_t _size, EShaderParameterType _type);
    void                                         RemoveShaderParameterAllocation(const char* _param_name);

    inline void GetAllParamsNames(std::vector<std::string>& _out_names) const {
        for (const auto& params_pair : shader_parameters_map) {
            _out_names.push_back(params_pair.first);
        }
    }
    inline const std::unordered_map<std::string, ShaderParameterAllocationInfo>& GetShaderParameterMap() const {
        return shader_parameters_map;
    }

private:
    std::unordered_map<std::string, ShaderParameterAllocationInfo> shader_parameters_map;
};

//compiled shader platform and type information
struct alignas(4) ShaderTargetInfo {
    uint32_t shader_type : ST_NumBits;
    uint32_t shader_platform : SP_NumBits;
};

class ShaderType {
public:
    struct Parameters {
    };
};
struct ShaderReflectionInfo {
    struct UniformBufferEntry {
        std::string name;
        uint32_t    binding;
    };
};

//compiled shader output container for shader initialization
struct ShaderCompiledInfo {
    const ShaderType*           Type;
    ShaderTargetInfo            target_info;
    const std::vector<uint8_t>& compiled_code;
    const ShaderParameterMap&   ParameterMap;
    const SHA256Hash&           OutputHash;
    SHA256Hash                  MaterialShaderMapHash;
    const ShaderPipelineType*   ShaderPipeline;
    //    const VertexFactoryType* VertexFactoryType;
    uint32_t NumInstructions;
    uint32_t NumTextureSamplers;
    uint32_t CodeSize;
    int32_t  PermutationId;

    RHI_API ShaderCompiledInfo(
        const ShaderType*           _shader_type,
        const ShaderCompilerOutput& _out_compiler_output,
        const SHA256Hash&           _material_shader_map_hash,
        const ShaderPipelineType*   _shader_pipeline_type
        //        const FVertexFactoryType* InVertexFactoryType
    );
};

class Shader {
    friend class ShaderType;

public:
    RHI_API Shader();

    RHI_API Shader(const ShaderCompiledInfo& intializer);

    ~Shader();

private:
    ShaderType*      type;
    ShaderTargetInfo target_info;
    int32_t          resource_index;

    int32_t num_samplers;
    int32_t code_size;
};

class ShaderParametersMetadata {
public:
    /** The use case of the uniform buffer structures. */
    enum class EUseCase : uint8_t {
        /** Stand alone shader parameter struct used for render passes and shader parameters. */
        SHADER_PARAMETER_STRUCT,

        /** Uniform buffer definition authored at compile-time. */
        UNIFORM_BUFFER
    };

    /** Shader binding name of the uniform buffer that contains the root shader parameters. */
    static constexpr const char* s_root_shader_binding_name = "_RootShaderParameters";

    /** Shader binding name of the uniform buffer that contains the root shader parameters. */
    static constexpr int32_t s_root_constant_buffer_binding_index = 0;

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
              struct_offset(_struct_offset),
              base_type(_base_type),
              type_precision(_type_precision),
              num_elements(_num_elements),
              p_struct_meta_data(_p_struct_meta_data) {
            std::string temp(_binding_type);
            auto        binding_type_name = std::string(_binding_type).substr(0, temp.find_first_of('<'));
        }

        /** Returns the string of the name of the element or name of the array of elements. */
        const char* GetName() const { return name; }

        /** Returns the string of the type. */
        EShaderCodeResourceBindingType GetShaderBindingType() const { return binding_type; }

        /** Returns the string of the type. */
        const char* GetShaderBindingTypeStr() const { return ToString(binding_type); }

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
        const char*                     name;
        EShaderCodeResourceBindingType  binding_type;

        uint32_t                        struct_offset;
        EShaderBindingBaseType          base_type;
        EShaderPrecisionModifier        type_precision;

        uint32_t                        num_elements;
        const ShaderParametersMetadata* p_struct_meta_data;
    };

    RHI_API ShaderParametersMetadata(
        EUseCase                           _use_case,
        EUniformBufferBindingFlags         _binding_flags,
        const char*                        _layout_name,
        const char*                        _struct_name,
        const char*                        _shader_variable_name,
        const char*                        _static_slot_name,
        const char*                        _file_name,
        const int32_t                      _file_line,
        uint32_t                           _size,
        const std::vector<Member>&         _members,
        bool                               _b_force_complete_initialization = false,
        RHIUniformBufferLayoutInitializer* _out_layout_initializer          = nullptr,
        uint32_t                           _usage_flags                     = 0);

    RHI_API virtual ~ShaderParametersMetadata();

    RHI_API void GetNestedStructs(std::vector<const ShaderParametersMetadata*>& _out_nested_structs) const;

    const char*       GetStructTypeName() const { return struct_name; }
    const char*       GetShaderVariableName() const { return shader_variable_name; }
    const SHA256Hash& GetShaderVariableHashedName() const { return shader_variable_hash_name; }
    const char*       GetStaticSlotName() const { return static_slot_name; }

    bool HasStaticSlot() const { return static_slot_name != nullptr; }

    EUniformBufferBindingFlags GetBindingFlags() const { return binding_flags; }

    EUniformBufferBindingFlags GetPreferredBindingFlag() const {
        // Decay to static when both binding flags are specified.
        return binding_flags != EUniformBufferBindingFlags::ALL ? binding_flags : EUniformBufferBindingFlags::STATIC;
    }

    /** Returns the C++ file name where the parameter structure is declared. */
    const char* GetFileName() const { return file_name; }

    /** Returns the C++ line number where the parameter structure is declared. */
    const int32_t GetFileLine() const { return file_line; }

    uint32_t GetSize() const { return size; }
    EUseCase GetUseCase() const { return use_case; }
    //    inline bool IsLayoutInitialized() const { return Layout != nullptr; }
    uint32_t GetUsageFlags() const { return usage_flags; }

    //    const RHIUniformBufferLayout& GetLayout() const
    //    {
    //        assert(IsLayoutInitialized());
    //        return *Layout;
    //    }
    //    const RHIUniformBufferLayout* GetLayoutPtr() const
    //    {
    //        assert(IsLayoutInitialized());
    //        return Layout;
    //    }
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


    /** Returns a hash about the entire layout of the structure. */
    uint32_t GetLayoutHash() const {
        assert(use_case == EUseCase::SHADER_PARAMETER_STRUCT || use_case == EUseCase::UNIFORM_BUFFER);
        //        assert(IsLayoutInitialized());
        return layout_hash;
    }

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
    const char* const layout_name;

    /** Name of the structure type in C++ and shader code. */
    const char* const struct_name;

    /** Name of the shader variable name for global shader parameter structs. */
    const char* const shader_variable_name;

    /** Name of the static slot to use for the uniform buffer (or null). */
    const char* const static_slot_name;

    SHA256Hash shader_variable_hash_name;

    /** Name of the C++ file where the parameter structure is declared. */
    const char* const file_name;

    /** Line in the C++ file where the parameter structure is declared. */
    const int32_t file_line;

    /** Size of the entire struct in bytes. */
    const uint32_t size;

    /** The use case of this shader parameter struct. */
    const EUseCase use_case;

    /** The binding model used by this parameter struct. */
    const EUniformBufferBindingFlags binding_flags;

    /** Layout of all the resources in the shader parameter struct. */
    //    UniformBufferLayoutRHIRef Layout{};

    /** List of all members. */
    std::vector<Member> members;

    /** Shackle elements in global link list of globally named shader parameters. */
    //    TLinkedList<FShaderParametersMetadata*> GlobalListLink;

    /** Hash about the entire memory layout of the structure. */
    uint32_t layout_hash = 0;

    /** Additional flags for how to use the buffer */
    uint32_t usage_flags = 0;

    RHI_API void InitializeLayout(RHIUniformBufferLayoutInitializer* OutLayoutInitializer = nullptr);
};

template<typename TPtr>
class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) TShaderParameterPtr {
public:
    TShaderParameterPtr() {}

    TShaderParameterPtr(const TPtr& Other)
        : ref(Other) {}

    TShaderParameterPtr(const TShaderParameterPtr<TPtr>& Other)
        : ref(Other.ref) {}

    FORCEINLINE void operator=(const TPtr& Other) {
        ref = Other;
    }

    FORCEINLINE operator TPtr&() {
        return ref;
    }

    FORCEINLINE operator const TPtr&() const {
        return ref;
    }

    FORCEINLINE const TPtr& operator->() const {
        return ref;
    }

private:
    TPtr ref;
};

template<typename ShaderResourceType>
struct TShaderResourceParameterTypeInfo {
    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool    b_is_stored_in_constant_buffer = false;

    using TParamPtr = TShaderParameterPtr<ShaderResourceType>;

    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }

    static_assert(sizeof(TParamPtr) == SHADER_PARAMETER_STRUCTURE_ALIGNMENT, "Uniform buffer layout must not be platform dependent.");
};

template<class UniformBufferStructType>
struct TShaderParameterStructureTypeInfo
{
    static constexpr int32_t s_num_rows = 1;
    static constexpr int32_t s_num_columns = 1;
    static constexpr int32_t s_num_elements = 0;
    static constexpr int32_t alignment = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool b_is_stored_in_constant_buffer = true;

    using TParamPtr = TShaderParameterPtr<UniformBufferStructType>;

    static const ShaderParametersMetadata* GetStructMetadata() { return UniformBufferStructType::GetStructMetadata(); }
};

template<>
struct TShaderParameterStructureTypeInfo<float>
{
    static constexpr int32_t s_num_rows = 1;
    static constexpr int32_t s_num_columns = 1;
    static constexpr int32_t s_num_elements = 0;
    static constexpr int32_t alignment = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool b_is_stored_in_constant_buffer = true;

    using TParamPtr = TShaderParameterPtr<float>;

    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};
// shader->bind("ubo", value);

#define BEGIN_SHADER_PARAMETER_DEFINITION(StructureName)                                                                     \
    class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) StructureName {                                                      \
    public:                                                                                                                  \
        StructureName() {}                                                                                                   \
                                                                                                                             \
    private:                                                                                                                 \
        using TThisStruct = StructureName;                                                                                   \
        struct _firstMemberId {                                                                                              \
            enum { HasDeclaredResource = 0 };                                                                                \
        };                                                                                                                   \
        typedef void* (*MemberFunction)(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*);                     \
        static void* AppendMemberGetPrev(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*) { return nullptr; } \
        typedef _firstMemberId

#define INTERNAL_DEFINE_SHADER_PARAM_IMPL(TypeInfo, MemberType, MemberName, HlslType, Precision, UBMTBaseType)             \
    MemberId##MemberName;                                                                                                  \
                                                                                                                           \
public:                                                                                                                    \
    /* a ptr wrapped shader param type  */    \
    TypeInfo::TParamPtr MemberName = nullptr;                                                                              \
                                                                                                                           \
private:                                                                                                                   \
    struct _nextMemberId##MemberName {                                                                                     \
        enum { HasDeclaredResource = MemberId##MemberName::HasDeclaredResource };                                          \
    };                                                                                                                     \
    static void* AppendMemberGetPrev(_nextMemberId##MemberName, std::vector<ShaderParametersMetadata::Member>* _members) { \
                                                                                                                           \
        _members->push_back(ShaderParametersMetadata::Member(                                                              \
            #MemberName,                                                                                                   \
            #HlslType,                                                                                                     \
            offsetof(TThisStruct, MemberName),                                                                             \
            UBMTBaseType,                                                                                                  \
            Precision,                                                                                                     \
            TypeInfo::s_num_elements,                                                                                      \
            TypeInfo::GetStructMetadata()));                                                                               \
        void* (*PrevFunc)(MemberId##MemberName, std::vector<ShaderParametersMetadata::Member>*);                           \
        PrevFunc = AppendMemberGetPrev;                                                                                    \
        return (void*)PrevFunc;                                                                                            \
    }                                                                                                                      \
    typedef _nextMemberId##MemberName

#define END_SHADER_PARAMETER_DEFINITION(StructureName)                                    \
    lastMemberId;                                                                         \
                                                                                          \
public:                                                                                   \
    static std::vector<ShaderParametersMetadata::Member> GetMembers() {                   \
        std::vector<ShaderParametersMetadata::Member> _members;                           \
        void* (*_lastFunc)(lastMemberId, std::vector<ShaderParametersMetadata::Member>*); \
        _lastFunc = AppendMemberGetPrev;                                                  \
        void* Ptr = (void*)_lastFunc;                                                     \
        do {                                                                              \
            Ptr = reinterpret_cast<MemberFunction>(Ptr)(_firstMemberId(), &_members);     \
        } while (Ptr);                                                                    \
        std::reverse(_members.begin(), _members.end());                                   \
        return _members;                                                                  \
    }                                                                                     \
    }                                                                                     \
    ;

// float [] 1
// float [] 2
// float [] 3

#define DEFINE_SHADER_PARAM_UAV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIUnorderedAccessView*>, RHIUnorderedAccessView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_UAV)

#define DEFINE_SHADER_PARAM_SRV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIShaderResourceView*>, RHIShaderResourceView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SRV)

#define DEFINE_SHADER_PARAM_STRUCTURED(HLSLTYPE, MemberName) \


class ShaderBase {
    ShaderBase();
    ~ShaderBase();
};

class TestGlobalShader : public ShaderBase {

public:
    BEGIN_SHADER_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_UAV(RWBuffer2D, buffer2d)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer, sbo)

    END_SHADER_PARAMETER_DEFINITION(Parameters)
};
/*
 *  uav v1 (register 0);
 *  srv s1 (register 1);
 *  constant buffer
 * */
void test() {
    TestGlobalShader::Parameters* pass;
    const auto& members = TestGlobalShader::Parameters::GetMembers();

}

#endif//MOER_ENGINE_SHADER_PROXY_H
