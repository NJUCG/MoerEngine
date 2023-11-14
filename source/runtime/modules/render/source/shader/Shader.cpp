#include "shader/Shader.h"
#include "misc/Hash.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"
#include <vcruntime_typeinfo.h>

class TestShaderClass : Shader {
    DEFINE_SHADER_TYPE(TestShaderClass, Global, RENDER_CORE_API)
};

IMPLEMENT_SHADER_TYPE(TestReflectionShader, "TestVert.vert", "main", EShaderType::ST_VERTEX);

Shader::Shader(){

};

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

const ShaderCodeEntry* Shader::GetCodeEntry() const {
    if (type != nullptr) {
        return ShaderResourceManager::GetInstance().GetShaderCodeMap().GetCodeEntry(type->GetName());
    }
    return nullptr;
}

const Hash64City& Shader::GetCompiledHash() const {
    return compiled_hash;
};

void Shader::ConstructRootParameterLayoutInfo(const ShaderParametersInfoMap& _param_map) {
    const auto&                            parameter_meta_data = type->GetParameterMetaData();
    std::vector<ShaderParameterLayoutInfo> layout_infos;
    const auto&                            reflect_map = _param_map.GetShaderParameterInfoMap();
    for (const auto& member : parameter_meta_data->GetMembers()) {
        int16_t              slot = -1, space = -1, num = 0;
        EShaderParameterType param_type = EShaderParameterType::UNKNOWN;
        bool                 b_valid    = reflect_map.count(member.GetName()) > 0;
        if (b_valid) {
            const auto& iter = reflect_map.find(member.GetName());
            slot             = iter->second.slot;
            space            = iter->second.space;
            num              = iter->second.num;
            param_type       = iter->second.type;
        }
        uint32_t step = 0;

        step = (num > 0 ? (member.GetStride() / num) : member.GetStride());
        //for root constants
        if (param_type == EShaderParameterType::CONSTANT_STRUCT) {
            param_layout_info.constant_infos.emplace_back(member.GetOffset(), member.GetStride(), slot, space, param_type);
            continue;
        }
        //for resources
        for (size_t i = 0; i < ((num == 0) ? 1 : num); ++i) {
            layout_infos.emplace_back(ShaderParameterLayoutInfo(member.GetOffset() + step * i,
                                                                step,
                                                                slot++,
                                                                space,
                                                                param_type));
        }
    }
    param_layout_info.layout_infos.swap(layout_infos);
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