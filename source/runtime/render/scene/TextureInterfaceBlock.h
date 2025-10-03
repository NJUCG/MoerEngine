#pragma once
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "serialize/Serializer.h"

namespace Moer {
class SceneCache;
}
namespace Moer {

class TextureInterfaceBlock {
public:
    struct TextureInfo {
        std::string       name{};
        uint32_t          offset{0};
        ESamplerType      sampler_type{ESamplerType::SAMPLER_2D};
        ETextureDimension texture_type{ETextureDimension::TEX_2D};
        EParamaterType    type{EParamaterType::UNDEFINED};

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << name << offset << sampler_type << texture_type << type;
            return _stream;
        }

        InputStream& operator>>(InputStream& _stream) {
            _stream >> name >> offset >> sampler_type >> texture_type >> type;
            return _stream;
        }
    };
    class Builder {
    public:
        Builder& AddSampler(const std::string& _sampler_name, ESamplerType _type) noexcept;
        Builder& AddTexture(const std::string& texture_name, ETextureDimension type) noexcept;
        Builder& AddCombinedSampler(
            const std::string& sampler_name,
            ESamplerType       type,
            ETextureDimension  format
        ) noexcept;
        Builder&              Name(const std::string& name);
        TextureInterfaceBlock Build();

    protected:
        friend class TextureInterfaceBlock;
        Array<TextureInfo> m_entries;
        std::string        m_name;
    };
    const Moer::Array<TextureInfo>& GetSamplerInfoList() const noexcept {
        return m_sampler_info_list;
    }
    TextureInfo const* GetSamplerInfo(const std::string& name) const noexcept;
    uint32_t           GetSize() const noexcept;
    TextureInterfaceBlock(const Builder& builder) noexcept;
    TextureInterfaceBlock() noexcept = default;

private:
    friend SceneCache;
    Array<TextureInfo>                 m_sampler_info_list;
    UnorderedMap<std::string, uint8_t> m_info_map;
    std::string                        m_name;
};

} // namespace Moer