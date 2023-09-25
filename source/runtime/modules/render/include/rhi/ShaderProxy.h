#ifndef MOER_ENGINE_SHADER_PROXY_H
#define MOER_ENGINE_SHADER_PROXY_H
#include "RHIResource.h"
#include <vector>
#include "API_Macro.h"
#include "misc/Hash.h"
#include "unordered_map"
#define SHADER_PARAMETER_STRUCTURE_ALIGNMENT 16
enum class EShaderParameterType : uint8_t {
    LooseData,
    UniformBuffer,
    Sampler,
    SRV,
    UAV,

    Bindless_ResourceIndex,
    Bindless_SamplerIndex,

    Num
};

enum class EShaderPrecisionModifier : uint8_t {
    Float,
    Half,
    Fixed,
    Invalid
};
ENUM_BIT_OP_IMPL(EShaderPrecisionModifier, )
struct RHIUniformBufferMemberInitializer {

    uint16_t member_offset;

    /** Type of the member that allow (). */
    EUniformBufferBaseType member_type;
    uint8_t                padding;

    /** Compare two uniform buffer layout resources. */
    friend inline bool operator==(const RHIUniformBufferMemberInitializer& A, const RHIUniformBufferMemberInitializer& B) {
        return A.member_offset == B.member_offset && A.member_type == B.member_type;
    }
};
class ShaderParametersMetadata {
public:
    /** The use case of the uniform buffer structures. */
    enum class EUseCase : uint8_t {
        /** Stand alone shader parameter struct used for render passes and shader parameters. */
        ShaderParameterStruct,

        /** Uniform buffer definition authored at compile-time. */
        UniformBuffer,

        /** Uniform buffer generated from assets, such as material parameter collection or Niagara. */
        DataDrivenUniformBuffer,
    };

    /** Additional flags that can be used to determine usage */
    enum class EUsageFlags : uint8_t {
        None = 0,

        /** On platforms that support emulated uniform buffers, disable them for this uniform buffer */
        NoEmulatedUniformBuffer = 1 << 0,
    };

    /** Shader binding name of the uniform buffer that contains the root shader parameters. */
    static constexpr const char* kRootUniformBufferBindingName = "_RootShaderParameters";

    /** Shader binding name of the uniform buffer that contains the root shader parameters. */
    static constexpr int32_t kRootCBufferBindingIndex = 0;

    /** A member of a shader parameter structure. */
    class Member {
    public:
        /** Initialization constructor. */
        Member(
            const char*                     InName,
            const char*                     InShaderType,
            int32_t                         InFileLine,
            uint32_t                        InOffset,
            EUniformBufferBaseType          InBaseType,
            EShaderPrecisionModifier        InPrecision,
            uint32_t                        InNumRows,
            uint32_t                        InNumColumns,
            uint32_t                        InNumElements,
            const ShaderParametersMetadata* InStruct)
            : Name(InName), ShaderType(InShaderType), FileLine(InFileLine), Offset(InOffset), BaseType(InBaseType), Precision(InPrecision), NumRows(InNumRows), NumColumns(InNumColumns), NumElements(InNumElements), Struct(InStruct) {
        }

        /** Returns the string of the name of the element or name of the array of elements. */
        const char* GetName() const { return Name; }

        /** Returns the string of the type. */
        const char* GetShaderType() const { return ShaderType; }

        /** Returns the C++ line number where the parameter is declared. */
        int32_t GetFileLine() const { return int32_t(FileLine); }

        /** Returns the offset of the element in the shader parameter struct in bytes. */
        uint32_t GetOffset() const { return Offset; }

        /** Returns the type of the elements, int, UAV... */
        EUniformBufferBaseType GetBaseType() const { return BaseType; }

        /** Floating point the element is being stored. */
        EShaderPrecisionModifier GetPrecision() const { return Precision; }

        /** Returns the number of row in the element. For instance FMatrix would return 4, or FVector would return 1. */
        uint32_t GetNumRows() const { return NumRows; }

        /** Returns the number of column in the element. For instance FMatrix would return 4, or FVector would return 3. */
        uint32_t GetNumColumns() const { return NumColumns; }

        /** Returns the number of elements in array, or 0 if this is not an array. */
        uint32_t GetNumElements() const { return NumElements; }

