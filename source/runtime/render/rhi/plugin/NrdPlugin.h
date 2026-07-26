#ifndef MOER_NRD_PLUGIN_H
#define MOER_NRD_PLUGIN_H

/**
 * 因为NVIDIA的NRD为闭源协议，所以MoerEngine无法直接引入NRD相关头文件和库文件，只能以插件的形式提供NRD支持。
 * 
 * 所以，MoerEngine在编译期引入了WITH_NRD选项，用于控制是否编译NRD相关代码。
 * 在启用NRD时，宏WITH_NRD会被定义为1，否则为0。
 * 该头文件需要保证WITH_NRD=0时，也可以正常编译。
 * 换句话说，该头文件需要与nrd命名空间内的所有类型切割，保证独立性。
 */

#include "rhi/RHI.h"

#if WITH_NRD

// NRI

// 1
#include <NRI.h>
// 2
#include <Extensions/NRIHelper.h>
// 3
#include <Extensions/NRIRayTracing.h>
// 4
#include <Extensions/NRIWrapperVK.h>
// 5
#include <NRD.h>
// 6
#include <NRDIntegration.h>

#else
// 模拟 NRI / NRD，保证 WITH_NRD=0 时无需真实头文件也能编译

namespace nri {

struct CoreInterface {};
struct HelperInterface {};
struct WrapperVKInterface {};

struct Device {};
struct CommandBuffer {};

struct Texture {
    // 占位类型，仅用于编译期
};

struct TextureBarrierDesc {
    Texture* texture = nullptr;
};

inline void nriDestroyDevice(Device&) {}

} // namespace nri

// 模拟 nrd

namespace nrd {

enum class Denoiser : uint32_t {
    REBLUR_DIFFUSE_SPECULAR,
    REBLUR_DIFFUSE,
    REBLUR_SPECULAR,
    RELAX_DIFFUSE_SPECULAR,
    RELAX_DIFFUSE,
    RELAX_SPECULAR,
    SIGMA_SHADOW_TRANSLUCENCY,
    SIGMA_SHADOW,
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

[[nodiscard]] inline constexpr bool
IsReblurDenoiser(nrd::Denoiser denoiser) {
    switch (denoiser) {
        case nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR:
        case nrd::Denoiser::REBLUR_DIFFUSE:
        case nrd::Denoiser::REBLUR_SPECULAR:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline constexpr bool
IsRelaxDenoiser(nrd::Denoiser denoiser) {
    switch (denoiser) {
        case nrd::Denoiser::RELAX_DIFFUSE_SPECULAR:
        case nrd::Denoiser::RELAX_DIFFUSE:
        case nrd::Denoiser::RELAX_SPECULAR:
            return true;
        default:
            return false;
    }
}

class NRDInterface : public std::enable_shared_from_this<NRDInterface> {
public:
    NRDInterface() = default;

    virtual ~NRDInterface();

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

    static constexpr uint8 resource_slot_count =
        uint8(EResourceSlot::SLOT_NUM);

    struct FrameDesc {
        uint32            frame_index = 0;
        Vector2ui         size{};
        Vector2f          jitter{};
        Matrix4x4f        view{};
        Matrix4x4f        projection{};
        nrd::Denoiser     denoiser = nrd::Denoiser::MAX_NUM;
        StaticArray<TextureRef, resource_slot_count> resources{};

        FrameDesc& Set(EResourceSlot slot, TextureRef texture) {
            resources[uint8(slot)] = std::move(texture);
            return *this;
        }
    };

    /**
     * Immutable renderer-to-RHI transaction payload. It freezes every native
     * resource and temporal setting before graph recording; backend commands
     * may never consult the live interface for per-frame bindings.
     */
    class PreparedFrame {
    public:
        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] uint64 GetBaseRevision() const {
            return base_revision;
        }
        [[nodiscard]] nrd::Denoiser GetDenoiser() const {
            return denoiser;
        }
        [[nodiscard]] const nrd::CommonSettings& GetCommonSettings() const {
            return common_settings;
        }
        [[nodiscard]] const TextureRef& GetResource(EResourceSlot slot) const {
            return resources[uint8(slot)];
        }
        [[nodiscard]] const StaticArray<TextureRef, resource_slot_count>&
        GetResources() const {
            return resources;
        }

    private:
        uint64            base_revision = 0;
        nrd::Denoiser     denoiser = nrd::Denoiser::MAX_NUM;
        nrd::CommonSettings common_settings{};
        StaticArray<TextureRef, resource_slot_count> resources{};
        SharedPtr<uint8> generation_token{};

        friend class NRDInterface;
    };

    using PreparedFrameRef = SharedPtr<const PreparedFrame>;

    /**
     * Pure preparation: no NRD/NRI call and no accepted-history mutation.
     */
    [[nodiscard]] RENDER_API PreparedFrameRef
    PrepareFrame(FrameDesc _desc) const;

    /**
     * Advances only renderer-side accepted history. Call once after the whole
     * frame graph and final submit have been accepted.
     */
    RENDER_API bool CommitFrame(const PreparedFrameRef& _frame);
    RENDER_API void ResetAcceptedHistory();
    [[nodiscard]] uint8 GetMaxFrameInFlight() const {
        return nrd.max_frame_in_flight;
    }

    /**
     * Records one immutable custom command. NewFrame, settings, resource
     * binding and NRD dispatch are delayed until native translation.
     */
    virtual void Denoise(
        CommandList&            _cmd_list,
        PreparedFrameRef        _frame,
        std::string_view        _name
    ) = 0;

protected:
    [[nodiscard]] bool OwnsPreparedFrame(
        const PreparedFrameRef& frame
    ) const {
        return frame &&
               frame->generation_token.get() == generation_token.get();
    }
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
    };

    NRDEntry nrd = {};

    StaticArray<Map<uint64, nri::TextureBarrierDesc>, uint8(EResourceSlot::SLOT_NUM)> texture_barrier_descs =
        {};

    UnorderedMap<uint64, nri::CommandBuffer*> cmd_lists_on_use = {};

    uint64        accepted_revision      = 0;
    bool          accepted_history_valid = false;
    nrd::Denoiser accepted_denoiser      = nrd::Denoiser::MAX_NUM;
    SharedPtr<uint8> generation_token = MakeShared<uint8>(0);
};

class NRDPlugin : public RuntimePlugin {
public:
    ~NRDPlugin()                        = default;
    static constexpr std::string_view name = "NRDPlugin";

    virtual SharedPtr<NRDInterface>
    CreateInterface(uint8 _max_frame_in_flight = 0, uint16 _frame_width = 0, uint16 _frame_height = 0) = 0;

    virtual SharedPtr<NRDInterface> RecreateInterface(
        SharedPtr<NRDInterface> _interface,
        uint16                  _frame_width  = 0,
        uint16                  _frame_height = 0
    ) = 0;
};
} // namespace Moer::Render::Ext

#endif
