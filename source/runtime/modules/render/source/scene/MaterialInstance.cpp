#include "scene/MaterialInstance.h"

#include "resources/GlobalRenderResources.h"
#include "scene/BufferInterfaceBlock.h"
#include "scene/Material.h"

namespace Moer {
    class MaterialInstance::Impl {
    public:
        Impl(MaterialRef material) : m_material(material), m_sampler_group(material->GetSamplerInterfaceBlock().GetSize()), m_uniform(material->GetBufferInterfaceBlock().GetSize()) {
        }
        void SetParameter(const std::string& name, Render::TextureRef texture, SamplerParams sampler) {
            uint32_t sampler_index = m_material->GetSamplerInterfaceBlock().GetSamplerInfo(name)->offset;
            m_sampler_group.SetSampler(sampler_index, texture, sampler);
        }
        void SetParameter(const std::string& name, uint32 texture) {
            SetParameter(name,&texture,sizeof(uint32));
        }
        void SetParameter(const std::string& name, const void* value, size_t size) {
            auto offset = m_material->GetBufferInterfaceBlock().GetFieldInfo(name)->offset;
            m_uniform.SetData(value, size, offset);
        }
        void SetUnifomBuffer(const void* data, size_t size, size_t offset) {
            m_uniform.SetData(data, size, offset);
        }

        Render::Texture* GetTexture(const std::string& name) {
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

        const std::string& GetName() const {
            return m_name;
        }

        void SetName(const std::string& name) {
            m_name = name;
        }

    protected:
        MaterialRef   m_material;
        SamplerGroup  m_sampler_group;
        UniformBuffer m_uniform;
        std::string   m_name;
    };
    
    SamplerGroup::SamplerGroup(uint32_t size) {
        this->m_samplers = Array<SamplerDescriptor>(size);
    }

    MaterialInstance::MaterialInstance(MaterialRef material) {
        m_impl = new Impl(material);
    }
    void MaterialInstance::SetParameter(const std::string& name, const void* value, size_t size) {
        m_impl->SetParameter(name, value, size);
    }
    // void MaterialInstance::SetTexture(const std::string& name, const uint texture_handle) {
    //     m_impl->SetParameter(name,texture_handle);
    // }
    // void MaterialInstance::SetParameter(const std::string& name, const Render::TextureRef& texture) {
    //     m_impl->SetParameter(name, texture);
    // }
    // void MaterialInstance::SetParameter(const std::string& name, const SamplerParams& params) {
    //     m_impl->SetParameter(name, params);
    // }
    void MaterialInstance::SetUnifomBuffer(const void* data, size_t size) {
        m_impl->SetUnifomBuffer(data, size, 0);
    }
    Render::Texture* MaterialInstance::GetTexture(const std::string& name) const {
        return m_impl->GetTexture(name);
    }
    void MaterialInstance::Use(RHIBatchedShaderParameters& parameters) {
    }
    const UniformBuffer& MaterialInstance::GetUniformBuffer() const {
        return m_impl->GetUniformBuffer();
    }
    void MaterialInstance::SetName(const std::string& name) {
        m_impl->SetName(name);
    }

    const std::string& MaterialInstance::GetName() const {
        return m_impl->GetName();
    }
    MaterialRef MaterialInstance::GetMaterial() const {
        return m_impl->GetMaterial();
    }
    void SamplerGroup::SetSampler(uint32_t index, Render::TextureRef texture, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(texture, sampler, EParamaterType::COMBINED);
    }
    void SamplerGroup::SetSampler(uint32_t index, Render::TextureRef texture) {
        m_samplers[index] = SamplerDescriptor(texture, SamplerParams(), EParamaterType::TEXTURE);
    }
    void SamplerGroup::SetSampler(uint32_t index, SamplerParams sampler) {
        m_samplers[index] = SamplerDescriptor(nullptr, sampler, EParamaterType::SAMPLER);
    }
    Render::TextureRef SamplerGroup::GetTexture(uint32_t index) const {
        return m_samplers[index].texture;
    }

}