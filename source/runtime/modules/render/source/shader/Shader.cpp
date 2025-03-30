#include "shader/Shader.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"

#include <regex>
#include <cstring>
class TestShaderClass : Shader {
public:
    MUTATION_SPARSE_UINT(TestInts, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    DEFINE_MUTATION_SET(TestInts);
    DEFINE_SHADER_TYPE(TestShaderClass, Global, RENDER_API)

    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_SRV(Buffer, bar)
    DEFINE_SHADER_PARAM_SRV(AccelerationStructure, as)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestReflectionShader, "TestVert.hlsl", "main", EShaderType::ST_VERTEX);

Shader::Shader(){};

Shader::Shader(const ShaderCompiledInitializer& initializer)
    : type(initializer.type_info),
      target_info(initializer.target_info),
      code_size(initializer.code_size),
      compiled_hash(initializer.compiled_hash) {
    //truncated hashkey for other usages
    memcpy(&hash_key, &compiled_hash, sizeof(hash_key));
    ConstructRootParameterLayoutInfo(initializer.parameter_map);
};

Shader::~Shader(){

};

const Hash64City& Shader::GetCompiledHash() const {
    return compiled_hash;
};

namespace Utils {
    EShaderCodeResourceBindingType BindingTypeStrToEnum(std::string_view _binding_type_str) {
        std::regex binding_type_regex("(RW)?(ConstantBuffer"
                                      "|StructuredBuffer(<([a-zA-Z]+)>)?"
                                      "|ByteAddressBuffer"
                                      "|Texture2D(<([a-zA-Z]+)>)?"
                                      "|Texture2DArray<([a-zA-Z]+)>"
                                      "|Texture3D<([a-zA-Z]+)>"
                                      "|TextureCube<([a-zA-Z]+)>"
                                      "|TextureCubeArray<([a-zA-Z]+)>"
                                      "|SamplerState"
                                      "|AccelerationStructure)");
        using SVMatchResults = std::match_results<std::string_view::const_iterator>;
        SVMatchResults match;

        static constexpr std::string_view structured_buffer     = "StructuredBuffer";
        static constexpr std::string_view constant_buffer       = "ConstantBuffer";
        static constexpr std::string_view byte_addressed_buffer = "ByteAddressBuffer";

        if (std::regex_search(_binding_type_str.begin(), _binding_type_str.end(), match, binding_type_regex)) {

            if (match[0].str().find("RWStructuredBuffer") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_STRUCTURED_BUFFER;
            }
            if (match[0].str().find("RWByteAddressBuffer") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_BYTE_ADDRESSED_BUFFER;
            }

            if (match[0].str().find("RWTexture2D") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_TEXTURE_2D;
            }
            if (match[0].str().find("RWTexture2DArray") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_TEXTURE_2D_ARRAY;
            }
            if (match[0].str().find("RWTexture3D") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_TEXTURE_3D;
            }
            if (match[0].str().find("RWTextureCube") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::RW_TEXTURE_CUBE;
            }
            if (match[0].str().find(constant_buffer) != std::string_view::npos) {
                return EShaderCodeResourceBindingType::CONSTANT_BUFFER;
            }
            if (match[0].str().find(structured_buffer) != std::string_view::npos) {
                return EShaderCodeResourceBindingType::STRUCTURED_BUFFER;
            }

            if (match[0] == byte_addressed_buffer.data()) {
                return EShaderCodeResourceBindingType::BYTE_ADDRESS_BUFFER;
            }

            if (match[0].str().find("Texture2D") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::TEXTURE_2D;
            }
            if (match[0].str().find("Texture2DArray") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::TEXTURE_2D_ARRAY;
            }
            if (match[0].str().find("Texture3D") != std::string_view::npos) {
                return EShaderCodeResourceBindingType::TEXTURE_3D;
            }
            if (match[0] == "TextureCube") {
                return EShaderCodeResourceBindingType::TEXTURE_CUBE;
            }
            if (match[0] == "TextureCubeArray") {
                return EShaderCodeResourceBindingType::TEXTURE_CUBE_ARRAY;
            }
            if (match[0] == "Sampler") {
                return EShaderCodeResourceBindingType::SAMPLER;
            }
            if (match[0] == "AccelerationStructure") {
                return EShaderCodeResourceBindingType::RAYTRACING_ACCELERATION_STRUCTURE;
            }
        }
        return EShaderCodeResourceBindingType::INVALID;
    }

}// namespace Utils
//construct root parameter layout info by reflection and meta_data
void Shader::ConstructRootParameterLayoutInfo(const ShaderParametersInfoMap& _param_map) {

    // const auto& parameter_meta_data = type->GetParameterMetaData();

    // Moer::Array<ShaderParameterLayoutInfo> binding_infos;

    // const auto& reflect_map = _param_map.GetShaderParameterInfoMap();
    // for (const auto& member : parameter_meta_data->GetMembers()) {
    //     int16_t                        slot = -1, space = -1, num = 0;
    //     EShaderParameterType           param_type = EShaderParameterType::UNKNOWN;
    //     EShaderCodeResourceBindingType resource_type{EShaderCodeResourceBindingType::INVALID};

    //     bool b_valid = reflect_map.count(member.GetName().data()) > 0;

    //     auto binding_type_str = member.GetShaderBindingTypeStr();
    //     resource_type         = Utils::BindingTypeStrToEnum(binding_type_str);

    //     if (b_valid) {
    //         const auto& iter = reflect_map.find(member.GetName().data());
    //         slot             = iter->second.slot;
    //         space            = iter->second.space;
    //         num              = iter->second.num;
    //         param_type       = iter->second.type;
    //     }
    //     uint32_t step = 0;

    //     step = (num > 0 ? (member.GetStride() / num) : member.GetStride());
    //     //for root constants
    //     if (param_type == EShaderParameterType::CONSTANT_STRUCT) {
    //         param_layout_info.constant_infos.emplace_back(member.GetOffset(), member.GetStride(), slot, space, num, param_type);
    //         continue;
    //     }
    //     //for resources
    //     for (uint32_t i = 0; i < num; i++) {
    //         binding_infos.emplace_back(ShaderParameterLayoutInfo(member.GetOffset() + step * i,
    //                                                              step,
    //                                                              slot++,
    //                                                              space,
    //                                                              1,
    //                                                              param_type,
    //                                                              resource_type));
    //     }
    //     // binding_infos.emplace_back(ShaderParameterLayoutInfo(member.GetOffset(),
    //     //                                                      member.GetStride(),
    //     //                                                      slot,
    //     //                                                      space,
    //     //                                                      num,
    //     //                                                      param_type,
    //     //                                                      resource_type));
    // }
    // param_layout_info.binding_infos.swap(binding_infos);
}

// class TestReflectionShad : public Shader {
// public:
//     static ShaderTypeRegistration s_registration;
//     static ShaderMetaType&        GetMetaType();
//     static Shader*                ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) { return new TestReflectionShad(_initializer); }
//     TestReflectionShad(const ShaderCompiledInitializer& _initializer) : Shader(_initializer) {}

// public:
//     class alignas(16) Ubo {
//     public:
//         Ubo() { memset(this, 0, sizeof(Ubo)); }
//         struct TypeInfo {
//             static constexpr int32_t s_num_rows                     = 1;
//             static constexpr int32_t s_num_columns                  = 1;
//             static constexpr int32_t s_num_elements                 = 0;
//             static constexpr int32_t alignment                      = 16;
//             static constexpr bool    b_is_stored_in_constant_buffer = true;
//             using TParamPtr                                         = Ubo;
//             static const ShaderParametersMetadata* GetStructMetadata() {
//                 static ShaderParametersMetadata s_struct_metadata(EShaderParameterUseCase ::SHADER_CONSTANT_STRUCT, "Ubo", sizeof(Ubo), Ubo ::GetMembers());
//                 return &s_struct_metadata;
//             }
//         };

//     private:
//         using TThisStruct = Ubo;
//         struct _firstMemberId {
//             enum { HasDeclaredResource = 0 };
//         };
//         typedef void* (*MemberFunction)(_firstMemberId, std ::vector<ShaderParametersMetadata ::Member>*);
//         static void*           AppendMemberGetPrev(_firstMemberId, std ::vector<ShaderParametersMetadata ::Member>*) { return nullptr; }
//         typedef _firstMemberId MemberIdprojectionMatrix;

//     public:
//         TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr projectionMatrix;

//     private:
//         struct _nextMemberIdprojectionMatrix {
//             enum { HasDeclaredResource = MemberIdprojectionMatrix ::HasDeclaredResource };
//         };
//         static void* AppendMemberGetPrev(_nextMemberIdprojectionMatrix, std ::vector<ShaderParametersMetadata ::Member>* _members) {
//             _members->push_back(ShaderParametersMetadata ::Member("projectionMatrix", "", __builtin_offsetof(TThisStruct, projectionMatrix), sizeof(TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr), TShaderParameterTypeInfo<Moer ::Matrix4x4f>::BaseType, EShaderPrecisionModifier ::FLOAT, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::s_num_elements, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::GetStructMetadata()));
//             void* (*PrevFunc)(MemberIdprojectionMatrix, std ::vector<ShaderParametersMetadata ::Member>*);
//             PrevFunc = AppendMemberGetPrev;
//             return (void*)PrevFunc;
//         }
//         typedef _nextMemberIdprojectionMatrix
//             MemberIdmodelMatrix;

//     public:
//         TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr modelMatrix;

//     private:
//         struct _nextMemberIdmodelMatrix {
//             enum { HasDeclaredResource = MemberIdmodelMatrix ::HasDeclaredResource };
//         };
//         static void* AppendMemberGetPrev(_nextMemberIdmodelMatrix, std ::vector<ShaderParametersMetadata ::Member>* _members) {
//             _members->push_back(ShaderParametersMetadata ::Member("modelMatrix", "", __builtin_offsetof(TThisStruct, modelMatrix), sizeof(TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr), TShaderParameterTypeInfo<Moer ::Matrix4x4f>::BaseType, EShaderPrecisionModifier ::FLOAT, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::s_num_elements, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::GetStructMetadata()));
//             void* (*PrevFunc)(MemberIdmodelMatrix, std ::vector<ShaderParametersMetadata ::Member>*);
//             PrevFunc = AppendMemberGetPrev;
//             return (void*)PrevFunc;
//         }
//         typedef _nextMemberIdmodelMatrix MemberIdviewMatrix;

//     public:
//         TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr viewMatrix;

//     private:
//         struct _nextMemberIdviewMatrix {
//             enum { HasDeclaredResource = MemberIdviewMatrix ::HasDeclaredResource };
//         };
//         static void* AppendMemberGetPrev(_nextMemberIdviewMatrix, std ::vector<ShaderParametersMetadata ::Member>* _members) {
//             _members->push_back(ShaderParametersMetadata ::Member("viewMatrix", "", __builtin_offsetof(TThisStruct, viewMatrix), sizeof(TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr), TShaderParameterTypeInfo<Moer ::Matrix4x4f>::BaseType, EShaderPrecisionModifier ::FLOAT, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::s_num_elements, TShaderParameterTypeInfo<Moer ::Matrix4x4f>::GetStructMetadata()));
//             void* (*PrevFunc)(MemberIdviewMatrix, std ::vector<ShaderParametersMetadata ::Member>*);
//             PrevFunc = AppendMemberGetPrev;
//             return (void*)PrevFunc;
//         }
//         typedef _nextMemberIdviewMatrix lastMemberId;

//     public:
//         static std ::vector<ShaderParametersMetadata ::Member> GetMembers() {
//             std ::vector<ShaderParametersMetadata ::Member> _members;
//             void* (*_lastFunc)(lastMemberId, std ::vector<ShaderParametersMetadata ::Member>*);
//             _lastFunc = AppendMemberGetPrev;
//             void* Ptr = (void*)_lastFunc;
//             do { Ptr = reinterpret_cast<MemberFunction>(Ptr)(_firstMemberId(), &_members); } while (Ptr);
//             std ::reverse(_members.begin(), _members.end());
//             return _members;
//         }
//     };

// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(Ubo, ubo)
//     DEFINE_SHADER_PARAM_SRV(Buffer, bar)
//     //Ubo set
//     DEFINE_SHADER_PARAM_UAV(RWBuffer, dataLog)

//     DEFINE_SHADER_PARAM_SAMPLER_ARRAY(Sampler[2], samp, 2)
//     DEFINE_SHADER_PARAM_SAMPLER(Sampler, aniso)
//     //srv set
//     DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D[5], foo, 5)
//     //uav se

//     END_ROOT_PARAMETER_DEFINITION(Parameters)

//     // static_assert(sizeof(TShaderParameterTypeInfo<Moer ::Matrix4x4f>::TParamPtr) == 16);
// };