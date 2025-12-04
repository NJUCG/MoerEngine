#ifndef MOER_NRD_EXTENSION_H
#define MOER_NRD_EXTENSION_H

/**
 * 因为NVIDIA的NRD为闭源协议，所以MoerEngine无法直接引入NRD相关头文件和库文件，只能以插件的形式提供NRD支持。
 * 
 * 所以，MoerEngine在编译期引入了WITH_NRD选项，用于控制是否编译NRD相关代码。
 * 在启用NRD时，宏WITH_NRD会被定义为1，否则为0。
 * 该头文件需要保证WITH_NRD=0时，也可以正常编译。
 * 换句话说，该头文件需要与nrd命名空间内的所有类型切割，保证独立性。
 */

#include "rhi/RHI.h"

// NRD and NRI-based integration
// 1
#include <NRI.h>
// 2
#include <Extensions/NRIHelper.h>
// 3
#include <Extensions/NRIWrapperVK.h>

#if WITH_NRD

// 4
#include <NRD.h>
// 5
#include <NRDIntegration.h>

#else
// 模拟nrd

namespace nrd {

enum class Denoiser : uint32_t {
    REBLUR_DIFFUSE_SPECULAR,
    RELAX_DIFFUSE_SPECULAR,
    MAX_NUM,
};

struct CommonSettings {};

struct ReblurSettings {};

struct RelaxSettings {};

struct Integration {};

struct UserPool {};

typedef uint32_t Identifier;

} // namespace nrd

#endif

namespace Moer::Render::Ext {

class NRDInterface {
public:
    NRDInterface() = default;

    virtual ~NRDInterface();

    virtual void Begin() = 0;

    virtual void Denoise(CommandList& _cmd_list, const nrd::Denoiser _denoiser, std::string_view _name) = 0;

    virtual void Reinitialize(uint16 _frame_width, uint16 _frame_height) = 0;

public:
    enum struct EResourceSlot : uint8 {
        // Reblur/Relax
        MOTION_VECTOR,
        NORMAL_ROUGHNESS,
        VIEW_Z,
        BASECOLOR_METALNESS,
        IN_DIFFUSE,
        IN_SPECULAR,

        // Sigma
        IN_PENUMBRA,
        IN_TRANSLUCENCY,

        // Reblur/Relax
        OUT_DIFFUSE,
        OUT_SPECULAR,

        // Sigma
        OUT_SHADOW_TRANSLUCENCY,

        SLOT_NUM
    };

public:
    virtual void SetInput(EResourceSlot _index, TextureRef _texture)  = 0;
    virtual void SetOutput(EResourceSlot _index, TextureRef _texture) = 0;

    RENDER_API void UpdateCommonSettings(
        uint32            _frame_index,
        const Vector2ui&  _size,
        const Vector2f&   _jitter,
        const Matrix4x4f& _view,
        const Matrix4x4f& _proj
    );
    RENDER_API void SetCommonSettings(const nrd::CommonSettings& _settings);
    RENDER_API void SetDenoiserSettings(const nrd::Denoiser _type, const nrd::ReblurSettings& _settings);
    RENDER_API void SetDenoiserSettings(const nrd::Denoiser _type, const nrd::RelaxSettings& _settings);

protected:
    void SetDefaultDenoiserSettings(const nrd::Identifier _denoiser_id);
    void SetDefaultCommonSettings(uint16 _frame_width, uint16 _frame_height);

protected:
    nrd::CommonSettings nrd_common_settings = {};

    struct NRDEntry {
        struct NRIEntry {
            struct NRIInterface : public nri::CoreInterface,
                                  public nri::HelperInterface,
                                  public nri::WrapperVKInterface {};
            NRIInterface        rhi      = {};
            nri::Device*        device   = nullptr;
            nri::CommandBuffer* cmd_list = nullptr;
        };

        uint16 frame_width         = 0;
        uint16 frame_height        = 0;
        uint8  max_frame_in_flight = 0;

        NRIEntry nri = {};

        nrd::Integration integration = {};

        nrd::UserPool user_pool = {};
    };

    NRDEntry nrd = {};

    StaticArray<Map<uint64, nri::TextureBarrierDesc>, uint8(EResourceSlot::SLOT_NUM)> texture_barrier_descs =
        {};

    UnorderedMap<uint64, nri::CommandBuffer*> cmd_lists_on_use = {};
};

class NRDExtension : public DeviceExtension {
public:
    ~NRDExtension()                        = default;
    static constexpr std::string_view name = "NRDExt";

    virtual UniquePtr<NRDInterface>
    CreateInterface(uint8 _max_frame_in_flight = 0, uint16 _frame_width = 0, uint16 _frame_height = 0) = 0;

    virtual UniquePtr<NRDInterface> RecreateInterface(
        UniquePtr<NRDInterface> _interface,
        uint16                  _frame_width  = 0,
        uint16                  _frame_height = 0
    ) = 0;
};
} // namespace Moer::Render::Ext

#endif