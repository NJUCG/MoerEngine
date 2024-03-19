#include "scene/MaterialInstance.h"

#include "resources/GlobalRenderResources.h"
#include "scene/BufferInterfaceBlock.h"
#include "scene/Material.h"

namespace Moer {
    class MaterialInstance::Impl {
    public:
        Impl(MaterialRef material) : m_material(material), m_sampler_group(material->GetSamplerInterfaceBlock().GetSize()), m_uniform(material->GetBufferInterfaceBlock().getSize()) {
        }
        void SetParameter(const std::string& name, RHITextureRef texture, SamplerParams sampler) {
            uint32_t sampler_index = m_material->GetSamplerInterfaceBlock().GetSamplerInfo(name)->offset;
            m_sampler_group.SetSampler(sampler_index, texture, sampler);
        }
        void SetParameter(const std::string& name, RHITextureRef texture) {
            uint32_t sampler_index = m_material->GetSamplerInterfaceBlock().GetSamplerInfo(name)->offset;
            m_sampler_group.SetSampler(sampler_index, texture);
        }
        void SetParameter(const std::string& name, SamplerParams sampler) {
            uint32_t sampler_index = m_material->GetSamplerInterfaceBlock().GetSamplerInfo(name)->offset;
            m_sampler_group.SetSampler(sampler_index, sampler);
        }
        void SetParameter(const std::string& name, const void* value, size_t size) {
            auto offset = m_material->GetBufferInterfaceBlock().getFieldInfo(name)->offset;
            m_uniform.SetData(value, size, offset);
        }

        RHITexture* GetTexture(const std::string& name) {
            uint32_t sampler_index = m_material->GetSamplerInterfaceBlock().GetSamplerInfo(name)->offset;
            return m_sampler_group.GetTexture(sampler_index);
        }

        const UniformBuffer& GetUniformBuffer() const {
            return m_uniform;
        }

        void Use(RHIBatchedShaderParameters& parameters) {
            //  m_sampler_group.Bind(parameters);
        }
        MaterialRef GetMaterial() const {
            return m_material;
        }

    protected:
        MaterialRef   m_material;
        SamplerGroup  m_sampler_group;
        UniformBuffer m_uniform;
    };

    // void SamplerGroup::Bind(RHIBatchedShaderParameters& parameters) {
    //     uint16_t idx = 0;
    //     for (auto& sampler : m_samplers) {
    //         if (sampler.type == EParamaterType::TEXTURE) {
    //             auto texture_view = SamplerCache::Get().GetTextureView(sampler.texture);
    //             parameters.SetParameters(texture_view, idx++, 1);
    //         } else if (sampler.type == EParamaterType::SAMPLER) {
    //             auto rhiSampler = SamplerCache::Get().GetSampler(sampler.sampler);
    //             parameters.SetParameters(rhiSampler, idx++, 1);
    //         }
    //     }
    // }
    SamplerGroup::SamplerGroup(uint32_t size) {
        this->m_samplers = Array<SamplerDescriptor>(size);
    }

    MaterialInstance::MaterialInstance(MaterialRef material) {
        m_impl = new Impl(material);
    }
    void MaterialInstance::SetParameter(const std::string& name, const void* value, size_t size) {
        m_impl->SetParameter(name, value, size);
    }
    void MaterialInstance::SetParameter(const std::string& name, const RHITextureRef& texture) {
        m_impl->SetParameter(name, texture);
    }
    void MaterialInstance::SetParameter(const std::string& name, const SamplerParams& params) {
        m_impl->SetParameter(name, params);
    }
    RHITexture* MaterialInstance::GetTexture(const std::string& name) const {
        return m_impl->GetTexture(name);
    }
    void MaterialInstance::Use(RHIBatchedShaderParameters& parameters) {
    }
    const UniformBuffer& MaterialInstance::GetUniformBuffer() const {
        return m_impl->GetUniformBuffer();
    }
    MaterialRef MaterialInstance::GetMaterial() const {
        return m_impl->GetMaterial();
    }
    void SamplerGroup::SetSampler(uint32_t index, RHITextureRef texture, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(texture, sampler, EParamaterType::COMBINED);
    }
    void SamplerGroup::SetSampler(uint32_t index, RHITextureRef texture) {
        m_samplers[index] = SamplerDescriptor(texture, SamplerParams(), EParamaterType::TEXTURE);
    }
    void SamplerGroup::SetSampler(uint32_t index, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(nullptr, sampler, EParamaterType::SAMPLER);
    }
    RHITextureRef SamplerGroup::GetTexture(uint32_t index) const {
        return m_samplers[index].texture;
    }

}