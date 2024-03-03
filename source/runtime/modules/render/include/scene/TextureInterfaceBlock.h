#pragma once
#include "misc/STL.h"
#include "rhi/RHICommon.h"

namespace Moer {

    class TextureInterfaceBlock {
    public:
        struct TextureInfo {
            std::string         name{};
            uint32_t            offset{0};
            ESamplerType        samplerType{ESamplerType::SAMPLER_2D};
            ETextureDimension   textureType{ETextureDimension::TEX_2D};
            ESamplerBindingType type{ESamplerBindingType::UNDEFINED};
        };
        class Builder {
        public:
            Builder&              AddSampler(const std::string& sampler_name, ESamplerType type) noexcept;
            Builder&              AddTexture(const std::string& texture_name, ETextureDimension type) noexcept;
            Builder&              AddCombinedSampler(const std::string& sampler_name, ESamplerType type, ETextureDimension format) noexcept;
            Builder&              Name(const std::string& name);
            TextureInterfaceBlock Build();

        protected:
            friend class TextureInterfaceBlock;
            Array<TextureInfo> m_entries;
            std::string        m_name;
        };
        TextureInfo const* GetSamplerInfo(const std::string& name) const noexcept;
        uint32_t           GetSize() const noexcept;
        TextureInterfaceBlock(const Builder& builder) noexcept;
        TextureInterfaceBlock() noexcept = default;

    private:
        Array<TextureInfo>                 m_sampler_info_list;
        UnorderedMap<std::string, uint8_t> m_info_map;
        std::string                        m_name;
    };
}