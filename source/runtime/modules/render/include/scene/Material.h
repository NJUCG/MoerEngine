#pragma once
#include "TextureInterfaceBlock.h"
#include "misc/CountableRef.h"

namespace Moer {
    class TextureInterfaceBlock;
    class MaterialInstance;
    using MaterialInstanceRef = CountableRef<MaterialInstance>;

    class RENDER_API Material : public CountableResource {
    public:
        MaterialInstanceRef          CreateInstance();
        void                         SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept;
        const TextureInterfaceBlock& GetSamplerInterfaceBlock() const noexcept;
        Material();

    protected:
        class Impl;
        Impl* m_impl;
    };

    using MaterialRef = CountableRef<Material>;

    class MaterialBuilder {
        struct RENDER_API Parameter {
            Parameter(const std::string name_, ESamplerType type) : name(name_), samplerType(type), type(ESamplerBindingType::SAMPLER) {
            }
            Parameter(const std::string name_, ETextureDimension type) : name(name_), textureType(type), type(ESamplerBindingType::TEXTURE) {
            }
            Parameter(const std::string name_, ESamplerType samplerType, ETextureDimension textureType) : name(name_), samplerType(samplerType), textureType(textureType), type(ESamplerBindingType::COMBINED) {
            }
            Parameter() = default;
            bool isSampler() const noexcept {
                return type == ESamplerBindingType::SAMPLER;
            }
            bool isTexture() const noexcept {
                return type == ESamplerBindingType::TEXTURE;
            }
            bool isCombinedSampler() const noexcept {
                return type == ESamplerBindingType::COMBINED;
            }
            std::string         name;
            ESamplerType        samplerType{ESamplerType::SAMPLER_2D};
            ETextureDimension   textureType{ETextureDimension::TEX_2D};
            ESamplerBindingType type{ESamplerBindingType::UNDEFINED};
        };

    public:
        RENDER_API MaterialBuilder& parameter(const std::string& name, ESamplerType samplerType) noexcept;
        RENDER_API MaterialBuilder& parameter(const std::string& name, ETextureDimension textureType) noexcept;

        RENDER_API MaterialRef Build();

    protected:
        // constexpr static uint32_t MAX_PARAMETER_COUNT = 16;
        Parameter mParameters[32];
        uint32_t  mParameterCount{0};
    };

}