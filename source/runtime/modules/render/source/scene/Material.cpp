#include "scene/Material.h"

#include "scene/MaterialInstance.h"
#include "misc/MMemory.h"
#include "rhi/RHIResource.h"
#include "scene/TextureInterfaceBlock.h"
#include "shader/Shader.h"
namespace Moer {

    class Material::Impl {
        friend Material;
        Impl() = default;
        RHIShaderBoundStateInput m_shader_bound_state;
        TextureInterfaceBlock    m_sampler_interface_block;
    };

    Material::Material() {
        m_impl = new Impl();
    }
    MaterialInstanceRef Material::CreateInstance() {
        MaterialInstanceRef material_instance = MoerNew(MaterialInstance(this));
        return material_instance;
    }
    void Material::SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept {
        m_impl->m_sampler_interface_block = std::move(sampler_interface_block);
    }
    // MaterialInstanceRef Material::createInstance() {
    //     return MoerNew(MaterialInstance);
    // }
    const TextureInterfaceBlock& Material::GetSamplerInterfaceBlock() const noexcept {
        return m_impl->m_sampler_interface_block;
    }
    // const RHIShaderBoundStateInput& Material::getShaderBoundStateInput() const noexcept {
    //     return m_impl->m_shader_bound_state;
    // }
    MaterialRef MaterialBuilder::Build() {
        TextureInterfaceBlock::Builder sampler_interface_block_builder;
        sampler_interface_block_builder.Name("material samplers");
        for (const auto& paramater : mParameters) {
            if (paramater.isSampler()) {
                sampler_interface_block_builder.AddSampler(paramater.name.c_str(), paramater.samplerType);
            }
            if (paramater.isTexture()) {
                sampler_interface_block_builder.AddTexture(paramater.name.c_str(), paramater.textureType);
            }
        }

        TextureInterfaceBlock sampler_interface_block = sampler_interface_block_builder.Build();
        MaterialRef           material                = new Material();
        material->SetSamplerInterfaceBlock(sampler_interface_block);

        return material;
    }
    MaterialBuilder& MaterialBuilder::parameter(const std::string& name, ESamplerType samplerType) noexcept {
        this->mParameters[this->mParameterCount++] = Parameter(name, samplerType);
        return *this;
    }

    MaterialBuilder& MaterialBuilder::parameter(const std::string& name, ETextureDimension textureType) noexcept {
        this->mParameters[this->mParameterCount++] = Parameter(name, textureType);
        return *this;
    }

    // MaterialInstanceRef Material::createInstance() {
    //     return MoerNew(MaterialInstance);
    // }
}