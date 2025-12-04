#include "VulkanNrdExtension.h"

#if WITH_NRD

#include "../../vulkan/VulkanCustomCommand.h"
#include "../../vulkan/VulkanDescriptor.h"
#include "../../vulkan/VulkanDevice.h"
#include "../../vulkan/VulkanRHIResource.h"
#include "vulkan/vulkan_core.h"

namespace Moer::Render::Ext {

class VkNRDInterface final : public NRDInterface {
    friend class VkNrdDenoiseCmd;

public:
    VkNRDInterface(
        VulkanDevice* _device,
        uint8         _max_frame_in_flight,
        uint16        _frame_width,
        uint16        _frame_height
    ) {
        //=======================================================================================================
        // INITIALIZATION - INITIALIZE NRI, WRAP NATIVE DEVICE
        //=======================================================================================================
        auto& nri_entry = nrd.nri;

        {
            nri::DeviceCreationVKDesc device_desc{};

            device_desc.vkInstance       = _device->GetInstance();
            device_desc.vkDevice         = _device->GetDevice();
            device_desc.vkPhysicalDevice = _device->GetGpu();

            const uint32 queue_family_indices[] = {
                _device->GetQueueFamilyIndices().graphics.value(),
                _device->GetQueueFamilyIndices().present.value(),
                _device->GetQueueFamilyIndices().compute.value(),
                _device->GetQueueFamilyIndices().transfer.value(),
            };

            device_desc.queueFamilyIndices  = queue_family_indices;
            device_desc.queueFamilyIndexNum = _device->GetQueueFamilyIndices().Size();
            device_desc.minorVersion = VK_VERSION_MINOR(_device->GetCoreProperties().core_1_0.apiVersion);
            device_desc.enableNRIValidation = false;

            CHECK_ASSERT(
                nri::nriCreateDeviceFromVkDevice(device_desc, nri_entry.device) == nri::Result::SUCCESS,
                "Failed to create NRI device"
            );

            // Get core functionality
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

            // Get appropriate "wrapper" extension (XXX - can be D3D11, D3D12 or VULKAN)
            CHECK_ASSERT(
                nri::nriGetInterface(
                    *nri_entry.device,
                    NRI_INTERFACE(nri::WrapperVKInterface),
                    (nri::WrapperVKInterface*)&nri_entry.rhi
                ) == nri::Result::SUCCESS,
                "Failed to get NRI WrapperVK Interface"
            );

            LOG_INFO("[NRD]: NRI device created");
        }

        //=======================================================================================================
        // INITIALIZATION - INITIALIZE NRD && RESOURCES && SETTINGS
        //=======================================================================================================
        {
            nrd.frame_width         = _frame_width;
            nrd.frame_height        = _frame_height;
            nrd.max_frame_in_flight = _max_frame_in_flight;

            nrd::IntegrationCreationDesc integration_desc = {};
            integration_desc.name                         = "NRD Integration for MoerEngine VkBackend";
            integration_desc.resourceWidth                = _frame_width;
            integration_desc.resourceHeight               = _frame_height;
            integration_desc.bufferedFramesNum            = _max_frame_in_flight;

#define NRD_ID(x) nrd::Identifier(nrd::Denoiser::x)
            // Denoisers
            // NRD sample doesn't use several instances of the same denoiser in one NRD instance (like REBLUR_DIFFUSE x 3),
            // thus we can use fields of "nrd::Denoiser" enum as unique identifiers
            const Array<nrd::DenoiserDesc> denoiser_descs = {
                // REBLUR
                // {NRD_ID(REBLUR_DIFFUSE_SPECULAR_SH), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR_SH},
                // {NRD_ID(REBLUR_DIFFUSE_SH), nrd::Denoiser::REBLUR_DIFFUSE_SH},
                // {NRD_ID(REBLUR_SPECULAR_SH), nrd::Denoiser::REBLUR_SPECULAR_SH},
                // {NRD_ID(REBLUR_DIFFUSE_SPECULAR_OCCLUSION), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR_OCCLUSION},
                // {NRD_ID(REBLUR_DIFFUSE_OCCLUSION), nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION},
                // {NRD_ID(REBLUR_SPECULAR_OCCLUSION), nrd::Denoiser::REBLUR_SPECULAR_OCCLUSION},
                // {NRD_ID(REBLUR_DIFFUSE_DIRECTIONAL_OCCLUSION), nrd::Denoiser::REBLUR_DIFFUSE_DIRECTIONAL_OCCLUSION},
                {NRD_ID(REBLUR_DIFFUSE_SPECULAR), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
                {NRD_ID(REBLUR_DIFFUSE), nrd::Denoiser::REBLUR_DIFFUSE},
                {NRD_ID(REBLUR_SPECULAR), nrd::Denoiser::REBLUR_SPECULAR},
                // RELAX
                // {NRD_ID(RELAX_DIFFUSE_SPECULAR_SH), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR_SH},
                // {NRD_ID(RELAX_DIFFUSE_SH), nrd::Denoiser::RELAX_DIFFUSE_SH},
                // {NRD_ID(RELAX_SPECULAR_SH), nrd::Denoiser::RELAX_SPECULAR_SH},
                {NRD_ID(RELAX_DIFFUSE_SPECULAR), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR},
                {NRD_ID(RELAX_DIFFUSE), nrd::Denoiser::RELAX_DIFFUSE},
                {NRD_ID(RELAX_SPECULAR), nrd::Denoiser::RELAX_SPECULAR},
                // SIGMA
                {NRD_ID(SIGMA_SHADOW_TRANSLUCENCY), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
                {NRD_ID(SIGMA_SHADOW), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
                // REFERENCE
                // {NRD_ID(REFERENCE), nrd::Denoiser::REFERENCE},
            };

            nrd::InstanceCreationDesc instance_desc = {};
            instance_desc.denoisers                 = denoiser_descs.data();
            instance_desc.denoisersNum              = denoiser_descs.size();

            // NRD itself is flexible and supports any kind of dynamic resolution scaling, but NRD INTEGRATION pre-
            // allocates resources with statically defined dimensions. DRS is only supported by adjusting the viewport
            // via "CommonSettings::rectSize"
            CHECK_ASSERT(
                nrd.integration.Initialize(
                    integration_desc, instance_desc, *nri_entry.device, nri_entry.rhi, nri_entry.rhi
                ),
                "Failed to initialize NRD Integration"
            );

            LOG_INFO("[NRD]: NRD Integration created.");
        }

        {
            SetDefaultCommonSettings(_frame_width, _frame_height);
            SetDefaultDenoiserSettings(NRD_ID(REBLUR_DIFFUSE_SPECULAR));
            SetDefaultDenoiserSettings(NRD_ID(REBLUR_DIFFUSE));
            SetDefaultDenoiserSettings(NRD_ID(REBLUR_SPECULAR));
            SetDefaultDenoiserSettings(NRD_ID(RELAX_DIFFUSE_SPECULAR));
            SetDefaultDenoiserSettings(NRD_ID(RELAX_DIFFUSE));
            SetDefaultDenoiserSettings(NRD_ID(RELAX_SPECULAR));
            SetDefaultDenoiserSettings(NRD_ID(SIGMA_SHADOW_TRANSLUCENCY));
            SetDefaultDenoiserSettings(NRD_ID(SIGMA_SHADOW));
        }
#undef NRD_ID
    }

    void Begin() override {
        // Must be called once on a frame start
        nrd.integration.NewFrame();
    }

    void Denoise(CommandList& _cmd_list, const nrd::Denoiser _denoiser, std::string_view _name) override;

    void Reinitialize(uint16 _frame_width, uint16 _frame_height) override {
        nrd.frame_width  = _frame_width;
        nrd.frame_height = _frame_height;
        nrd.integration.RecreateResources(_frame_width, _frame_height);

        SetDefaultCommonSettings(_frame_width, _frame_height);

        LOG_INFO("[NRD]: NRD Texture Resources recreated.");
    }

    void SetInput(EResourceSlot _index, TextureRef _texture) override {
        assert(_index < EResourceSlot::OUT_DIFFUSE && "Invalid resource slot index");
        auto* vk_tex = ResourceCast(_texture);

        auto& tex_barrier_desc_map = texture_barrier_descs[uint8(_index)];

        auto desc_key = uint64(vk_tex->GetHandle());
        if (tex_barrier_desc_map.contains(desc_key)) {
            auto& tex_barrier_desc = tex_barrier_desc_map[desc_key];
            if (_index == EResourceSlot::MOTION_VECTOR) {
                // The 'nri::TextureBarrierDesc.after' of motion vector has been trasitioned to GENERAL by NRD REBLUR Denoisers, so we need to reset it
                tex_barrier_desc.after.access = nri::AccessBits::SHADER_RESOURCE;
                tex_barrier_desc.after.layout = nri::Layout::SHADER_RESOURCE;
            }
            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(_index), &tex_barrier_desc);
        } else {
            auto& tex_barrier_desc = tex_barrier_desc_map[desc_key];
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
            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, (nri::Texture*&)tex_barrier_desc.texture);
            tex_barrier_desc.before.access = nri::AccessBits::SHADER_RESOURCE;
            tex_barrier_desc.before.layout = nri::Layout::SHADER_RESOURCE;
            tex_barrier_desc.before.stages = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.after.access  = nri::AccessBits::SHADER_RESOURCE;
            tex_barrier_desc.after.layout  = nri::Layout::SHADER_RESOURCE;
            tex_barrier_desc.after.stages  = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.mipOffset     = 0;
            tex_barrier_desc.mipNum        = vk_tex->GetNumMips();
            tex_barrier_desc.layerOffset   = 0;
            tex_barrier_desc.layerNum      = vk_tex->GetNumArray();
            tex_barrier_desc.planes        = nri::PlaneBits::COLOR;
            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(_index), &tex_barrier_desc);
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
            auto& tex_barrier_desc = tex_barrier_desc_map[desc_key];
            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(_index), &tex_barrier_desc);
        } else {
            auto& tex_barrier_desc = tex_barrier_desc_map[desc_key];
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
            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, (nri::Texture*&)tex_barrier_desc.texture);
            tex_barrier_desc.before.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.before.layout = nri::Layout::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.before.stages = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.after.access  = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.after.layout  = nri::Layout::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.after.stages  = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.mipOffset     = 0;
            tex_barrier_desc.mipNum        = vk_tex->GetNumMips();
            tex_barrier_desc.layerOffset   = 0;
            tex_barrier_desc.layerNum      = vk_tex->GetNumArray();
            tex_barrier_desc.planes        = nri::PlaneBits::COLOR;
            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(_index), &tex_barrier_desc);
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

