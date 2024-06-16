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
        void                         OrganizeInstancesAndBind(RHIBatchedShaderParameters& parameters, Moer::Array<MaterialInstanceRef> instances);
        void                         SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept;
        const BufferInterfaceBlock&  GetBufferInterfaceBlock() const noexcept;
        EMaterialType                GetType() const noexcept;
        void                         SetType(EMaterialType type) noexcept;
        void                         SetName(const std::string& name) noexcept;
        const std::string&           GetName() const noexcept;

        std::string              m_name;
        RHIShaderBoundStateInput m_shader_bound_state;
        TextureInterfaceBlock    m_sampler_interface_block;
        BufferInterfaceBlock     m_buffer_interface_block;
        EMaterialType            m_type;
        RHIBufferRef             m_material_data_buffer{nullptr};
        RHISRVRef                m_material_data_srv{nullptr};
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
    void Material::OrganizeInstancesAndBind(RHIBatchedShaderParameters& parameters, Moer::Array<MaterialInstanceRef> instances) {
        return m_impl->OrganizeInstancesAndBind(parameters, instances);
    }
    // const RHIShaderBoundStateInput& Material::getShaderBoundStateInput() const noexcept {
    //     return m_impl->m_shader_bound_state;
    // }
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
        return material;
    }
    MaterialBuilder& MaterialBuilder::SetParameter(const std::string& name, ESamplerType samplerType) noexcept {
        this->m_parameters[this->m_param_count++] = Parameter(name, samplerType);
        return *this;
    }

    MaterialBuilder& MaterialBuilder::SetParameter(const std::string& name, ETextureDimension textureType) noexcept {
        this->m_parameters[this->m_param_count++] = Parameter(name, textureType);
        //For every texture, we add a  same name index uniform attribute
        SetParameter(name, UniformType::INT);
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

    // MaterialInstanceRef Material::createInstance() {
    //     return MoerNew(MaterialInstance);
    // }

    void Material::Impl::SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept {
        m_sampler_interface_block = std::move(sampler_interface_block);
    }
    const TextureInterfaceBlock& Material::Impl::GetSamplerInterfaceBlock() const noexcept {
        return m_sampler_interface_block;
    }
    void Material::Impl::OrganizeInstancesAndBind(RHIBatchedShaderParameters& parameters, Moer::Array<MaterialInstanceRef> instances) {
        if (m_type == EMaterialType::E_PBR_STANDARD) {
            Moer::Array<MaterialData>               material_data(instances.size());
            Moer::Array<RHITextureRef>              textures;
            RHISamplerRef                           default_sampler;
            Moer::UnorderedMap<RHITexture*, size_t> texture_idx;
            uint32_t                                instance_idx = 0;
            //Just organize texture idx for material instances
            for (auto& mi : instances) {
                memcpy(&material_data[instance_idx], mi->GetUniformBuffer().GetData(), sizeof(MaterialData));
                auto&       mat_data               = material_data[instance_idx++];
                RHITexture* albedo_map             = mi->GetTexture("albedo_map");
                RHITexture* normal_map             = mi->GetTexture("normal_map");
                RHITexture* metallic_roughness_map = mi->GetTexture("metallic_roughness_map");
                RHITexture* ao_map                 = mi->GetTexture("ao_map");
                RHITexture* emissive_map           = mi->GetTexture("emissive_map");

                static auto find_or_insert = [&](RHITexture* texture, int* idx) {
                    if (!texture) {
                        *idx = -1;
                        return;
                    }
                    auto it = texture_idx.find(texture);
                    if (it == texture_idx.end()) {
                        *idx = textures.size();
                        textures.emplace_back(CountableRef(texture));
                        texture_idx[texture] = *idx;
                    }
                };
                find_or_insert(albedo_map, &mat_data.albedo_map);
                find_or_insert(normal_map, &mat_data.normal_map);
                find_or_insert(metallic_roughness_map, &mat_data.metallic_roughness_map);
                find_or_insert(ao_map, &mat_data.ao_map);
                find_or_insert(emissive_map, &mat_data.emissive_map);
            }

            default_sampler = SamplerCache::Get().GetSampler(SamplerParams(SF_CUBIC, TEXTURE_LAYOUT_UNDEFINED));

            if (!m_material_data_buffer || m_material_data_buffer->GetByteSize() != sizeof(MaterialData) * instances.size()) {
                m_material_data_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, material_data.data(), sizeof(MaterialData) * instances.size());
                // void* mapped_data      = g_rhi->RHIMapBuffer(m_material_data_buffer, 0, sizeof(MaterialData) * instances.size());
                // memcpy(mapped_data, material_data.data(), sizeof(MaterialData) * instances.size());
                m_material_data_srv = g_rhi->RHICreateBufferSRV(m_material_data_buffer);
            }
            if (textures.empty())
                return;
            uint32_t       offset          = 0;
            constexpr uint max_binding_cnt = 25;
            uint           binding_size    = std::max(uint(textures.size()), max_binding_cnt);
            auto           last_srv        = RenderGraphResourceCache::Get().GetSRV(textures[0], textures[0]->GetFormat(), 0, textures[0]->GetNumMips(), 0, textures[0]->GetInfo().array_size);
            for (size_t i = 0; i < binding_size; i++) {
                if (i >= textures.size()) {
                    parameters.SetParameters(last_srv, i + offset, 2);
                    continue;
                }
                RHISRVRef srv = RenderGraphResourceCache::Get().GetSRV(textures[i], textures[i]->GetFormat(), 0, textures[i]->GetNumMips(), 0, textures[i]->GetInfo().array_size);
                parameters.SetParameters(srv, i + offset, 2);
                // break;
            }
            default_sampler = RenderGraphResourceCache::Get().GetSampler({});
            parameters.SetParameters(default_sampler, 4, 1);
            parameters.SetParameters(m_material_data_srv, 0, 0);
            //Bind these resources
        } else {
            //todo
        }
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