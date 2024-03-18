#pragma once

#include "TextureInterfaceBlock.h"
#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

namespace Moer {
    class UniformBuffer;

    using SamplerParams = RHISamplerCreateInfo;
    class Material;
    using MaterialRef = CountableRef<Material>;

    class RENDER_API MaterialInstance : public CountableResource {
    public:
        MaterialInstance(const MaterialInstance& rhs) = delete;
        MaterialInstance& operator=(const MaterialInstance& rhs) = delete;
        MaterialInstance(MaterialRef material);
        template<typename T>
        using is_supported_parameter_t = typename std::enable_if<
            std::is_same<float, T>::value ||
            std::is_same<int32_t, T>::value ||
            std::is_same<uint32_t, T>::value ||
            std::is_same<Vector2i, T>::value ||
            std::is_same<Vector3i, T>::value ||
            std::is_same<Vector4i, T>::value ||
            std::is_same<Vector2ui, T>::value ||
            std::is_same<Vector3ui, T>::value ||
            std::is_same<Vector4ui, T>::value ||
            std::is_same<Vector2f, T>::value ||
            std::is_same<Vector3f, T>::value ||
            std::is_same<Vector4f, T>::value ||
            std::is_same<Matrix4x4f, T>::value ||
            // these types are slower as they need a layout conversion
            std::is_same<bool, T>::value ||
            std::is_same<Matrix3x3f, T>::value>::type;

        template<typename T, typename = is_supported_parameter_t<T>>
        void        SetParameter(const std::string& name, T const& value);
        void        SetParameter(const std::string& name, const void* value, size_t size);
        void        SetParameter(const std::string& name, const RHITextureRef& texture);
        void        SetParameter(const std::string& name, const SamplerParams& params);
        RHITexture* GetTexture(const std::string& name) const;

        void                 Use(RHIBatchedShaderParameters& parameters);
        MaterialRef          GetMaterial() const;
        const UniformBuffer& GetUniformBuffer() const;

    protected:
        class Impl;
        Impl* m_impl;
    };

    template<typename T, typename>
    void MaterialInstance::SetParameter(const std::string& name, T const& value) {
        SetParameter(name, &value, sizeof(T));
    }

    class SamplerGroup {
    public:
        SamplerGroup(uint32_t size);
        void          SetSampler(uint32_t index, RHITextureRef texture, SamplerParams sampler);
        void          SetSampler(uint32_t index, RHITextureRef texture);
        void          SetSampler(uint32_t index, SamplerParams sampler);
        RHITextureRef GetTexture(uint32_t index) const;
        // void Bind(RHIBatchedShaderParameters& parameters);

    protected:
        struct SamplerDescriptor {
            RHITextureRef  texture;
            SamplerParams  sampler;
            EParamaterType type;
        };
        Array<SamplerDescriptor> m_samplers;
    };

    using MaterialInstanceRef = CountableRef<MaterialInstance>;

}