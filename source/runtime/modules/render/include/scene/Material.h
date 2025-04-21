#pragma once
#include "BufferInterfaceBlock.h"
#include "TextureInterfaceBlock.h"
#include "misc/CountableRef.h"
#include "rhi/RHIResource.h"

#include "scene/MaterialInstance.h"
// Add the above line to fix: CountableRef.h(120,13): error C2027: 使用了未定义类型“Moer::MaterialInstance”

namespace Moer {
    class TextureInterfaceBlock;
    class MaterialInstance;
    using MaterialInstanceRef = CountableRef<MaterialInstance>;
    class BufferInterfaceBlock;

    struct [[deprecated("ECS: PackedMaterialData is not used yet")]] PackedMaterialData {
        float4 packed_0;
        float4 packed_1;
        float4 packed_2;
        float4 packed_3;

        float4 packed_4;
        float4 packed_5;
        float4 packed_6;
        float4 packed_7;
    };

    enum class EMaterialType : uint32_t {
        E_PBR_STANDARD,
        E_HAIR,
        E_CLOTH,
        E_MATERIAL_NUM
    };

    class RENDER_API Material : public CountableResource {
    public:
        static constexpr uint32_t    MaterialBytesNum = 512;
        const std::string&           GetName() const noexcept;
        void                         SetName(const std::string& name) noexcept;
        MaterialInstanceRef          CreateInstance();
        void                         SetSamplerInterfaceBlock(TextureInterfaceBlock& sampler_interface_block) noexcept;
        const TextureInterfaceBlock& GetSamplerInterfaceBlock() const noexcept;
        void                         SetBufferInterfaceBlock(BufferInterfaceBlock& buffer_interface_block) noexcept;
        const BufferInterfaceBlock&  GetBufferInterfaceBlock() const noexcept;
        EMaterialType                GetType() const noexcept;
        void                         SetType(EMaterialType type) noexcept;
        Material();

    protected:
        class Impl;
        Impl* m_impl;
    };

    struct [[deprecated("ECS: MaterialComponent is not used yet")]] MaterialComponent {
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
        uint32_t flags = CAST_SHADOW;

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
        } shader_type = SHADERTYPE_PBR;

