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
        static constexpr uint32_t MaterialBytesNum = 512;
        const std::string&           GetName() const noexcept;
        void                         SetName(const std::string& name) noexcept;
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

    struct MaterialComponent {
        static constexpr uint32_t MaterialBytesNum = 512;
        enum FLAGS {
            EMPTY                         = 0,
            DIRTY                         = 1 << 0,
            CAST_SHADOW                   = 1 << 1,
            _DEPRECATED_PLANAR_REFLECTION = 1 << 2,
            _DEPRECATED_WATER             = 1 << 3,
            _DEPRECATED_FLIP_NORMALMAP    = 1 << 4,
            USE_VERTEXCOLORS              = 1 << 5,
            SPECULAR_GLOSSINESS_WORKFLOW  = 1 << 6,
            OCCLUSION_PRIMARY             = 1 << 7,
            OCCLUSION_SECONDARY           = 1 << 8,
            USE_WIND                      = 1 << 9,
            DISABLE_RECEIVE_SHADOW        = 1 << 10,
            DOUBLE_SIDED                  = 1 << 11,
            OUTLINE                       = 1 << 12,
            PREFER_UNCOMPRESSED_TEXTURES  = 1 << 13,
            DISABLE_VERTEXAO              = 1 << 14,
        };
        uint32_t _flags = CAST_SHADOW;

        enum SHADERTYPE {
            SHADERTYPE_PBR,
            SHADERTYPE_PBR_PLANARREFLECTION,
            SHADERTYPE_PBR_PARALLAXOCCLUSIONMAPPING,
            SHADERTYPE_PBR_ANISOTROPIC,
            SHADERTYPE_WATER,
            SHADERTYPE_CARTOON,
            SHADERTYPE_UNLIT,
            SHADERTYPE_PBR_CLOTH,
            SHADERTYPE_PBR_CLEARCOAT,
            SHADERTYPE_PBR_CLOTH_CLEARCOAT,
            SHADERTYPE_PBR_TERRAINBLENDED,
            SHADERTYPE_COUNT
        } shaderType = SHADERTYPE_PBR;

        inline static const Moer::Array<std::string> shaderTypeDefines[] = {
            {},                          // SHADERTYPE_PBR,
            {"PLANARREFLECTION"},        // SHADERTYPE_PBR_PLANARREFLECTION,
            {"PARALLAXOCCLUSIONMAPPING"},// SHADERTYPE_PBR_PARALLAXOCCLUSIONMAPPING,
            {"ANISOTROPIC"},             // SHADERTYPE_PBR_ANISOTROPIC,
            {"WATER"},                   // SHADERTYPE_WATER,
            {"CARTOON"},                 // SHADERTYPE_CARTOON,
            {"UNLIT"},                   // SHADERTYPE_UNLIT,
            {"SHEEN"},                   // SHADERTYPE_PBR_CLOTH,
            {"CLEARCOAT"},               // SHADERTYPE_PBR_CLEARCOAT,
            {"SHEEN", "CLEARCOAT"},      // SHADERTYPE_PBR_CLOTH_CLEARCOAT,
            {"TERRAINBLENDED"},          //SHADERTYPE_PBR_TERRAINBLENDED
        };
        static_assert(SHADERTYPE_COUNT == sizeof(shaderTypeDefines) / sizeof(shaderTypeDefines[0]), "These values must match!");

        uint8_t               userStencilRef   = 0;

        Moer::Vector4f
            baseColor = Moer::Vector4f(1, 1, 1, 1);
        Moer::Vector4f
            specularColor = Moer::Vector4f(1, 1, 1, 1);
        Moer::Vector4f
            emissiveColor = Moer::Vector4f(1, 1, 1, 0);
        Moer::Vector4f
            subsurfaceScattering = Moer::Vector4f(1, 1, 1, 0);
        Moer::Vector4f
              texMulAdd                 = Moer::Vector4f(1, 1, 0, 0);
        float roughness                 = 0.2f;
        float reflectance               = 0.02f;
        float metalness                 = 0.0f;
        float normalMapStrength         = 1.0f;
        float parallaxOcclusionMapping  = 0.0f;
        float displacementMapping       = 0.0f;
        float refraction                = 0.0f;
        float transmission              = 0.0f;
        float alphaRef                  = 1.0f;
        float anisotropy_strength       = 0;
        float anisotropy_rotation       = 0;//radians, counter-clockwise
        float blend_with_terrain_height = 0;

        Moer::Vector4f
              sheenColor         = Moer::Vector4f(1, 1, 1, 1);
        float sheenRoughness     = 0;
        float clearcoat          = 0;
        float clearcoatRoughness = 0;

        Moer::Vector2f
              texAnimDirection   = Moer::Vector2f(0, 0);
        float texAnimFrameRate   = 0.0f;
        float texAnimElapsedTime = 0.0f;

        enum TEXTURESLOT {
            BASECOLORMAP,
            NORMALMAP,
            SURFACEMAP,
            EMISSIVEMAP,
            DISPLACEMENTMAP,
            OCCLUSIONMAP,
            TRANSMISSIONMAP,
            SHEENCOLORMAP,
            SHEENROUGHNESSMAP,
            CLEARCOATMAP,
            CLEARCOATROUGHNESSMAP,
            CLEARCOATNORMALMAP,
            SPECULARMAP,
            ANISOTROPYMAP,
            TRANSPARENCYMAP,

            TEXTURESLOT_COUNT
        };
        struct TextureMap {
            std::string        name;
            Render::TextureRef resource;
            uint32_t           uvset = 0;
            // Non-serialized attributes:
            float lod_clamp                      = 0; // optional, can be used by texture streaming
            int   sparse_residencymap_descriptor = -1;// optional, can be used by texture streaming
            int   sparse_feedbackmap_descriptor  = -1;// optional, can be used by texture streaming
        };
        TextureMap textures[TEXTURESLOT_COUNT];

        int   customShaderID = -1;
        uint4 userdata       = uint4(0, 0, 0, 0);// can be accessed by custom shader

        // Non-serialized attributes:
        uint32_t layerMask          = ~0u;
        int      sampler_descriptor = -1;// optional

        // User stencil value can be in range [0, 15]
        inline void SetUserStencilRef(uint8_t value) {
            assert(value < 16);
            userStencilRef = value & 0x0F;
        }
        uint32_t GetStencilRef() const;

        inline float GetOpacity() const { return baseColor.w; }
        inline float GetEmissiveStrength() const { return emissiveColor.w; }
        inline int   GetCustomShaderID() const { return customShaderID; }

        inline bool HasPlanarReflection() const { return shaderType == SHADERTYPE_PBR_PLANARREFLECTION || shaderType == SHADERTYPE_WATER; }

        inline void SetDirty(bool value = true) {
            if (value) {
                _flags |= DIRTY;
            } else {
                _flags &= ~DIRTY;
            }
        }
        inline bool IsDirty() const { return _flags & DIRTY; }

        inline void SetCastShadow(bool value) {
            SetDirty();
            if (value) {
                _flags |= CAST_SHADOW;
            } else {
                _flags &= ~CAST_SHADOW;
            }
        }
        inline void SetReceiveShadow(bool value) {
            SetDirty();
            if (value) {
                _flags &= ~DISABLE_RECEIVE_SHADOW;
            } else {
                _flags |= DISABLE_RECEIVE_SHADOW;
            }
        }
        inline void SetOcclusionEnabled_Primary(bool value) {
            SetDirty();
            if (value) {
                _flags |= OCCLUSION_PRIMARY;
            } else {
                _flags &= ~OCCLUSION_PRIMARY;
            }
        }
        inline void SetOcclusionEnabled_Secondary(bool value) {
            SetDirty();
            if (value) {
                _flags |= OCCLUSION_SECONDARY;
            } else {
                _flags &= ~OCCLUSION_SECONDARY;
            }
        }

        // inline wi::enums::BLENDMODE GetBlendMode() const { if (userBlendMode == wi::enums::BLENDMODE_OPAQUE && (GetFilterMask() & wi::enums::FILTER_TRANSPARENT)) return wi::enums::BLENDMODE_ALPHA; else return userBlendMode; }
        inline bool IsCastingShadow() const { return _flags & CAST_SHADOW; }
        inline bool IsAlphaTestEnabled() const { return alphaRef <= 1.0f - 1.0f / 256.0f; }
        inline bool IsUsingVertexColors() const { return _flags & USE_VERTEXCOLORS; }
        inline bool IsUsingWind() const { return _flags & USE_WIND; }
        inline bool IsReceiveShadow() const { return (_flags & DISABLE_RECEIVE_SHADOW) == 0; }
        inline bool IsUsingSpecularGlossinessWorkflow() const { return _flags & SPECULAR_GLOSSINESS_WORKFLOW; }
        inline bool IsOcclusionEnabled_Primary() const { return _flags & OCCLUSION_PRIMARY; }
        inline bool IsOcclusionEnabled_Secondary() const { return _flags & OCCLUSION_SECONDARY; }
        inline bool IsCustomShader() const { return customShaderID >= 0; }
        inline bool IsDoubleSided() const { return _flags & DOUBLE_SIDED; }
        inline bool IsOutlineEnabled() const { return _flags & OUTLINE; }
        inline bool IsPreferUncompressedTexturesEnabled() const { return _flags & PREFER_UNCOMPRESSED_TEXTURES; }
        inline bool IsVertexAODisabled() const { return _flags & DISABLE_VERTEXAO; }

        inline void SetBaseColor(const Moer::Vector4f& value) {
            SetDirty();
            baseColor = value;
        }
        inline void SetSpecularColor(const Moer::Vector4f& value) {
            SetDirty();
            specularColor = value;
        }
        inline void SetEmissiveColor(const Moer::Vector4f& value) {
            SetDirty();
            emissiveColor = value;
        }
        inline void SetRoughness(float value) {
            SetDirty();
            roughness = value;
        }
        inline void SetReflectance(float value) {
            SetDirty();
            reflectance = value;
        }
        inline void SetMetalness(float value) {
            SetDirty();
            metalness = value;
        }
        inline void SetEmissiveStrength(float value) {
            SetDirty();
            emissiveColor.w = value;
        }
        inline void SetTransmissionAmount(float value) {
            SetDirty();
            transmission = value;
        }
        inline void SetRefractionAmount(float value) {
            SetDirty();
            refraction = value;
        }
        inline void SetNormalMapStrength(float value) {
            SetDirty();
            normalMapStrength = value;
        }
        inline void SetParallaxOcclusionMapping(float value) {
            SetDirty();
            parallaxOcclusionMapping = value;
        }
        inline void SetDisplacementMapping(float value) {
            SetDirty();
            displacementMapping = value;
        }
        inline void SetSubsurfaceScatteringColor(Moer::Vector3f
                                                     value) {
            SetDirty();
            subsurfaceScattering.x = value.x;
            subsurfaceScattering.y = value.y;
            subsurfaceScattering.z = value.z;
        }
        inline void SetSubsurfaceScatteringAmount(float value) {
            SetDirty();
            subsurfaceScattering.w = value;
        }
        inline void SetOpacity(float value) {
            SetDirty();
            baseColor.w = value;
        }
        inline void SetAlphaRef(float value) {
            SetDirty();
            alphaRef = value;
        }
        inline void SetUseVertexColors(bool value) {
            SetDirty();
            if (value) {
                _flags |= USE_VERTEXCOLORS;
            } else {
                _flags &= ~USE_VERTEXCOLORS;
            }
        }
        inline void SetUseWind(bool value) {
            SetDirty();
            if (value) {
                _flags |= USE_WIND;
            } else {
                _flags &= ~USE_WIND;
            }
        }
        inline void SetUseSpecularGlossinessWorkflow(bool value) {
            SetDirty();
            if (value) {
                _flags |= SPECULAR_GLOSSINESS_WORKFLOW;
            } else {
                _flags &= ~SPECULAR_GLOSSINESS_WORKFLOW;
            }
        }
        inline void SetSheenColor(const Moer::Vector3f& value) {
            sheenColor = Moer::Vector4f(value.x, value.y, value.z, sheenColor.w);
            SetDirty();
        }
        inline void SetSheenRoughness(float value) {
            sheenRoughness = value;
            SetDirty();
        }
        inline void SetClearcoatFactor(float value) {
            clearcoat = value;
            SetDirty();
        }
        inline void SetClearcoatRoughness(float value) {
            clearcoatRoughness = value;
            SetDirty();
        }
        inline void SetCustomShaderID(int id) { customShaderID = id; }
        inline void DisableCustomShader() { customShaderID = -1; }
        inline void SetDoubleSided(bool value = true) {
            if (value) {
                _flags |= DOUBLE_SIDED;
            } else {
                _flags &= ~DOUBLE_SIDED;
            }
        }
        inline void SetOutlineEnabled(bool value = true) {
            if (value) {
                _flags |= OUTLINE;
            } else {
                _flags &= ~OUTLINE;
            }
        }
        inline void SetPreferUncompressedTexturesEnabled(bool value = true) {
            if (value) {
                _flags |= PREFER_UNCOMPRESSED_TEXTURES;
            } else {
                _flags &= ~PREFER_UNCOMPRESSED_TEXTURES;
            }
            CreateRenderData(true);
        }
        inline void SetVertexAODisabled(bool value = true) {
            if (value) {
                _flags |= DISABLE_VERTEXAO;
            } else {
                _flags &= ~DISABLE_VERTEXAO;
            }
        }

        // The MaterialComponent will be written to ShaderMaterial (a struct that is optimized for GPU use)
        Moer::StaticArray<char, MaterialComponent::MaterialBytesNum> WriteShaderMaterial() const;
        void                                                         WriteShaderTextureSlot(int slot, int descriptor);

        // Retrieve the array of textures from the material

        // Returns the bitwise OR of all the wi::enums::FILTER flags applicable to this material
        uint32_t GetFilterMask() const;


        // Create texture resources for GPU
        void CreateRenderData(bool force_recreate = false);

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
        RENDER_API MaterialBuilder& SetTexture(const std::string& name, ETextureDimension textureType) noexcept;
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, UniformType type) noexcept;
        RENDER_API MaterialBuilder& SetParameter(const std::string& name, UniformType type, uint32_t size) noexcept;
        RENDER_API MaterialBuilder& SetName(const std::string& name) noexcept;
        RENDER_API MaterialBuilder& SetType(EMaterialType type) noexcept;
        RENDER_API MaterialRef      Build();
    protected:
        // constexpr static uint32_t MAX_PARAMETER_COUNT = 16;
        Parameter   m_parameters[32];
        uint32_t    m_param_count{0};
        std::string m_material_name;
        EMaterialType m_material_type{EMaterialType::E_PBR_STANDARD};
    };

}