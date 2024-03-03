#include "scene/MaterialInstance.h"

#include "resources/GlobalRenderResources.h"
#include "scene/Material.h"

namespace Moer {
    class MaterialInstance::Impl {
    public:
        Impl(MaterialRef material) : m_material(material), m_sampler_group(material->GetSamplerInterfaceBlock().GetSize()) {
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
        void Use(RHIBatchedShaderParameters& parameters) {
            m_sampler_group.Bind(parameters);
        }

    protected:
        MaterialRef  m_material;
        SamplerGroup m_sampler_group;
    };

    void SamplerGroup::Bind(RHIBatchedShaderParameters& parameters) {
        uint16_t idx = 0;
        for (auto& sampler : m_samplers) {
            if (sampler.type == ESamplerBindingType::TEXTURE) {
                auto texture_view = SamplerCache::Get().GetTextureView(sampler.texture);
                parameters.SetParameters(texture_view, idx++, 1);
            } else if (sampler.type == ESamplerBindingType::SAMPLER) {
                auto rhiSampler = SamplerCache::Get().GetSampler(sampler.sampler);
                parameters.SetParameters(rhiSampler, idx++, 1);
            }
        }
    }
    SamplerGroup::SamplerGroup(uint32_t size) {
        this->m_samplers = Array<SamplerDescriptor>(size);
    }

    MaterialInstance::MaterialInstance(MaterialRef material) {
        m_impl = new Impl(material);
    }
    void MaterialInstance::SetParameter(const std::string& name, RHITextureRef texture) {
        m_impl->SetParameter(name, texture);
    }
    void MaterialInstance::SetParameter(const std::string& name, SamplerParams params) {
        m_impl->SetParameter(name, params);
    }
    void MaterialInstance::Use(RHIBatchedShaderParameters& parameters) {
        m_impl->Use(parameters);
    }

    void SamplerGroup::SetSampler(uint32_t index, RHITextureRef texture, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(texture, sampler, ESamplerBindingType::COMBINED);
    }
    void SamplerGroup::SetSampler(uint32_t index, RHITextureRef texture) {
        m_samplers[index] = SamplerDescriptor(texture, SamplerParams(), ESamplerBindingType::TEXTURE);
    }
    void SamplerGroup::SetSampler(uint32_t index, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(nullptr, sampler, ESamplerBindingType::SAMPLER);
    }

}