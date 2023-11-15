#ifndef MOER_ENGINE_SHADER_PROXY_H
#define MOER_ENGINE_SHADER_PROXY_H
#include "rhi/RHIResource.h"
#include "shader/ShaderParameterTypeInfo.h"
#include <array>
#include <cstddef>
#include <functional>
#include <vector>
#include "API_Macro.h"
#include "misc/Hash.h"
#include "unordered_map"
#include <cstring>

#pragma region forward
class VertexFactoryType;
#pragma endregion

// uint32_t constexpr STRUCT_RESERVE_SIZE = sizeof(uint32_t);
// uint32_t constexpr STRUCT_DIRTY_BIT    = 1 << 31;

// #define INNER_SET_STRUCT_MEMBER_VALUE_IMPL(MemberType, MemberName) \
//     inline void Set_##MemberName(const MemberType& _value) {       \
//         if (_value != MemberName) {                                \
//             MemberName = _value;                                   \
//             reserved |= STRUCT_DIRTY_BIT;                          \
//         }                                                          \
//     }                                                              \
//     //                                                                \
//     // inline void Set##MemberName(MemberType _value) {               \
//     //     if (_value != MemberName) {                                \
//     //         MemberName = _value;                                   \
//     //         reserved |= STRUCT_DIRTY_BIT;                          \
//     //     }                                                          \
//     // }

#define INNER_GET_STRUCTURE_METADATA_IMPL(StructureName, ParameterUsage) \
    {                                                                    \
        static ShaderParametersMetadata s_struct_metadata(               \
            ParameterUsage,                                              \
            #StructureName,                                              \
            sizeof(StructureName),                                       \
            StructureName::GetMembers());                                \
        return &s_struct_metadata;                                       \
    }

#define BEGIN_ROOT_PARAMETER_DEFINITION(StructureName) \
    INNER_BEGIN_SHADER_PARAMETER_DEFINITION(StructureName, INNER_GET_STRUCTURE_METADATA_IMPL(StructureName, EShaderParameterUseCase::SHADER_ROOT_PARAMETERS), TRUE)

#define END_ROOT_PARAMETER_DEFINITION(StructureName) \
    lastMemberId;                                    \
    END_SHADER_PARAMETER_DEFINITION(StructureName, IS_ROOT)

#define INNER_DEFINE_GET_ROOT_PARAMETER(StructureName, IS_ROOT) \
    static const ShaderParametersMetadata* GetParametersMetaData();

#define INNER_IMPLEMENT_GET_ROOT_PARAMETER(StructureName, IS_ROOT)   \
    static const ShaderParametersMetadata* GetParametersMetaData() { \
        return StructureName::TypeInfo::GetStructMetadata();         \
    }
/**
 * @brief Uniform buffer table, means descriptor set layout which contains constant buffers, or CBV table in D3D12
 * 
 */
#define BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(StructureName) \
    INNER_BEGIN_SHADER_PARAMETER_DEFINITION(StructureName, INNER_GET_STRUCTURE_METADATA_IMPL(StructureName, EShaderParameterUseCase::SHADER_CONSTANT_STRUCT), )

#define END_SHADER_CONSTANT_STRUCT_DEFINITION(StructureName) \
    lastMemberId;                                            \
    END_SHADER_PARAMETER_DEFINITION(StructureName, )

