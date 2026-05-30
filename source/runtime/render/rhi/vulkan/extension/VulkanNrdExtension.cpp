#include "VulkanNrdExtension.h"

#if WITH_NRD

#include "../../vulkan/VulkanCustomCommand.h"
#include "../../vulkan/VulkanDescriptor.h"
#include "../../vulkan/VulkanDevice.h"
#include "../../vulkan/VulkanRHIResource.h"
#include <array>
#include <cstdio>
#include <memory>
#include "vulkan/vulkan_core.h"

namespace Moer::Render::Ext {

namespace {

void FreezeResourceSnapshot(const nrd::ResourceSnapshot& source, nrd::ResourceSnapshot& target) {
    target.restoreInitialState = source.restoreInitialState;
    for (size_t slot_index = 0; slot_index < source.slots.size(); ++slot_index) {
        const nrd::Resource* resource = source.slots[slot_index];
        if (resource == nullptr || resource->nri.texture == nullptr) {
            continue;
        }

        target.SetResource(static_cast<nrd::ResourceType>(slot_index), *resource);
    }
}

} // namespace

class VkNRDInterface final : public NRDInterface {
    friend class VkNrdDenoiseCmd;

    struct DispatchSyncState {
        RHITranslateFence inflight_translate_fence{};
        bool              begin_frame_pending{true};
    };

private:
    void InitializeNri(VulkanDevice* _device) {
        auto& nri_entry = nrd.nri;

        nri::DeviceCreationVKDesc device_desc{};

        device_desc.vkInstance       = _device->GetInstance();
        device_desc.vkDevice         = _device->GetDevice();
        device_desc.vkPhysicalDevice = _device->GetGpu();

        const std::array<nri::QueueFamilyVKDesc, 3> queue_families = {{
            {1, nri::QueueType::GRAPHICS, _device->GetQueueFamilyIndices().graphics.value()},
            {1, nri::QueueType::COMPUTE, _device->GetQueueFamilyIndices().compute.value()},
            {1, nri::QueueType::COPY, _device->GetQueueFamilyIndices().transfer.value()},
        }};

        device_desc.queueFamilies      = queue_families.data();
        device_desc.queueFamilyNum     = uint32(queue_families.size());
        device_desc.minorVersion       = VK_VERSION_MINOR(_device->GetCoreProperties().core_1_0.apiVersion);
        device_desc.enableNRIValidation = false;

        CHECK_ASSERT(
            nri::nriCreateDeviceFromVKDevice(device_desc, nri_entry.device) == nri::Result::SUCCESS,
            "Failed to create NRI device"
        );

        CHECK_ASSERT(
            nri::nriGetInterface(
                *nri_entry.device, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&nri_entry.rhi
            ) == nri::Result::SUCCESS,
            "Failed to get NRI Core Interface"
        );

        CHECK_ASSERT(
            nri::nriGetInterface(
                *nri_entry.device,
                NRI_INTERFACE(nri::HelperInterface),
                (nri::HelperInterface*)&nri_entry.rhi
            ) == nri::Result::SUCCESS,
            "Failed to get NRI Helper Interface"
        );

        CHECK_ASSERT(
            nri::nriGetInterface(
                *nri_entry.device,
                NRI_INTERFACE(nri::WrapperVKInterface),
                (nri::WrapperVKInterface*)&nri_entry.rhi
            ) == nri::Result::SUCCESS,
            "Failed to get NRI WrapperVK Interface"
        );

        LOG_INFO(MOER_TEXT("[NRD]: NRI device created"));
    }

