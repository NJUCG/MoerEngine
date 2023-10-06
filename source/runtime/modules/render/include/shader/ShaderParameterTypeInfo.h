#ifndef MOREENGINE_SHADER_PARAM_TYPE_INFO
#define MOREENGINE_SHADER_PARAM_TYPE_INFO

#include <array>
#include <cstdint>
#include "math/Math.h"
#include "shader/ShaderCommon.h"
#define SHADER_PARAMETER_STRUCTURE_ALIGNMENT 16
#define SHADER_PARAMETER_ARRAY_ALIGNMENT     16
template<uint32_t Alignment>
concept concept_valid_alignment =
    (Alignment == 2 || Alignment == 4 || Alignment == 8 || Alignment == 16) == true;

template<typename TargetType, uint32_t Alignment>
    requires concept_valid_alignment<Alignment>
struct AlignType {
    ALIGNED_TYPE_DEF(TargetType, Type, Alignment);
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

    using TParamPtr = TShaderParameterPtr<float>;

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
    using InstanceType = float4;
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
    using InstanceType = int4;
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
    using InstanceType = uint4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<float2> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<float2, alignment>;
    using InstanceType = float4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

//float3 packed to float4 in gpu
template<>
struct TShaderParameterTypeInfo<float3> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<float3, alignment>;
    using InstanceType = float4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<float4> {
    static constexpr EShaderBindingBaseType BaseType = SBT_FLOAT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<float4, alignment>;
    using InstanceType = float4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<int2> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<int2, alignment>;
    using InstanceType = int4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<uint2> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 2;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 8;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<uint2, alignment>;
    using InstanceType = uint4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<int3> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<int3, alignment>;
    using InstanceType = int4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<uint3> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 3;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<uint3, alignment>;
    using InstanceType = uint4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<int4> {
    static constexpr EShaderBindingBaseType BaseType = SBT_INT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<int4, alignment>;
    using InstanceType = int4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

template<>
struct TShaderParameterTypeInfo<uint4> {
    static constexpr EShaderBindingBaseType BaseType = SBT_UINT32;

    static constexpr int32_t s_num_rows                     = 1;
    static constexpr int32_t s_num_columns                  = 4;
    static constexpr int32_t s_num_elements                 = 0;
    static constexpr int32_t alignment                      = 16;
    static constexpr bool    b_is_stored_in_constant_buffer = true;

    using TParamPtr    = AlignType<uint4, alignment>;
    using InstanceType = uint4;
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
    using InstanceType = uint4;
    static const ShaderParametersMetadata* GetStructMetadata() { return nullptr; }
};

//todo: quaternion
//todo: matrix

#endif