#define INNER_BEGIN_SHADER_PARAMETER_DEFINITION(StructureName, GetStructMetadataFunctionImpl, ...)                           \
    class alignas(SHADER_PARAMETER_STRUCTURE_ALIGNMENT) StructureName {                                                      \
    public:                                                                                                                  \
        StructureName() { memset(this, 0, sizeof(StructureName)); }                                                          \
        /* struct type info*/                                                                                                \
        struct TypeInfo {                                                                                                    \
            static constexpr int32_t s_num_rows                     = 1;                                                     \
            static constexpr int32_t s_num_columns                  = 1;                                                     \
            static constexpr int32_t s_num_elements                 = 0;                                                     \
            static constexpr int32_t alignment                      = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;                  \
            static constexpr bool    b_is_stored_in_constant_buffer = true;                                                  \
                                                                                                                             \
            using TParamPtr = StructureName;                                                                                 \
                                                                                                                             \
            static const ShaderParametersMetadata* GetStructMetadata() GetStructMetadataFunctionImpl                         \
        };                                                                                                                   \
                                                                                                                             \
    private:                                                                                                                 \
        using TThisStruct = StructureName;                                                                                   \
        struct _firstMemberId {                                                                                              \
            enum { HasDeclaredResource = 0 };                                                                                \
        };                                                                                                                   \
        typedef void* (*MemberFunction)(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*);                     \
        static void* AppendMemberGetPrev(_firstMemberId, std::vector<ShaderParametersMetadata::Member>*) { return nullptr; } \
        typedef _firstMemberId

#define INTERNAL_DEFINE_SHADER_PARAM_IMPL(MemberTypeInfo, MemberType, MemberName, HlslType, Precision, UBMTBaseType, MemberScope, PublicDefs) \
    MemberId##MemberName;                                                                                                                     \
    MemberScope: /* a ptr wrapped shader param type  */                                                                                       \
    MemberTypeInfo::TParamPtr MemberName;                                                                                                     \
                                                                                                                                              \
public:                                                                                                                                       \
    PublicDefs                                                                                                                                \
                                                                                                                                              \
        private : struct _nextMemberId##MemberName {                                                                                          \
        enum { HasDeclaredResource = MemberId##MemberName::HasDeclaredResource };                                                             \
    };                                                                                                                                        \
    static void* AppendMemberGetPrev(_nextMemberId##MemberName, std::vector<ShaderParametersMetadata::Member>* _members) {                    \
                                                                                                                                              \
        _members->push_back(ShaderParametersMetadata::Member(                                                                                 \
            #MemberName,                                                                                                                      \
            #HlslType,                                                                                                                        \
            offsetof(TThisStruct, MemberName),                                                                                                \
            sizeof(MemberTypeInfo::TParamPtr),                                                                                                \
            UBMTBaseType,                                                                                                                     \
            Precision,                                                                                                                        \
            MemberTypeInfo::s_num_elements,                                                                                                   \
            MemberTypeInfo::GetStructMetadata()));                                                                                            \
        void* (*PrevFunc)(MemberId##MemberName, std::vector<ShaderParametersMetadata::Member>*);                                              \
                                                                                                                                              \
        PrevFunc = AppendMemberGetPrev;                                                                                                       \
        return (void*)PrevFunc;                                                                                                               \
    }                                                                                                                                         \
    typedef _nextMemberId##MemberName

#define END_SHADER_PARAMETER_DEFINITION(StructureName, ...)                               \
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
    ;                                                                                     \
    __VA_OPT__(INNER_IMPLEMENT_GET_ROOT_PARAMETER(StructureName, __VA_ARGS__))

#define DEFINE_SHADER_PARAM_CBV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIUnorderedAccessView*>, RHIConstantBufferView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_CBV, public, )

#define DEFINE_SHADER_PARAM_UAV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIUnorderedAccessView*>, RHIUnorderedAccessView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_UAV, public, )

#define DEFINE_SHADER_PARAM_SRV(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIShaderResourceView*>, RHIShaderResourceView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SRV, public, )

#define DEFINE_SHADER_PARAM_SRV_ARRAY(HLSLType, MemberName, NumElements) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIShaderResourceView* [NumElements]>, RHIShaderResourceView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SRV, public, )

#define DEFINE_SHADER_PARAM_SAMPLER(HLSLType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHISampler*>, RHISampler*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SAMPLER, public, )

#define DEFINE_SHADER_PARAM_SAMPLER_ARRAY(HLSLType, MemberName, NumElements) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHISampler* [NumElements]>, RHISampler*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SAMPLER, public, )

// #define DEFINE_SHADER_PARAM_SET(StructType, MemberName) \
//     INTERNAL_DEFINE_SHADER_PARAM_IMPL(StructType::TypeInfo, StructType, MemberName, , EShaderPrecisionModifier::FLOAT, SBT_NESTED_STRUCT)

#define DEFINE_SHADER_PARAM_STRUCT(StructType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderParameterStructureTypeInfo<StructType>, StructType, MemberName, StructType, EShaderPrecisionModifier::FLOAT, SBT_CONST_STRUCT, public, )

#define DEFINE_SHADER_PARAM(MemberType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderParameterTypeInfo<MemberType>, MemberType, MemberName, , EShaderPrecisionModifier::FLOAT, TShaderParameterTypeInfo<MemberType>::BaseType, public, )

// #define DEFINE_SHADER_PARAM_ATTACHMENT_BINDING() \
//     INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderParameterTypeInfo<AttachmentBindingSlots>, AttachmentBindingSlots, Attachments, , EShaderPrecisionModifier::FLOAT, SBT_ATTACHMENT_BINDING_SLOTS, )

#define DEFINE_SHADER_ROOT_PARAM_SRV(StructType, MemberName) \
    INTERNAL_DEFINE_SHADER_PARAM_IMPL(TShaderResourceParameterTypeInfo<RHIShaderResourceView*>, RHIShaderResourceView*, MemberName, HLSLType, EShaderPrecisionModifier::FLOAT, SBT_SRV, )

/*
 *  uav v1 (register 0);
 *  srv s1 (register 1);
 *  constant buffer
 * */

#endif//MOER_ENGINE_SHADER_PROXY_H