    constexpr nrd::ResourceType ResourceSlot(const EResourceSlot _index) {
        switch (_index) {
            case EResourceSlot::MOTION_VECTOR:
                return nrd::ResourceType::IN_MV;
            case EResourceSlot::NORMAL_ROUGHNESS:
                return nrd::ResourceType::IN_NORMAL_ROUGHNESS;
            case EResourceSlot::VIEW_Z:
                return nrd::ResourceType::IN_VIEWZ;
            case EResourceSlot::BASECOLOR_METALNESS:
                return nrd::ResourceType::IN_BASECOLOR_METALNESS;
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

    return MakeUnique<VkNRDInterface>(m_device, _max_frame_in_flight, _frame_width, _frame_height);
}

UniquePtr<NRDInterface> VkNRDExtension::RecreateInterface(
    UniquePtr<NRDInterface> _interface,
    uint16                  _frame_width,
    uint16                  _frame_height
) {

    auto* vk_interface = static_cast<VkNRDInterface*>(_interface.get());

    vk_interface->Reinitialize(_frame_width, _frame_height);

    return std::move(_interface);
}

class VkNrdDenoiseCmd final : public VkCustomDispatchCmd {
private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return nrd_interface.resource_usages;
    }

private:
    VkNRDInterface& nrd_interface;
    nrd::Denoiser   denoiser;

public:
    VkNrdDenoiseCmd(VkNRDInterface& _nrd, const nrd::Denoiser _denoiser) :
        nrd_interface(_nrd),
        denoiser(_denoiser) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    void Execute(const VkDispatchContext& _context) const override {
        auto& nrd_integration = nrd_interface.nrd.integration;
        auto& nri             = nrd_interface.nrd.nri;
        //=======================================================================================================
        // RENDER - DENOISE
        //=======================================================================================================
        // Wrap a command buffer
        if (nrd_interface.cmd_lists_on_use.contains(uint64(_context.cmd_list))) {
            nri.cmd_list = nrd_interface.cmd_lists_on_use[uint64(_context.cmd_list)];
        } else {
            nri::CommandBufferVKDesc cmd_buffer_desc = {};
            cmd_buffer_desc.commandQueueType         = nri::CommandQueueType::GRAPHICS;
            cmd_buffer_desc.vkCommandBuffer          = _context.cmd_list;
            nri.rhi.CreateCommandBufferVK(*nri.device, cmd_buffer_desc, nri.cmd_list);
            nrd_interface.cmd_lists_on_use[uint64(_context.cmd_list)] = nri.cmd_list;
        }

        const nrd::Identifier denoiser_id = nrd::Identifier(denoiser);
        nrd_integration.Denoise(&denoiser_id, 1, *nri.cmd_list, nrd_interface.nrd.user_pool);

        // The motion vector has been trasitioned to GENERAL layout by NRD REBLUR Denoisers, so we need to flush the state
        auto mv_usage_slot =
            nrd_interface.resource_usages_indices[uint8(NRDInterface::EResourceSlot::MOTION_VECTOR)];
        if (denoiser < nrd::Denoiser::RELAX_DIFFUSE && mv_usage_slot) {
            auto* mv = ResourceCast(
                std::get<TextureView>(nrd_interface.resource_usages[mv_usage_slot - 1].resource).GetTexture()
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

void VkNRDInterface::Denoise(CommandList& _cmd_list, const nrd::Denoiser _denoiser, std::string_view _name) {
    _cmd_list.AddCustomCommand(MakeUnique<VkNrdDenoiseCmd>(*this, _denoiser), _name);
}

} // namespace Moer::Render::Ext

#endif
