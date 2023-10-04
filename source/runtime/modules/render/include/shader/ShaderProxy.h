#ifndef MOER_ENGINE_SHADER_PROXY_H
#define MOER_ENGINE_SHADER_PROXY_H
#include "ShaderParameterTypeInfo.h"
#include <array>
#include <vector>
#include "API_Macro.h"
#include "misc/Hash.h"
#include "unordered_map"
#include <cstring>

#pragma region forward
class VertexFactoryType;
#pragma endregion

struct ShaderReflectionInfo {
    struct UniformBufferEntry {
        std::string name;
        uint32_t    binding;
    };
};

//compiled shader output container for shader initialization

#define BEGIN_SHADER_PARAMETER_DEFINITION(StructureName)                                                                          \
    class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) StructureName {                                                           \
    public:                                                                                                                       \
        StructureName() {}                                                                                                        \
        /*struct TypeInfo {                                                                                                    \
            static constexpr int32_t s_num_rows                     = 1;                                                     \
            static constexpr int32_t s_num_columns                  = 1;                                                     \
            static constexpr int32_t s_num_elements                 = 0;                                                     \
            static constexpr int32_t alignment                      = sizeof(StructType);                                    \
            static constexpr bool    b_is_stored_in_constant_buffer = true;                                                  \
                                                                                                                             \
            using TParamPtr = StructureName; \
                                                                                                                                  \
        static const ShaderParametersMetadata* GetStructMetadata() {                                                              \
            return nullptr;                                                                                                       \
        }                                                                                                                         \
    };                                                                                                                            \
    */ \
                                                                                                                                  \
    private:                                                                                                                      \
        using TThisStruct = StructureName;                                                                                        \
        struct _firstMemberId {                                                                                                   \
            enum { HasDeclaredResource = 0 };                                                                                     \
        };                                                                                                                        \
        typedef void* (*MemberFunction)(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*);                          \
        static void* AppendMemberGetPrev(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*) { return nullptr; }      \
        typedef _firstMemberId

#define INTERNAL_DEFINE_SHADER_PARAM_IMPL(TypeInfo, MemberType, MemberName, HlslType, Precision, UBMTBaseType)             \
    MemberId##MemberName;                                                                                                  \
                                                                                                                           \
public:                                                                                                                    \
    /* a ptr wrapped shader param type  */                                                                                 \
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

#define DEFINE_SHADER_PARAM_UAV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIUnorderedAccessView*>, RHIUnorderedAccessView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_UAV)

#define DEFINE_SHADER_PARAM_SRV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIShaderResourceView*>, RHIShaderResourceView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SRV)

#define DEFINE_SHADER_PARAM_STRUCTURED(HLSLTYPE, MemberName)

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
    const auto&                   members = TestGlobalShader::Parameters::GetMembers();
}

#endif//MOER_ENGINE_SHADER_PROXY_H