        inline static const Moer::Array<std::string> shader_type_defines[] = {
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
        static_assert(SHADERTYPE_COUNT == sizeof(shader_type_defines) / sizeof(shader_type_defines[0]), "These values must match!");

        uint8_t user_stencil_ref = 0;

        Moer::Vector4f
            base_color = Moer::Vector4f(1, 1, 1, 1);
        Moer::Vector4f
            specular_color = Moer::Vector4f(1, 1, 1, 1);
        Moer::Vector4f
            emissive_color = Moer::Vector4f(1, 1, 1, 0);
        Moer::Vector4f
            subsurface_scattering = Moer::Vector4f(1, 1, 1, 0);
        Moer::Vector4f
              tex_mul_add                = Moer::Vector4f(1, 1, 0, 0);
        float roughness                  = 0.2f;
        float reflectance                = 0.02f;
        float metalness                  = 0.0f;
        float normal_map_strength        = 1.0f;
        float parallax_occlusion_mapping = 0.0f;
        float displacement_mapping       = 0.0f;
        float refraction                 = 0.0f;
        float transmission               = 0.0f;
        float alpha_ref                  = 1.0f;
        float anisotropy_strength        = 0;
        float anisotropy_rotation        = 0;//radians, counter-clockwise
        float blend_with_terrain_height  = 0;

        Moer::Vector4f
              sheen_color         = Moer::Vector4f(1, 1, 1, 1);
        float sheen_roughness     = 0;
        float clearcoat           = 0;
        float clearcoat_roughness = 0;

        Moer::Vector2f
              tex_anim_direction    = Moer::Vector2f(0, 0);
        float tex_anim_frame_rate   = 0.0f;
        float tex_anim_elapsed_time = 0.0f;

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

        int   custom_shader_id = -1;
        uint4 userdata         = uint4(0, 0, 0, 0);// can be accessed by custom shader

        // Non-serialized attributes:
        uint32_t layer_mask         = ~0u;
        int      sampler_descriptor = -1;// optional

        // User stencil value can be in range [0, 15]
        inline void SetUserStencilRef(uint8_t _value) {
            assert(_value < 16);
            user_stencil_ref = _value & 0x0F;
        }
        uint32_t GetStencilRef() const;

        inline float GetOpacity() const { return base_color.w; }
        inline float GetEmissiveStrength() const { return emissive_color.w; }
        inline int   GetCustomShaderID() const { return custom_shader_id; }

        inline bool HasPlanarReflection() const { return shader_type == SHADERTYPE_PBR_PLANARREFLECTION || shader_type == SHADERTYPE_WATER; }

        inline void SetDirty(bool _value = true) {
            if (_value) {
                flags |= DIRTY;
            } else {
                flags &= ~DIRTY;
            }
        }
        inline bool IsDirty() const { return flags & DIRTY; }

        inline void SetCastShadow(bool _value) {
            SetDirty();
            if (_value) {
                flags |= CAST_SHADOW;
            } else {
                flags &= ~CAST_SHADOW;
            }
        }
        inline void SetReceiveShadow(bool _value) {
            SetDirty();
            if (_value) {
                flags &= ~DISABLE_RECEIVE_SHADOW;
            } else {
                flags |= DISABLE_RECEIVE_SHADOW;
            }
        }
        inline void SetOcclusionEnabledPrimary(bool _value) {
            SetDirty();
            if (_value) {
                flags |= OCCLUSION_PRIMARY;
            } else {
                flags &= ~OCCLUSION_PRIMARY;
            }
        }
        inline void SetOcclusionEnabledSecondary(bool _value) {
            SetDirty();
            if (_value) {
                flags |= OCCLUSION_SECONDARY;
            } else {
                flags &= ~OCCLUSION_SECONDARY;
            }
        }

        // inline wi::enums::BLENDMODE GetBlendMode() const { if (userBlendMode == wi::enums::BLENDMODE_OPAQUE && (GetFilterMask() & wi::enums::FILTER_TRANSPARENT)) return wi::enums::BLENDMODE_ALPHA; else return userBlendMode; }
        inline bool IsCastingShadow() const { return flags & CAST_SHADOW; }
        inline bool IsAlphaTestEnabled() const { return alpha_ref <= 1.0f - 1.0f / 256.0f; }
        inline bool IsUsingVertexColors() const { return flags & USE_VERTEXCOLORS; }
        inline bool IsUsingWind() const { return flags & USE_WIND; }
        inline bool IsReceiveShadow() const { return (flags & DISABLE_RECEIVE_SHADOW) == 0; }
        inline bool IsUsingSpecularGlossinessWorkflow() const { return flags & SPECULAR_GLOSSINESS_WORKFLOW; }
        inline bool IsOcclusionEnabledPrimary() const { return flags & OCCLUSION_PRIMARY; }
        inline bool IsOcclusionEnabledSecondary() const { return flags & OCCLUSION_SECONDARY; }
        inline bool IsCustomShader() const { return custom_shader_id >= 0; }
        inline bool IsDoubleSided() const { return flags & DOUBLE_SIDED; }
        inline bool IsOutlineEnabled() const { return flags & OUTLINE; }
        inline bool IsPreferUncompressedTexturesEnabled() const { return flags & PREFER_UNCOMPRESSED_TEXTURES; }
        inline bool IsVertexAODisabled() const { return flags & DISABLE_VERTEXAO; }

        inline void SetBaseColor(const Moer::Vector4f& value) {
            SetDirty();
            base_color = value;
        }
        inline void SetSpecularColor(const Moer::Vector4f& value) {
            SetDirty();
            specular_color = value;
        }
        inline void SetEmissiveColor(const Moer::Vector4f& value) {
            SetDirty();
            emissive_color = value;
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
            emissive_color.w = value;
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
            normal_map_strength = value;
        }
        inline void SetParallaxOcclusionMapping(float value) {
            SetDirty();
            parallax_occlusion_mapping = value;
        }
        inline void SetDisplacementMapping(float value) {
            SetDirty();
            displacement_mapping = value;
        }
        inline void SetSubsurfaceScatteringColor(Moer::Vector3f
                                                     value) {
            SetDirty();
            subsurface_scattering.x = value.x;
            subsurface_scattering.y = value.y;
            subsurface_scattering.z = value.z;
        }
        inline void SetSubsurfaceScatteringAmount(float value) {
            SetDirty();
            subsurface_scattering.w = value;
        }
        inline void SetOpacity(float value) {
            SetDirty();
            base_color.w = value;
        }
        inline void SetAlphaRef(float value) {
            SetDirty();
            alpha_ref = value;
        }
        inline void SetUseVertexColors(bool value) {
            SetDirty();
            if (value) {
                flags |= USE_VERTEXCOLORS;
            } else {
                flags &= ~USE_VERTEXCOLORS;
            }
        }
        inline void SetUseWind(bool value) {
            SetDirty();
            if (value) {
                flags |= USE_WIND;
            } else {
                flags &= ~USE_WIND;
            }
        }
        inline void SetUseSpecularGlossinessWorkflow(bool value) {
            SetDirty();
            if (value) {
                flags |= SPECULAR_GLOSSINESS_WORKFLOW;
            } else {
                flags &= ~SPECULAR_GLOSSINESS_WORKFLOW;
            }
        }
        inline void SetSheenColor(const Moer::Vector3f& value) {
            sheen_color = Moer::Vector4f(value.x, value.y, value.z, sheen_color.w);
            SetDirty();
        }
        inline void SetSheenRoughness(float value) {
            sheen_roughness = value;
            SetDirty();
        }
        inline void SetClearcoatFactor(float value) {
            clearcoat = value;
            SetDirty();
        }
        inline void SetClearcoatRoughness(float value) {
            clearcoat_roughness = value;
            SetDirty();
        }
        inline void SetCustomShaderID(int id) { custom_shader_id = id; }
        inline void DisableCustomShader() { custom_shader_id = -1; }
        inline void SetDoubleSided(bool value = true) {
            if (value) {
                flags |= DOUBLE_SIDED;
            } else {
                flags &= ~DOUBLE_SIDED;
            }
        }
        inline void SetOutlineEnabled(bool value = true) {
            if (value) {
                flags |= OUTLINE;
            } else {
                flags &= ~OUTLINE;
            }
        }
        inline void SetPreferUncompressedTexturesEnabled(bool value = true) {
            if (value) {
                flags |= PREFER_UNCOMPRESSED_TEXTURES;
            } else {
                flags &= ~PREFER_UNCOMPRESSED_TEXTURES;
            }
            CreateRenderData(true);
        }
        inline void SetVertexAODisabled(bool value = true) {
            if (value) {
                flags |= DISABLE_VERTEXAO;
            } else {
                flags &= ~DISABLE_VERTEXAO;
            }
        }

        // The MaterialComponent will be written to ShaderMaterial (a struct that is optimized for GPU use)
        Moer::StaticArray<char, MaterialComponent::MaterialBytesNum> WriteShaderMaterial() const;
        void                                                         WriteShaderTextureSlot(int slot, int descriptor);

        // Retrieve the array of textures from the material

        // Returns the bitwise OR of all the wi::enums::FILTER flags applicable to this material
        uint32_t GetFilterMask() const;

        // Create texture resources for GPU
        void CreateRenderData(bool _force_recreate = false);
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
        Parameter     m_parameters[32];
        uint32_t      m_param_count{0};
        std::string   m_material_name;
        EMaterialType m_material_type{EMaterialType::E_PBR_STANDARD};
    };

    class MaterialFactory {
    public:
        RENDER_API MaterialFactory();

        template<typename... TMaterialArgs>
        RENDER_API MaterialInstanceRef CreateMaterialInstance(const EMaterialType _type, std::string_view _name, TMaterialArgs&&... _material_args) noexcept;

    private:
        Array<MaterialRef> m_materials;
    };

}// namespace Moer