        /** Returns the metadata of the struct. */
        const ShaderParametersMetadata* GetStructMetadata() const { return Struct; }

        inline bool IsVariableNativeType() const {
            return BaseType == UBMT_INT32 ||
                   BaseType == UBMT_UINT32 ||
                   BaseType == UBMT_FLOAT32;
        }

        /** Returns the size of the member. */
        inline uint32_t GetMemberSize() const {
            uint32_t ElementSize = sizeof(uint32_t) * NumRows * NumColumns;

            /** If this an array, the alignment of the element are changed. */
            if (NumElements > 0) {
                return ((ElementSize - 1) / SHADER_PARAMETER_STRUCTURE_ALIGNMENT + 1) * SHADER_PARAMETER_STRUCTURE_ALIGNMENT * NumElements;
            }
            return ElementSize;
        }

        static RHI_API void GenerateShaderParameterType(
            std::string&             Result,
            bool                     bSupportsPrecisionModifier,
            EUniformBufferBaseType   BaseType,
            EShaderPrecisionModifier PrecisionModifier,
            uint32_t                 NumRows,
            uint32_t                 NumColumns);
        RHI_API void GenerateShaderParameterType(std::string& Result, bool bSupportsPrecisionModifier) const;
        RHI_API void GenerateShaderParameterType(std::string& Result, EShaderPlatform ShaderPlatform) const;

    private:
        const char*                     Name;
        const char*                     ShaderType;
        int32_t                         FileLine;
        uint32_t                        Offset;
        EUniformBufferBaseType          BaseType;
        EShaderPrecisionModifier        Precision;
        uint32_t                        NumRows;
        uint32_t                        NumColumns;
        uint32_t                        NumElements;
        const ShaderParametersMetadata* Struct;
    };

    /** Initialization constructor.
	 *
	 * EUseCase::UniformBuffer are listed in the global GetStructList() that will be visited at engine startup to know all the global uniform buffer
	 * that can generate code in /Engine/Generated/GeneratedUniformBuffers.ush. Their initialization will be finished during the this list
	 * traversal. bForceCompleteInitialization force to ignore the list for EUseCase::UniformBuffer and instead handle it like a standalone non
	 * globally listed EUseCase::ShaderParameterStruct. This is required for the ShaderCompileWorker to deserialize them without side global effects.
	 */
    RHI_API ShaderParametersMetadata(
        EUseCase                           UseCase,
        EUniformBufferBindingFlags         InBindingFlags,
        const char*                        InLayoutName,
        const char*                        InStructTypeName,
        const char*                        InShaderVariableName,
        const char*                        InStaticSlotName,
        const char*                        InFileName,
        const int32_t                      InFileLine,
        uint32_t                           InSize,
        const std::vector<Member>&         InMembers,
        bool                               bForceCompleteInitialization = false,
        RHIUniformBufferMemberInitializer* OutLayoutInitializer         = nullptr,
        uint32_t                           InUsageFlags                 = 0);

    RHI_API virtual ~ShaderParametersMetadata();

    RHI_API void GetNestedStructs(std::vector<const ShaderParametersMetadata*>& OutNestedStructs) const;

    const char*       GetStructTypeName() const { return StructTypeName; }
    const char*       GetShaderVariableName() const { return ShaderVariableName; }
    const SHA256Hash& GetShaderVariableHashedName() const { return ShaderVariableHashedName; }
    const char*       GetStaticSlotName() const { return StaticSlotName; }

    bool HasStaticSlot() const { return StaticSlotName != nullptr; }

    EUniformBufferBindingFlags GetBindingFlags() const { return BindingFlags; }

    EUniformBufferBindingFlags GetPreferredBindingFlag() const {
        // Decay to static when both binding flags are specified.
        return BindingFlags != EUniformBufferBindingFlags::ALL ? BindingFlags : EUniformBufferBindingFlags::STATIC;
    }

    /** Returns the C++ file name where the parameter structure is declared. */
    const char* GetFileName() const { return FileName; }

    /** Returns the C++ line number where the parameter structure is declared. */
    const int32_t GetFileLine() const { return FileLine; }

