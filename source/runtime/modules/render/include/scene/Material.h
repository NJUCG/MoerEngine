#pragma once
#include "BufferInterfaceBlock.h"
#include "TextureInterfaceBlock.h"
#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

namespace Moer {
    class TextureInterfaceBlock;
    class MaterialInstance;
    using MaterialInstanceRef = CountableRef<MaterialInstance>;
    class BufferInterfaceBlock;

    enum class EMaterialType : uint32_t {
        E_PBR_STANDARD,
        E_HAIR,
        E_CLOTH,
        E_MATERIAL_NUM
    };

    class RENDER_API Material : public CountableResource {
    public:
        MaterialInstanceRef          CreateInstance();
        void                         SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept;
        const TextureInterfaceBlock& GetSamplerInterfaceBlock() const noexcept;
        void                         SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept;
        const BufferInterfaceBlock&  GetBufferInterfaceBlock() const noexcept;
        EMaterialType                GetType() const noexcept;
        void                         SetType(EMaterialType type) noexcept;
        void                         OrganizeInstancesAndBind(RHIBatchedShaderParameters& parameters, Moer::Array<MaterialInstanceRef> instances);
        Material();

    protected:
        class Impl;
        Impl* m_impl;
    };

    using MaterialRef = CountableRef<Material>;

    class MaterialBuilder {

        struct RENDER_API Parameter {
            Parameter(const std::string name_, ESamplerType type) : name(name_), samplerType(type), type(EParamaterType::SAMPLER) {
            }
            Parameter(const std::string name_, ETextureDimension type) : name(name_), textureType(type), type(EParamaterType::TEXTURE) {
            }
            Parameter(const std::string name_, ESamplerType samplerType, ETextureDimension textureType) : name(name_), samplerType(samplerType), textureType(textureType), type(EParamaterType::COMBINED) {
            }
            Parameter(const std::string name_, UniformType type, uint32_t size) : name(name_), uniformType(type), type(EParamaterType::UNIFORM), size(size) {
            }
            Parameter() = default;
            bool IsSampler() const noexcept {
                return type == EParamaterType::SAMPLER;
            }
            bool IsTexture() const noexcept {
                return type == EParamaterType::TEXTURE;
            }
            bool IsCombinedSampler() const noexcept {
                return type == EParamaterType::COMBINED;
            }
            bool IsUniform() const noexcept {
                return type == EParamaterType::UNIFORM;
            }
            std::string       name;
            ESamplerType      samplerType{ESamplerType::SAMPLER_2D};
            ETextureDimension textureType{ETextureDimension::TEX_2D};
            uint32_t          size{1};
            UniformType       uniformType{UniformType::FLOAT};
            EParamaterType    type{EParamaterType::UNDEFINED};
        };

    public:
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, ESamplerType samplerType) noexcept;
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, ETextureDimension textureType) noexcept;
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, UniformType type) noexcept;
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, UniformType type, uint32_t size) noexcept;

        RENDER_API MaterialRef Build();

    protected:
        // constexpr static uint32_t MAX_PARAMETER_COUNT = 16;
        Parameter mParameters[32];
        uint32_t  mParameterCount{0};
    };

}