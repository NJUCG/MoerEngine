#ifndef MOREENGINE_SHADER_PARAM_TYPE_INFO
#define MOREENGINE_SHADER_PARAM_TYPE_INFO

#include "math/Math.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "RenderCommon.h"
#include "misc/Ptr.h"

#include <array>
#include <cstdint>

template<uint32_t Alignment>
concept concept_valid_alignment =
    (Alignment == 2 || Alignment == 4 || Alignment == 8 || Alignment == 16) == true;

template<typename TargetType, uint32_t Alignment>
    requires concept_valid_alignment<Alignment>
struct AlignType {
    ALIGNED_TYPE_DEF(TargetType, Type, Alignment);
};

template<typename ShaderResourceType>
struct TShaderResourceParameterTypeInfo {
    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool    b_is_stored_in_constant_buffer = false;

    using TParamPtr = ShaderParameterPtr<ShaderResourceType>;

    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }

    static_assert(sizeof(TParamPtr) == SHADER_PARAMETER_PTR_ALIGNMENT, "Uniform buffer layout must not be platform dependent.");
};

template<typename ShaderResourceType, uint32_t NumElements>
struct TShaderResourceParameterTypeInfo<ShaderResourceType[NumElements]> {
    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = NumElements;
    static constexpr int32_t alignment                      = SHADER_PARAMETER_ARRAY_ALIGNMENT;
    static constexpr bool    b_is_stored_in_constant_buffer = false;

    using TParamPtr = std::array<ShaderResourceType, NumElements>;

    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<class StructType>
struct TShaderParameterStructureTypeInfo {
    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = sizeof(StructType);
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr = StructType;

    static const ShaderParametersMetadata* GetStructMetadata() { return StructType::FTypeInfo::GetStructMetadata(); }
};

template<class StructType, uint32_t NumElements>
struct TShaderParameterStructureTypeInfo<StructType[NumElements]> {
    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = sizeof(StructType);
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr = std::array<StructType, NumElements>;

    static const ShaderParametersMetadata* GetStructMetadata() { return StructType::GetStructMetadata(); }
};

template<typename Type>
struct TShaderParameterTypeInfo {
    static constexpr EShaderBindingBaseType BaseType                       = SBT_SRV;
    static constexpr int32_t                s_num_rows                     = 1;
    static constexpr int32_t                s_num_columns                  = 1;
    static constexpr int32_t                s_num_elements                 = 0;
    static constexpr int32_t                alignment                      = sizeof(Type);
    static constexpr bool                   b_is_stored_in_constant_buffer = true;

    using TParamPtr = ShaderParameterPtr<float>;

    static const ShaderParametersMetadata* GetStructMetadata() { return Type::TypeInfo::GetStructMetadata(); }
};

template<typename Type, uint32_t NumElements>
struct TShaderParameterTypeInfo<Type[NumElements]> {
    static constexpr EShaderBindingBaseType BaseType                       = TShaderParameterTypeInfo<Type>::BaseType;
    static constexpr int32_t                s_num_rows                     = TShaderParameterTypeInfo<Type>::s_num_rows;
    static constexpr int32_t                s_num_columns                  = TShaderParameterTypeInfo<Type>::s_num_columns;
    static constexpr int32_t                s_num_elements                 = NumElements;
    static constexpr int32_t                alignment                      = SHADER_PARAMETER_ARRAY_ALIGNMENT;
    static constexpr bool                   b_is_stored_in_constant_buffer = TShaderParameterTypeInfo<Type>::b_is_stored_in_constant_buffer;

    using TParamPtr    = std::array<AlignType<Type, alignment>, NumElements>;
    using InstanceType = Type;
    static const ShaderParametersMetadata* GetStructMetadata() { return Type::TypeInfo::GetStructMetadata(); }
};

template<>
struct TShaderParameterTypeInfo<float> {
    static constexpr EShaderBindingBaseType BaseType                       = SBT_FLOAT32;
    static constexpr int32_t                s_num_rows                     = 1;
    static constexpr int32_t                s_num_columns                  = 1;
    static constexpr int32_t                s_num_elements                 = 0;
    static constexpr int32_t                alignment                      = sizeof(float);
    static constexpr bool                   b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<float, alignment>;
    using InstanceType = Moer::Vector4f;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<int32_t> {
    static constexpr EShaderBindingBaseType BaseType                       = SBT_INT32;
    static constexpr int32_t                s_num_rows                     = 1;
    static constexpr int32_t                s_num_columns                  = 1;
    static constexpr int32_t                s_num_elements                 = 0;
    static constexpr int32_t                alignment                      = 4;
    static constexpr bool                   b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<int32_t, alignment>;
    using InstanceType = Moer::Vector4i;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<uint32_t> {
    static constexpr EShaderBindingBaseType BaseType                       = SBT_UINT32;
    static constexpr int32_t                s_num_rows                     = 1;
    static constexpr int32_t                s_num_columns                  = 1;
    static constexpr int32_t                s_num_elements                 = 0;
    static constexpr int32_t                alignment                      = 4;
    static constexpr bool                   b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<int32_t, alignment>;
    using InstanceType = Moer::Vector4ui;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector2f> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector2f, alignment>;
    using InstanceType = Moer::Vector4f;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

//Moer::Vector3f packed to Moer::Vector4f in gpu
template<>
struct TShaderParameterTypeInfo<Moer::Vector3f> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector3f, alignment>;
    using InstanceType = Moer::Vector4f;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector4f> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector4f, alignment>;
    using InstanceType = Moer::Vector4f;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector2i> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector2i, alignment>;
    using InstanceType = Moer::Vector4i;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector2ui> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector2ui, alignment>;
    using InstanceType = Moer::Vector4ui;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector3i> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector3i, alignment>;
    using InstanceType = Moer::Vector4i;
    static const ShaderParametersMetadata* GetStructMetadata() {
        return nullptr;
    }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector3ui> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector3ui, alignment>;
    using InstanceType = Moer::Vector4ui;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector4i> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector4i, alignment>;
    using InstanceType = Moer::Vector4i;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<Moer::Vector4ui> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<Moer::Vector4ui, alignment>;
    using InstanceType = Moer::Vector4ui;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<bool> {
    static constexpr EShaderBindingBaseType BaseType = SBT_BOOL;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 4;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<bool, alignment>;
    using InstanceType = Moer::Vector4ui;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<AttachmentBindingSlots> {

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 1;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = SHADER_PARAMETER_STRUCTURE_ALIGNMENT;
    static constexpr bool    b_is_stored_in_constant_buffer = false;

    using TParamPtr = AttachmentBindingSlots;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

//todo: quaternion
//todo: matrix

#endif