    uint32_t GetSize() const { return Size; }
    EUseCase GetUseCase() const { return UseCase; }
    //    inline bool IsLayoutInitialized() const { return Layout != nullptr; }
    uint32_t GetUsageFlags() const { return UsageFlags; }

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
    const std::vector<Member>& GetMembers() const { return Members; }

#if WITH_EDITOR
    inline bool                IsUniformBufferDeclarationInitialized() const { return UniformBufferDeclaration.IsValid(); }
    FThreadSafeSharedStringPtr GetUniformBufferDeclarationPtr() const { return UniformBufferDeclaration; }
    const std::string&         GetUniformBufferDeclaration() const { return *UniformBufferDeclaration; }
    FORCEINLINE const std::string& GetUniformBufferPath() const { return UniformBufferPath; }
    FORCEINLINE const std::string& GetUniformBufferInclude() const { return UniformBufferInclude; }
    FORCEINLINE uint32_t           GetUniformBufferPathHash() const { return UniformBufferPathHash; }
#endif// WITH_EDITOR

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

    /** Initialize all the global shader parameter structs. */
    static RHI_API void InitializeAllUniformBufferStructs();

    /** Returns a hash about the entire layout of the structure. */
    uint32_t GetLayoutHash() const {
        assert(UseCase == EUseCase::ShaderParameterStruct || UseCase == EUseCase::UniformBuffer);
        //        assert(IsLayoutInitialized());
        return LayoutHash;
    }

    /** Iterate recursively over all FShaderParametersMetadata. */
    template<typename TParameterFunction>
    void IterateStructureMetadataDependencies(TParameterFunction Lambda) const {
        for (const ShaderParametersMetadata::Member& Member : Members) {
            const ShaderParametersMetadata* NewParametersMetadata = Member.GetStructMetadata();

            if (NewParametersMetadata) {
                NewParametersMetadata->IterateStructureMetadataDependencies(Lambda);
            }
        }

        Lambda(this);
    }

private:
    const char* const LayoutName;

    /** Name of the structure type in C++ and shader code. */
    const char* const StructTypeName;

    /** Name of the shader variable name for global shader parameter structs. */
    const char* const ShaderVariableName;

    /** Name of the static slot to use for the uniform buffer (or null). */
    const char* const StaticSlotName;

    SHA256Hash ShaderVariableHashedName;

    /** Name of the C++ file where the parameter structure is declared. */
    const char* const FileName;

    /** Line in the C++ file where the parameter structure is declared. */
    const int32_t FileLine;

    /** Size of the entire struct in bytes. */
    const uint32_t Size;

    /** The use case of this shader parameter struct. */
    const EUseCase UseCase;

    /** The binding model used by this parameter struct. */
    const EUniformBufferBindingFlags BindingFlags;

    /** Layout of all the resources in the shader parameter struct. */
    //    UniformBufferLayoutRHIRef Layout{};

    /** List of all members. */
    std::vector<Member> Members;

    /** Shackle elements in global link list of globally named shader parameters. */
    //    TLinkedList<FShaderParametersMetadata*> GlobalListLink;

    /** Hash about the entire memory layout of the structure. */
    uint32_t LayoutHash = 0;

    /** Additional flags for how to use the buffer */
    uint32_t UsageFlags = 0;

    RHI_API void InitializeLayout(RHIUniformBufferMemberInitializer* OutLayoutInitializer = nullptr);

#if WITH_EDITOR
    RHI_API void InitializeUniformBufferDeclaration();
#endif
};

template<typename PtrType>
class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) TAlignedShaderParameterPtr
{
public:
    TAlignedShaderParameterPtr()
    { }

    TAlignedShaderParameterPtr(const PtrType& Other)
        : Ref(Other)
    { }

    TAlignedShaderParameterPtr(const TAlignedShaderParameterPtr<PtrType>& Other)
        : Ref(Other.Ref)
    { }

    FORCEINLINE void operator=(const PtrType& Other)
    {
        Ref = Other;
    }

    FORCEINLINE operator PtrType&()
    {
        return Ref;
    }

    FORCEINLINE operator const PtrType&() const
    {
        return Ref;
    }

    FORCEINLINE const PtrType& operator->() const
    {
        return Ref;
    }

private:
    PtrType Ref;
//#if !PLATFORM_64BITS
//    uint32_t _Padding;
//    static_assert(sizeof(void*) == 8, "Wrong PLATFORM_64BITS settings.");
//#endif
//
//    static_assert(sizeof(PtrType) == sizeof(void*), "T should be a pointer.");
};

