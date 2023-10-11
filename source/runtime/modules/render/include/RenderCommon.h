#ifndef MOREENGINE_RENDER_COMMON_H
#define MOREENGINE_RENDER_COMMON_H
#define SHADER_PARAMETER_STRUCTURE_ALIGNMENT 16
#define SHADER_PARAMETER_ARRAY_ALIGNMENT     16
#define SHADER_PARAMETER_PTR_ALIGNMENT       sizeof(size_t)

#include "misc/Ptr.h"

template<typename TParamType>
class ShaderParameterPtr : public AlignedPtr<TParamType, SHADER_PARAMETER_PTR_ALIGNMENT> {
public:
    ShaderParameterPtr() {}

    ShaderParameterPtr(const TParamType& Other)
        : AlignedPtr<TParamType, SHADER_PARAMETER_PTR_ALIGNMENT>(Other) {}

    ShaderParameterPtr(const ShaderParameterPtr<TParamType>& Other)
        : AlignedPtr<TParamType, SHADER_PARAMETER_PTR_ALIGNMENT>(Other) {}

    inline void operator=(const TParamType& Other) {
        AlignedPtr<TParamType, SHADER_PARAMETER_PTR_ALIGNMENT>::ref = Other;
    }
};

#endif