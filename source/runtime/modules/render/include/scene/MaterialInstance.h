#pragma once

#include "TextureInterfaceBlock.h"
#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

namespace Moer {

    using SamplerParams = RHISamplerInitializer;
    class Material;
    using MaterialRef = CountableRef<Material>;

    class RENDER_API MaterialInstance : public CountableResource {
    public:
        MaterialInstance(MaterialRef material);
        void SetParameter(const std::string& name, RHITextureRef texture);
        void SetParameter(const std::string& name, SamplerParams params);
        void Use(RHIBatchedShaderParameters& parameters);

    protected:
        class Impl;
        Impl* m_impl;
    };

    class SamplerGroup {
    public:
        SamplerGroup(uint32_t size);
        void SetSampler(uint32_t index, RHITextureRef texture, SamplerParams sampler);
        void SetSampler(uint32_t index, RHITextureRef texture);
        void SetSampler(uint32_t index, SamplerParams sampler);
        void Bind(RHIBatchedShaderParameters& parameters);

    protected:
        struct SamplerDescriptor {
            RHITextureRef      texture;
            SamplerParams      sampler;
            ESamplerBindingType type;
        };
        Array<SamplerDescriptor> m_samplers;
    };

    using MaterialInstanceRef = CountableRef<MaterialInstance>;

}