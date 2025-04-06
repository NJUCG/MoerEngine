#include "scene/Material.h"

#include "rhi/RHICommon.h"
#include "rhi/RHIResourceInitilizer.h"
#include "scene/MaterialInstance.h"
#include "misc/MMemory.h"
#include "resources/GlobalRenderResources.h"
#include "resources/GpuScene.h"
#include "rhi/RHIResource.h"
#include "scene/BufferInterfaceBlock.h"
#include "scene/TextureInterfaceBlock.h"
#include "shader/Shader.h"

namespace Moer {

    //TODO: include this form hlsl file
    struct MaterialData {
        Vector4f base_color_factor;
        Vector3f emissive_factor;
        float    metallic_factor;
        float    roughness_factor;
        float    ao;
        int      albedo_map{-1};
        int      normal_map{-1};
        int      metallic_roughness_map{-1};
        int      ao_map{-1};
        int      emissive_map{-1};
        int      padding;
    };

    class Material::Impl {
        friend Material;
        Impl() = default;
        void                         SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept;
        const TextureInterfaceBlock& GetSamplerInterfaceBlock() const noexcept;
        void                         SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept;
        const BufferInterfaceBlock&  GetBufferInterfaceBlock() const noexcept;
        EMaterialType                GetType() const noexcept;
        void                         SetType(EMaterialType type) noexcept;
        void                         SetName(const std::string& name) noexcept;
        const std::string&           GetName() const noexcept;

        std::string           m_name;
        TextureInterfaceBlock m_sampler_interface_block;
        BufferInterfaceBlock  m_buffer_interface_block;
        EMaterialType         m_type;
    };

    Material::Material() {
        m_impl = new Impl();
    }
    void Material::SetName(const std::string& name) noexcept {
        m_impl->SetName(name);
    }
    const std::string& Material::GetName() const noexcept {
        return m_impl->GetName();
    }
    MaterialInstanceRef Material::CreateInstance() {
        MaterialInstanceRef material_instance = MoerNew(MaterialInstance(this));
        return material_instance;
    }
    void Material::SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept {
        m_impl->SetSamplerInterfaceBlock(sampler_interface_block);
    }
    // MaterialInstanceRef Material::createInstance() {
    //     return MoerNew(MaterialInstance);
    // }
    const TextureInterfaceBlock& Material::GetSamplerInterfaceBlock() const noexcept {
        return m_impl->GetSamplerInterfaceBlock();
    }
    void Material::SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept {
        m_impl->SetBufferInterfaceBlock(buffer_interface_block);
    }
    const BufferInterfaceBlock& Material::GetBufferInterfaceBlock() const noexcept {
        return m_impl->GetBufferInterfaceBlock();
    }
    EMaterialType Material::GetType() const noexcept {
        return m_impl->GetType();
    }
    void Material::SetType(EMaterialType type) noexcept {
        return m_impl->SetType(type);
    }
    MaterialRef MaterialBuilder::Build() {
        TextureInterfaceBlock::Builder sampler_interface_block_builder;
        BufferInterfaceBlock::Builder  buffer_interface_block_builder;
        sampler_interface_block_builder.Name("material samplers");
        buffer_interface_block_builder.name("material uniforms");
        for (const auto& param : m_parameters) {
            if (param.IsSampler()) {
                sampler_interface_block_builder.AddSampler(param.name.c_str(), param.samplerType);
            } else if (param.IsTexture()) {
                sampler_interface_block_builder.AddTexture(param.name.c_str(), param.textureType);
            } else if (param.IsUniform()) {
                buffer_interface_block_builder.add({
                    .name   = param.name,
                    .type   = param.uniformType,
                    .stride = param.size,
                });
            }
        }

        TextureInterfaceBlock sampler_interface_block = sampler_interface_block_builder.Build();
        BufferInterfaceBlock  buffer_interface_block  = buffer_interface_block_builder.Build();
        MaterialRef           material                = MoerNew(Material);
        material->SetSamplerInterfaceBlock(sampler_interface_block);
        material->SetBufferInterfaceBlock(buffer_interface_block);
        material->SetName(m_material_name);
        material->SetType(m_material_type);
        return material;
    }
    MaterialBuilder& MaterialBuilder::SetParameter(const std::string& name, ESamplerType samplerType) noexcept {
        this->m_parameters[this->m_param_count++] = Parameter(name, samplerType);
        return *this;
    }

    MaterialBuilder& MaterialBuilder::SetTexture(const std::string& name, ETextureDimension textureType) noexcept {
        // this->m_parameters[this->m_param_count++] = Parameter(name, UniformType::UINT, 1);
        //For every texture, we add a  same name index uniform attribute
        SetParameter(name, UniformType::UINT, 1);
        return *this;
    }
    MaterialBuilder& MaterialBuilder::SetParameter(const std::string& name, UniformType type) noexcept {
        this->m_parameters[this->m_param_count++] = Parameter(name, type, 1);
        return *this;
    }
    MaterialBuilder& MaterialBuilder::SetParameter(const std::string& name, UniformType type, uint32_t size) noexcept {
        this->m_parameters[this->m_param_count++] = Parameter(name, type, size);
        return *this;
    }
    MaterialBuilder& MaterialBuilder::SetName(const std::string& name) noexcept {
        m_material_name = name;
        return *this;
    }
    MaterialBuilder& MaterialBuilder::SetType(EMaterialType type) noexcept {
        m_material_type = type;
        return *this;
    }

    // MaterialInstanceRef Material::createInstance() {
    //     return MoerNew(MaterialInstance);
    // }

    void Material::Impl::SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept {
        m_sampler_interface_block = std::move(sampler_interface_block);
    }
    const TextureInterfaceBlock& Material::Impl::GetSamplerInterfaceBlock() const noexcept {
        return m_sampler_interface_block;
    }
    const BufferInterfaceBlock& Material::Impl::GetBufferInterfaceBlock() const noexcept {
        return m_buffer_interface_block;
    }
    void Material::Impl::SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept {
        m_buffer_interface_block = std::move(buffer_interface_block);
    }
    EMaterialType Material::Impl::GetType() const noexcept {
        return m_type;
    }
    void Material::Impl::SetType(EMaterialType type) noexcept {
        m_type = type;
    }
    const std::string& Material::Impl::GetName() const noexcept {
        return m_name;
    }
    void Material::Impl::SetName(const std::string& name) noexcept {
        m_name = name;
    }
}// namespace Moer