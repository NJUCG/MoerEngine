#ifndef MOER_HIZ_BUILDER_H
#define MOER_HIZ_BUILDER_H
#include "RenderAPI.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
namespace Moer {
    struct RENDER_API HiZBuffer {
        RHITextureRef          texture = nullptr;
        RHISRVRef              srv     = nullptr;
        Moer::Array<RHIUAVRef> uavs{};

        void InitFromDepthExtent(Vector2i extent);
    };

    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(HiZConfig)
    DEFINE_SHADER_PARAM(bool, b_mip0)
    DEFINE_SHADER_PARAM(uint32_t, target_level)
    DEFINE_SHADER_PARAM(Vector2i, size)
    END_SHADER_CONSTANT_STRUCT_DEFINITION(HiZConfig)

    class BuildHiZShader : public Shader {
        DEFINE_SHADER_TYPE(BuildHiZShader, Global, RENDER_API);

    public:
        BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
        DEFINE_SHADER_PARAM_STRUCT(HiZConfig, config)

        DEFINE_SHADER_PARAM_UAV(RWTexture2D<float>, target)
        DEFINE_SHADER_PARAM_SAMPLER(SamplerState, depth_sampler)
        DEFINE_SHADER_PARAM_SRV(Texture2D<float>, depth_buffer)

        END_ROOT_PARAMETER_DEFINITION(Parameters)
    };

    class RENDER_API HiZBuilder {
    public:
        struct Impl;
        HiZBuilder();
        ~HiZBuilder();
        static HiZBuilder& GetInstance();
        void               DispatchBuildHiZ(RHIGraphicsCommandList* cmd_list, RHISRVRef depth_buffer, HiZBuffer& hiz_buffer);

    private:
        UniquePtr<Impl> impl;
    };
};// namespace Moer
#endif