    void ApplyDefaultSettings(uint16 _frame_width, uint16 _frame_height) {
#define NRD_ID(x) nrd::Identifier(nrd::Denoiser::x)
        SetDefaultCommonSettings(_frame_width, _frame_height);
        SetDefaultDenoiserSettings(NRD_ID(REBLUR_DIFFUSE_SPECULAR));
        SetDefaultDenoiserSettings(NRD_ID(REBLUR_DIFFUSE));
        SetDefaultDenoiserSettings(NRD_ID(REBLUR_SPECULAR));
        SetDefaultDenoiserSettings(NRD_ID(RELAX_DIFFUSE_SPECULAR));
        SetDefaultDenoiserSettings(NRD_ID(RELAX_DIFFUSE));
        SetDefaultDenoiserSettings(NRD_ID(RELAX_SPECULAR));
        SetDefaultDenoiserSettings(NRD_ID(SIGMA_SHADOW_TRANSLUCENCY));
        SetDefaultDenoiserSettings(NRD_ID(SIGMA_SHADOW));
#undef NRD_ID
    }

    void InitializeIntegration(uint8 _max_frame_in_flight, uint16 _frame_width, uint16 _frame_height) {
        auto& nri_entry = nrd.nri;
        const uint8 buffered_frames = _max_frame_in_flight < 8 ? 8 : _max_frame_in_flight;

        nrd.frame_width         = _frame_width;
        nrd.frame_height        = _frame_height;
        nrd.max_frame_in_flight = buffered_frames;

        nrd::IntegrationCreationDesc integration_desc = {};
        integration_desc.resourceWidth                = _frame_width;
        integration_desc.resourceHeight               = _frame_height;
        integration_desc.queuedFrameNum               = buffered_frames;
        std::snprintf(
            integration_desc.name,
            sizeof(integration_desc.name),
            "%s",
            "NRD Integration for MoerEngine VkBackend"
        );

#define NRD_ID(x) nrd::Identifier(nrd::Denoiser::x)
        const Array<nrd::DenoiserDesc> denoiser_descs = {
            {NRD_ID(REBLUR_DIFFUSE_SPECULAR), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
            {NRD_ID(REBLUR_DIFFUSE), nrd::Denoiser::REBLUR_DIFFUSE},
            {NRD_ID(REBLUR_SPECULAR), nrd::Denoiser::REBLUR_SPECULAR},
            {NRD_ID(RELAX_DIFFUSE_SPECULAR), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR},
            {NRD_ID(RELAX_DIFFUSE), nrd::Denoiser::RELAX_DIFFUSE},
            {NRD_ID(RELAX_SPECULAR), nrd::Denoiser::RELAX_SPECULAR},
            {NRD_ID(SIGMA_SHADOW_TRANSLUCENCY), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
            {NRD_ID(SIGMA_SHADOW), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
        };

        nrd::InstanceCreationDesc instance_desc = {};
        instance_desc.denoisers                 = denoiser_descs.data();
        instance_desc.denoisersNum              = denoiser_descs.size();

        CHECK_ASSERT(
            nrd.integration.Recreate(integration_desc, instance_desc, nri_entry.device)
                == nrd::Result::SUCCESS,
            "Failed to initialize NRD Integration"
        );

        LOG_INFO(MOER_TEXT("[NRD]: NRD Integration created."));

        ApplyDefaultSettings(_frame_width, _frame_height);
#undef NRD_ID
    }

    void ResetResourceSnapshot() {
        std::destroy_at(&nrd.resource_snapshot);
        std::construct_at(&nrd.resource_snapshot);
    }

    void ResetIntegrationState() {
        for (auto& desc_map : texture_barrier_descs) {
            for (auto& [_, desc] : desc_map) {
                if (desc.nri.texture != nullptr) {
                    nrd.nri.rhi.DestroyTexture(desc.nri.texture);
                }
            }

            desc_map.clear();
        }

        for (auto& [_, cmd_list] : cmd_lists_on_use) {
            if (cmd_list != nullptr) {
                nrd.nri.rhi.DestroyCommandBuffer(cmd_list);
            }
        }

        cmd_lists_on_use.clear();
        resource_usages.clear();
        resource_usages_indices = {};
        ResetResourceSnapshot();
        nrd.integration.Destroy();
    }

public:
    VkNRDInterface(
        VulkanDevice* _device,
        uint8         _max_frame_in_flight,
        uint16        _frame_width,
        uint16        _frame_height
    ) {
        InitializeNri(_device);
        InitializeIntegration(_max_frame_in_flight, _frame_width, _frame_height);
    }

    ~VkNRDInterface() override {
        MOER_ASSERT(!HasPendingTranslateWork(), "NRDInterface destroyed while translate work is still pending");
    }

    bool HasPendingTranslateWork() const override {
        return sync_state.inflight_translate_fence.event &&
               !sync_state.inflight_translate_fence.event->IsComplete();
    }

    void Begin() override {
        sync_state.begin_frame_pending = true;
        ResetResourceSnapshot();
    }

    void Denoise(CommandList& _cmd_list, const nrd::Denoiser _denoiser, StringView _name) override;

    void Reinitialize(uint16 _frame_width, uint16 _frame_height) override {
        ResetIntegrationState();
        InitializeIntegration(nrd.max_frame_in_flight, _frame_width, _frame_height);

        LOG_INFO(MOER_TEXT("[NRD]: NRD integration recreated."));
    }

    void SetInput(EResourceSlot _index, TextureRef _texture) override {
        assert(_index < EResourceSlot::OUT_DIFFUSE && "Invalid resource slot index");
        auto* vk_tex = ResourceCast(_texture);

        auto& tex_barrier_desc_map = texture_barrier_descs[uint8(_index)];

        auto desc_key = uint64(vk_tex->GetHandle());
        if (tex_barrier_desc_map.contains(desc_key)) {
            auto& tex_resource = tex_barrier_desc_map[desc_key];
            if (_index == EResourceSlot::MOTION_VECTOR) {
                tex_resource.state.access = nri::AccessBits::SHADER_RESOURCE;
                tex_resource.state.layout = nri::Layout::SHADER_RESOURCE;
                tex_resource.state.stages = nri::StageBits::COMPUTE_SHADER;
            }
            nrd.resource_snapshot.SetResource(ResourceSlot(_index), tex_resource);
        } else {
            auto& tex_resource = tex_barrier_desc_map[desc_key];
            // Wrap required textures (better do it only once on initialization)
            nri::TextureVKDesc tex_desc = {};

            tex_desc.vkImage  = uint64(vk_tex->GetHandle());
            tex_desc.vkFormat = Moer::Render::VulkanEnumTranslator::METoVKFormat(vk_tex->GetFormat());
            tex_desc.vkImageType =
                Moer::Render::VulkanEnumTranslator::METoVKImageType(vk_tex->GetDimension());
            tex_desc.width     = vk_tex->GetWidth();
            tex_desc.height    = vk_tex->GetHeight();
            tex_desc.depth     = vk_tex->GetDepth();
            tex_desc.mipNum    = vk_tex->GetNumMips();
            tex_desc.layerNum  = vk_tex->GetNumArray();
            tex_desc.sampleNum = 1; // 1x texture sampling

            // You need to specify the current state of the resource here, after denoising NRD can modify
            // this state. Application must continue state tracking from this point.
            // Useful information:
            //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
            //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL
            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, tex_resource.nri.texture);
            tex_resource.state.access = nri::AccessBits::SHADER_RESOURCE;
            tex_resource.state.layout = nri::Layout::SHADER_RESOURCE;
            tex_resource.state.stages = nri::StageBits::COMPUTE_SHADER;
            tex_resource.userArg      = &tex_resource;
            nrd.resource_snapshot.SetResource(ResourceSlot(_index), tex_resource);
        }

        // Resource usage
        if (resource_usages_indices[uint8(_index)] == 0) {
            VulkanShaderResourceState pipeline_flags{};
            pipeline_flags.desc_type     = VDT_SAMPLED_IMAGE;
            pipeline_flags.b_sampled     = 1;
            pipeline_flags.resource_type = SRT_SRV;

            ParamInfoFlags read_flag = {pipeline_flags(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

            resource_usages.emplace_back(_texture, read_flag);
            resource_usages_indices[uint8(_index)] = resource_usages.size();
        } else {
            // Update resource usage
            auto& usage    = resource_usages[resource_usages_indices[uint8(_index)] - 1];
            usage.resource = _texture;
        }
    }

    void SetOutput(EResourceSlot _index, TextureRef _texture) override {
        assert(_index >= EResourceSlot::OUT_DIFFUSE && "Output resource index is invalid");
        auto* vk_tex = ResourceCast(_texture);

        auto& tex_barrier_desc_map = texture_barrier_descs[uint8(_index)];

        auto desc_key = uint64(vk_tex->GetHandle());
        if (tex_barrier_desc_map.contains(desc_key)) {
            auto& tex_resource = tex_barrier_desc_map[desc_key];
            nrd.resource_snapshot.SetResource(ResourceSlot(_index), tex_resource);
        } else {
            auto& tex_resource = tex_barrier_desc_map[desc_key];
            // Wrap required textures (better do it only once on initialization)
            nri::TextureVKDesc tex_desc = {};

            tex_desc.vkImage  = uint64(vk_tex->GetHandle());
            tex_desc.vkFormat = Moer::Render::VulkanEnumTranslator::METoVKFormat(vk_tex->GetFormat());
            tex_desc.vkImageType =
                Moer::Render::VulkanEnumTranslator::METoVKImageType(vk_tex->GetDimension());
            tex_desc.width     = vk_tex->GetWidth();
            tex_desc.height    = vk_tex->GetHeight();
            tex_desc.depth     = vk_tex->GetDepth();
            tex_desc.mipNum    = vk_tex->GetNumMips();
            tex_desc.layerNum  = vk_tex->GetNumArray();
            tex_desc.sampleNum = 1; // 1x texture sampling

            // You need to specify the current state of the resource here, after denoising NRD can modify
            // this state. Application must continue state tracking from this point.
            // Useful information:
            //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
            //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL
            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, tex_resource.nri.texture);
            tex_resource.state.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            tex_resource.state.layout = nri::Layout::SHADER_RESOURCE_STORAGE;
            tex_resource.state.stages = nri::StageBits::COMPUTE_SHADER;
            tex_resource.userArg      = &tex_resource;
            nrd.resource_snapshot.SetResource(ResourceSlot(_index), tex_resource);
        }

        // Resource usage
        if (resource_usages_indices[uint8(_index)] == 0) {
            VulkanShaderResourceState pipeline_flags{};
            pipeline_flags.desc_type     = VDT_STORAGE_IMAGE;
            pipeline_flags.b_sampled     = 1;
            pipeline_flags.resource_type = SRT_UAV;

            ParamInfoFlags write_flag = {pipeline_flags(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

            resource_usages.emplace_back(_texture, write_flag);
            resource_usages_indices[uint8(_index)] = resource_usages.size();
        } else {
            // Update resource usage
            auto& usage    = resource_usages[resource_usages_indices[uint8(_index)] - 1];
            usage.resource = _texture;
        }
    };

private:
    Array<CustomDispatchCmd::ResourceUsage>            resource_usages;
    StaticArray<uint8, uint8(EResourceSlot::SLOT_NUM)> resource_usages_indices = {};
    DispatchSyncState                                   sync_state{};

    constexpr nrd::ResourceType ResourceSlot(const EResourceSlot _index) {
        switch (_index) {
            case EResourceSlot::MOTION_VECTOR:
                return nrd::ResourceType::IN_MV;
            case EResourceSlot::NORMAL_ROUGHNESS:
                return nrd::ResourceType::IN_NORMAL_ROUGHNESS;
            case EResourceSlot::VIEW_Z:
                return nrd::ResourceType::IN_VIEWZ;
            case EResourceSlot::IN_DIFFUSE:
                return nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST;
            case EResourceSlot::IN_SPECULAR:
                return nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST;
            case EResourceSlot::IN_PENUMBRA:
                return nrd::ResourceType::IN_PENUMBRA;
            case EResourceSlot::IN_TRANSLUCENCY:
                return nrd::ResourceType::IN_TRANSLUCENCY;
            case EResourceSlot::OUT_DIFFUSE:
                return nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST;
            case EResourceSlot::OUT_SPECULAR:
                return nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST;
            case EResourceSlot::OUT_SHADOW_TRANSLUCENCY:
                return nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY;
            default:
                return nrd::ResourceType::MAX_NUM;
        }
    }
};

UniquePtr<NRDInterface>
VkNRDExtension::CreateInterface(uint8 _max_frame_in_flight, uint16 _frame_width, uint16 _frame_height) {
    CollectRetiredInterfaces();

    return MakeUnique<VkNRDInterface>(m_device, _max_frame_in_flight, _frame_width, _frame_height);
}

void VkNRDExtension::CollectRetiredInterfaces() {
    auto iter = retired_interfaces.begin();
    while (iter != retired_interfaces.end()) {
        if (!(*iter) || !(*iter)->HasPendingTranslateWork()) {
            iter = retired_interfaces.erase(iter);
            continue;
        }
        ++iter;
    }
}

UniquePtr<NRDInterface> VkNRDExtension::RecreateInterface(
    UniquePtr<NRDInterface> _interface,
    uint16                  _frame_width,
    uint16                  _frame_height
) {
    CollectRetiredInterfaces();
    const uint8 max_frame_in_flight = _interface ? _interface->MaxFrameInFlight() : 0;
    if (_interface) {
        if (_interface->HasPendingTranslateWork()) {
            retired_interfaces.emplace_back(std::move(_interface));
        } else {
            _interface.reset();
        }
    }

    return MakeUnique<VkNRDInterface>(m_device, max_frame_in_flight, _frame_width, _frame_height);
}

class VkNrdDenoiseCmd final : public VkCustomDispatchCmd {
private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return resource_usages;
    }

private:
    VkNRDInterface& nrd_interface;
    Array<ResourceUsage>      resource_usages{};
    mutable nrd::ResourceSnapshot resource_snapshot{};
    nrd::CommonSettings       common_settings{};
    std::optional<nrd::ReblurSettings> reblur_settings{};
    std::optional<nrd::RelaxSettings>  relax_settings{};
    bool                      begin_frame{false};
    uint8                     motion_vector_usage_index{0};
    nrd::Denoiser             denoiser;

public:
    VkNrdDenoiseCmd(
        VkNRDInterface&        _nrd,
        const nrd::Denoiser    _denoiser,
        Array<ResourceUsage>&& _resource_usages,
        const nrd::ResourceSnapshot& _resource_snapshot,
        const nrd::CommonSettings& _common_settings,
        std::optional<nrd::ReblurSettings> _reblur_settings,
        std::optional<nrd::RelaxSettings>  _relax_settings,
        bool                     _begin_frame,
        uint8                  _motion_vector_usage_index
    ) :
        nrd_interface(_nrd),
        resource_usages(std::move(_resource_usages)),
        common_settings(_common_settings),
        reblur_settings(std::move(_reblur_settings)),
        relax_settings(std::move(_relax_settings)),
        begin_frame(_begin_frame),
        motion_vector_usage_index(_motion_vector_usage_index),
        denoiser(_denoiser) {
        FreezeResourceSnapshot(_resource_snapshot, resource_snapshot);
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    void Execute(const VkDispatchContext& _context) const override {
        auto& nrd_integration = nrd_interface.nrd.integration;
        auto& nri             = nrd_interface.nrd.nri;
        if (begin_frame) {
            nrd_integration.NewFrame();
            nrd_integration.SetCommonSettings(common_settings);
        }
        if (reblur_settings.has_value()) {
            nrd_integration.SetDenoiserSettings(nrd::Identifier(denoiser), &reblur_settings.value());
        } else if (relax_settings.has_value()) {
            nrd_integration.SetDenoiserSettings(nrd::Identifier(denoiser), &relax_settings.value());
        }
        //=======================================================================================================
        // RENDER - DENOISE
        //=======================================================================================================
        // Wrap a command buffer
        if (nrd_interface.cmd_lists_on_use.contains(uint64(_context.cmd_list))) {
            nri.cmd_list = nrd_interface.cmd_lists_on_use[uint64(_context.cmd_list)];
        } else {
            nri::CommandBufferVKDesc cmd_buffer_desc = {};
            cmd_buffer_desc.queueType                = nri::QueueType::GRAPHICS;
            cmd_buffer_desc.vkCommandBuffer          = _context.cmd_list;
            nri.rhi.CreateCommandBufferVK(*nri.device, cmd_buffer_desc, nri.cmd_list);
            nrd_interface.cmd_lists_on_use[uint64(_context.cmd_list)] = nri.cmd_list;
        }

        const nrd::Identifier denoiser_id = nrd::Identifier(denoiser);
        auto& resource_snapshot = this->resource_snapshot;
        nrd_integration.Denoise(&denoiser_id, 1, *nri.cmd_list, resource_snapshot);

        for (size_t resource_index = 0; resource_index < resource_snapshot.uniqueNum; ++resource_index) {
            auto& resource = resource_snapshot.unique[resource_index];
            if (resource.userArg != nullptr) {
                auto* cached_resource = static_cast<nrd::Resource*>(resource.userArg);
                cached_resource->state = resource.state;
            }
        }

        // The motion vector has been trasitioned to GENERAL layout by NRD REBLUR Denoisers, so we need to flush the state
        if (denoiser < nrd::Denoiser::RELAX_DIFFUSE && motion_vector_usage_index != 0) {
            auto* mv = ResourceCast(
                std::get<TextureView>(resource_usages[motion_vector_usage_index - 1].resource).GetTexture()
            );
            auto* vk_tracker = (VkTracker*)_context.user_data;
            vk_tracker->FlushSrcState(
                mv,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            );
        }

        // IMPORTANT: NRD integration binds own descriptor pool, don't forget to re-bind back your pool (heap)
    }
};

void VkNRDInterface::Denoise(CommandList& _cmd_list, const nrd::Denoiser _denoiser, StringView _name) {
    if (sync_state.inflight_translate_fence.event && sync_state.inflight_translate_fence.event->IsComplete()) {
        sync_state.inflight_translate_fence = {};
    }

    if (sync_state.inflight_translate_fence.event && !sync_state.inflight_translate_fence.event->IsComplete()) {
        GraphEventRef record_dependency = GraphEvent::CreateGraphEvent();
        if (const GraphEventRef& existing_record_event = _cmd_list.GetRecordCompleteEvent(); existing_record_event) {
            record_dependency->WaitUntil(existing_record_event);
        }
        record_dependency->WaitUntil(sync_state.inflight_translate_fence.event);
        record_dependency->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
        _cmd_list.SetRecordCompleteEvent(std::move(record_dependency));
    }

    const bool begin_frame = sync_state.begin_frame_pending;
    sync_state.begin_frame_pending = false;
    sync_state.inflight_translate_fence = RHITranslateFence::Create();
    RHITranslateFence translate_fence = sync_state.inflight_translate_fence;

    _cmd_list.AddCustomCommand(
        MakeUnique<VkNrdDenoiseCmd>(
            *this,
            _denoiser,
            Array<CustomDispatchCmd::ResourceUsage>(resource_usages.begin(), resource_usages.end()),
            nrd.resource_snapshot,
            PendingCommonSettings(),
            PendingReblurSettings(_denoiser),
            PendingRelaxSettings(_denoiser),
            begin_frame,
            resource_usages_indices[uint8(EResourceSlot::MOTION_VECTOR)]
        ),
        _name
    );
    _cmd_list.TranslateFence(std::move(translate_fence));
}

} // namespace Moer::Render::Ext

#endif