template<typename ShaderResourceType>
struct TShaderResourceParameterTypeInfo
{
    static constexpr int32_t s_num_rows = 1;
    static constexpr int32_t s_num_columns = 1;
    static constexpr int32_t s_num_elements = 0;
    static constexpr int32_t Alignment = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool bIsStoredInConstantBuffer = false;

    using AlignStruct = TAlignedShaderParameterPtr<ShaderResourceType>;

    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }

    static_assert(sizeof(AlignStruct) == SHADER_PARAMETER_STRUCTURE_ALIGNMENT, "Uniform buffer layout must not be platform dependent.");
};

#define BEGIN_SHADER_PARAMETER_DEFINITION(StructureName)                                                                      \
    class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) StructureName {                                                       \
    public:                                                                                                                   \
        StructureName() {}                                                                                                    \
        struct TypeInfo {                                                                                                     \
            static constexpr int32_t s_num_rows     = 1;                                                                      \
            static constexpr int32_t s_num_columns = 1;                                                                      \
            static constexpr int32_t s_num_elements = 0;                                                                      \
            static constexpr int32_t s_alignment    = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;                                   \
            using AlignStruct                       = StructureName;                                                          \
            static const ShaderParametersMetadata* GetStructMetadata() {                                                      \
                return nullptr;                                                                                               \
            }                                                                                                                 \
        };                                                                                                                    \
                                                                                                                              \
    private:                                                                                                                  \
        using TThisStruct = StructureName;                                                                                    \
        struct _firstMemberId {                                                                                               \
            enum { HasDeclaredResource = 0 };                                                                                 \
        };                                                                                                                    \
        typedef void* (*MemberFunction)(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*);                      \
        static void* AppendMemberGetPrev(_firstMemberId, std::vector < ShaderParametersMetadata::Member>*) { return nullptr; } \
        typedef _firstMemberId

#define DEFINE_SHADER_PARAM_UAV(TypeInfo, MemberType, MemberName)                                                            \
    MemberId##MemberName;                                                                                          \
                                                                                                                   \
public:                                                                                                            \
    TypeInfo::AlignStruct MemberName=nullptr;                                                                 \
                                                                                                                   \
private:                                                                                                           \
    struct _nextMemberId##MemberName {                                                                             \
        enum { HasDeclaredResource = MemberId##MemberName::HasDeclaredResource };                                  \
    };                                                                                                             \
    static void* AppendMemberGetPrev(_nextMemberId##MemberName, std::vector<ShaderParametersMetadata::Member>* _members) { \
        /*static_assert(offsetof(TThisStruct, MemberName) & (TypeInfo::s_num_columnes - 1) == 0, "");*/                \
        _members->push_back(ShaderParametersMetadata::Member(                                                      \
            #MemberName,                                                                                           \
            (const char*)"x",                                                                                      \
            __LINE__,                                                                                              \
            offsetof(TThisStruct, MemberName),                                                                     \
            UBMT_FLOAT32,                                                                                          \
            EShaderPrecisionModifier::Float,                                                                       \
            TypeInfo::s_num_rows,                                                                                  \
            TypeInfo::s_num_columns,                                                                              \
            TypeInfo::s_num_elements,                                                                              \
            TypeInfo::GetStructMetadata()));                                                                       \
        void* (*PrevFunc)(MemberId##MemberName, std::vector<ShaderParametersMetadata::Member>*);                   \
        PrevFunc = AppendMemberGetPrev;                                                                            \
        return (void*)PrevFunc;                                                                                    \
    }                                                                                                              \
    typedef _nextMemberId##MemberName

#define END_SHADER_PARAMETER_DEFINITION(StructureName) \
    lastMemberId;                                      \
    }                                                  \
    ;



class ShaderBase {
    ShaderBase();
    ~ShaderBase();
};

class GlobalShader : public ShaderBase {

        BEGIN_SHADER_PARAMETER_DEFINITION(Parameters)

        DEFINE_SHADER_PARAM_UAV(TShaderResourceParameterTypeInfo<RHIUnorderedAccessView*>, RHIUnorderedAccessView*, buffer2d)

        END_SHADER_PARAMETER_DEFINITION(Parameters)

};

#endif//MOER_ENGINE_SHADER_PROXY_H
