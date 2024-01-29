#include "scene/TextureInterfaceBlock.h"
namespace Moer {

    TextureInterfaceBlock::Builder& TextureInterfaceBlock::Builder::Name(const std::string& name) {
        this->m_name = name;
        return *this;
    }

    TextureInterfaceBlock::Builder& TextureInterfaceBlock::Builder::AddCombinedSampler(const std::string& sampler_name, ESamplerType type, ETextureDimension format) noexcept {
        //todo
        return *this;
    }

    TextureInterfaceBlock::Builder& TextureInterfaceBlock::Builder::AddTexture(const std::string& texture_name, ETextureDimension type) noexcept {
        m_entries.emplace_back(TextureInfo{.name = texture_name, .offset = static_cast<uint8_t>(m_entries.size()), .textureType = type, .type = ESamplerBindingType::TEXTURE});
        return *this;
    }

    TextureInterfaceBlock::Builder& TextureInterfaceBlock::Builder::AddSampler(const std::string& sampler_name, ESamplerType type) noexcept {
        m_entries.emplace_back(TextureInfo{.name = sampler_name, .offset = static_cast<uint8_t>(m_entries.size()), .samplerType = type, .type = ESamplerBindingType::SAMPLER});
        return *this;
    }

    TextureInterfaceBlock TextureInterfaceBlock::Builder::Build() {
        return {*this};
    }

    TextureInterfaceBlock::TextureInfo const* TextureInterfaceBlock::GetSamplerInfo(const std::string& name) const noexcept {
        return &m_sampler_info_list[m_info_map.at(name)];
    }

    uint32_t TextureInterfaceBlock::GetSize() const noexcept {
        return static_cast<uint32_t>(m_sampler_info_list.size());
    }

    TextureInterfaceBlock::TextureInterfaceBlock(const Builder& builder) noexcept {
        for (auto& e : builder.m_entries) {
            m_info_map[e.name] = m_sampler_info_list.size();
            m_sampler_info_list.emplace_back(e);
        }
        m_name = builder.m_name;
    }
}