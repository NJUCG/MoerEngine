#include "VulkanNrdPlugin.h"

#if WITH_NRD

#include "../../vulkan/VulkanCustomCommand.h"
#include "../../vulkan/VulkanDescriptor.h"
#include "../../vulkan/VulkanDevice.h"
#include "../../vulkan/VulkanRHIResource.h"

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
                {NRD_ID(SIGMA_SHADOW), nrd::Denoiser::SIGMA_SHADOW},
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

    void Denoise(
        CommandList&     _cmd_list,
        PreparedFrameRef _frame,
        std::string_view _name
    ) override;

private:
    void SetInput(
        nrd::UserPool&     _user_pool,
        EResourceSlot      _index,
        const TextureRef&  _texture
    ) {
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
            nrd::Integration_SetResource(_user_pool, ResourceSlot(_index), &tex_barrier_desc);
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
            CHECK_ASSERT(
                nrd.nri.rhi.CreateTextureVK(
                    *nrd.nri.device,
                    tex_desc,
                    (nri::Texture*&)tex_barrier_desc.texture
                ) == nri::Result::SUCCESS,
                "Failed to wrap an NRD input texture"
            );
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
            nrd::Integration_SetResource(_user_pool, ResourceSlot(_index), &tex_barrier_desc);
        }
    }

    void SetOutput(
        nrd::UserPool&     _user_pool,
        EResourceSlot      _index,
        const TextureRef&  _texture
    ) {
        assert(_index >= EResourceSlot::OUT_DIFFUSE && "Output resource index is invalid");
        auto* vk_tex = ResourceCast(_texture);

        auto& tex_barrier_desc_map = texture_barrier_descs[uint8(_index)];

        auto desc_key = uint64(vk_tex->GetHandle());
        if (tex_barrier_desc_map.contains(desc_key)) {
            auto& tex_barrier_desc = tex_barrier_desc_map[desc_key];
            nrd::Integration_SetResource(_user_pool, ResourceSlot(_index), &tex_barrier_desc);
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
            CHECK_ASSERT(
                nrd.nri.rhi.CreateTextureVK(
                    *nrd.nri.device,
                    tex_desc,
                    (nri::Texture*&)tex_barrier_desc.texture
                ) == nri::Result::SUCCESS,
                "Failed to wrap an NRD output texture"
            );
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
            nrd::Integration_SetResource(_user_pool, ResourceSlot(_index), &tex_barrier_desc);
        }
    };

private:
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

SharedPtr<NRDInterface>
VkNRDPlugin::CreateInterface(uint8 _max_frame_in_flight, uint16 _frame_width, uint16 _frame_height) {
    return MakeShared<VkNRDInterface>(
        m_device,
        _max_frame_in_flight,
        _frame_width,
        _frame_height
    );
}

SharedPtr<NRDInterface> VkNRDPlugin::RecreateInterface(
    SharedPtr<NRDInterface> _interface,
    uint16                  _frame_width,
    uint16                  _frame_height
) {
    const uint8 max_frame_in_flight =
        _interface ? _interface->GetMaxFrameInFlight() : 0;
    // A published custom command owns the previous shared interface until
    // completion. Resize therefore creates a new backend generation instead
    // of mutating Integration/resources that may still be translating.
    return CreateInterface(
        max_frame_in_flight,
        _frame_width,
        _frame_height
    );
}

class VkNrdDenoiseCmd final : public VkCustomDispatchCmd {
private:
    std::span<const ResourceUsage> GetResourceUsages() const override {
        return resource_usages;
    }

private:
    SharedPtr<VkNRDInterface>        nrd_interface;
    NRDInterface::PreparedFrameRef   frame;
    Array<ResourceUsage>             resource_usages{};

public:
    VkNrdDenoiseCmd(
        SharedPtr<VkNRDInterface>      _nrd,
        NRDInterface::PreparedFrameRef _frame
    ) :
        nrd_interface(std::move(_nrd)),
        frame(std::move(_frame)) {
        for (uint8 index = 0;
             index < NRDInterface::resource_slot_count;
             ++index) {
            const auto slot =
                static_cast<NRDInterface::EResourceSlot>(index);
            const TextureRef& texture = frame->GetResource(slot);
            if (!texture) {
                continue;
            }

            VulkanShaderResourceState pipeline_flags{};
            pipeline_flags.b_sampled = 1;
            if (slot < NRDInterface::EResourceSlot::OUT_DIFFUSE) {
                pipeline_flags.desc_type     = VDT_SAMPLED_IMAGE;
                pipeline_flags.resource_type = SRT_SRV;
            } else {
                pipeline_flags.desc_type     = VDT_STORAGE_IMAGE;
                pipeline_flags.resource_type = SRT_UAV;
            }
            resource_usages.emplace_back(
                texture,
                ParamInfoFlags{
                    pipeline_flags(),
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                }
            );
        }
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    void Execute(const VkDispatchContext& _context) const override {
        auto& nrd_integration = nrd_interface->nrd.integration;
        auto& nri             = nrd_interface->nrd.nri;
        nrd::UserPool user_pool{};

        nrd_integration.NewFrame();
        CHECK_ASSERT(
            nrd_integration.SetCommonSettings(frame->GetCommonSettings()),
            "Failed to apply immutable NRD common settings"
        );
        for (uint8 index = 0;
             index < uint8(NRDInterface::EResourceSlot::OUT_DIFFUSE);
             ++index) {
            const auto slot =
                static_cast<NRDInterface::EResourceSlot>(index);
            const TextureRef& texture = frame->GetResource(slot);
            if (texture) {
                nrd_interface->SetInput(user_pool, slot, texture);
            }
        }
        for (uint8 index =
                 uint8(NRDInterface::EResourceSlot::OUT_DIFFUSE);
             index < NRDInterface::resource_slot_count;
             ++index) {
            const auto slot =
                static_cast<NRDInterface::EResourceSlot>(index);
            const TextureRef& texture = frame->GetResource(slot);
            if (texture) {
                nrd_interface->SetOutput(user_pool, slot, texture);
            }
        }
        //=======================================================================================================
        // RENDER - DENOISE
        //=======================================================================================================
        // Wrap a command buffer
        if (nrd_interface->cmd_lists_on_use.contains(uint64(_context.cmd_list))) {
            nri.cmd_list =
                nrd_interface->cmd_lists_on_use[uint64(_context.cmd_list)];
        } else {
            nri::CommandBufferVKDesc cmd_buffer_desc = {};
            cmd_buffer_desc.commandQueueType         = nri::CommandQueueType::GRAPHICS;
            cmd_buffer_desc.vkCommandBuffer          = _context.cmd_list;
            CHECK_ASSERT(
                nri.rhi.CreateCommandBufferVK(
                    *nri.device, cmd_buffer_desc, nri.cmd_list
                ) == nri::Result::SUCCESS,
                "Failed to wrap the Vulkan command buffer for NRD"
            );
            nrd_interface->cmd_lists_on_use[uint64(_context.cmd_list)] =
                nri.cmd_list;
        }

        const nrd::Denoiser denoiser = frame->GetDenoiser();
        const nrd::Identifier denoiser_id = nrd::Identifier(denoiser);
        nrd_integration.Denoise(
            &denoiser_id,
            1,
            *nri.cmd_list,
            user_pool
        );

        // Old UserPool-based NRD leaves REBLUR motion in GENERAL. Restore the
        // graph-declared Sampled state with a real native barrier; changing
        // only VkTracker would make the next explicit RDG barrier name the
        // wrong oldLayout.
        if (IsReblurDenoiser(denoiser)) {
            const TextureRef& motion = frame->GetResource(
                NRDInterface::EResourceSlot::MOTION_VECTOR
            );
            auto* vk_motion = ResourceCast(motion);
            VkImageMemoryBarrier2 restore_motion{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2
            };
            restore_motion.srcStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            restore_motion.srcAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT |
                VK_ACCESS_2_SHADER_WRITE_BIT;
            restore_motion.dstStageMask =
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            restore_motion.dstAccessMask =
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            restore_motion.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            restore_motion.newLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            restore_motion.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            restore_motion.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            restore_motion.image = vk_motion->GetHandle();
            restore_motion.subresourceRange = VkImageSubresourceRange{
                .aspectMask =
                    VulkanEnumTranslator::METoVKImageAspectFlags(
                        vk_motion->GetAspectFlags()
                    ),
                .baseMipLevel   = 0,
                .levelCount     = vk_motion->GetNumMips(),
                .baseArrayLayer = 0,
                .layerCount     = vk_motion->GetNumArray(),
            };
            const VkDependencyInfo dependency{
                .sType                   =
                    VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                   = nullptr,
                .dependencyFlags         = 0,
                .memoryBarrierCount      = 0,
                .pMemoryBarriers         = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers   = nullptr,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &restore_motion,
            };
            vkCmdPipelineBarrier2(_context.cmd_list, &dependency);
        }

        auto* vk_tracker = static_cast<VkTracker*>(_context.user_data);
        for (uint8 index = 0;
             index < NRDInterface::resource_slot_count;
             ++index) {
            const auto slot =
                static_cast<NRDInterface::EResourceSlot>(index);
            const TextureRef& texture = frame->GetResource(slot);
            if (!texture) {
                continue;
            }
            auto* vk_texture = ResourceCast(texture);
            if (slot < NRDInterface::EResourceSlot::OUT_DIFFUSE) {
                vk_tracker->FlushSrcState(
                    vk_texture,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                );
            } else {
                vk_tracker->FlushSrcState(
                    vk_texture,
                    static_cast<VkAccessFlagBits2>(
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    ),
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                );
            }
        }
        // IMPORTANT: NRD integration binds own descriptor pool, don't forget to re-bind back your pool (heap)
    }
};

void VkNRDInterface::Denoise(
    CommandList&     _cmd_list,
    PreparedFrameRef _frame,
    std::string_view _name
) {
    if (!_frame || !_frame->IsValid()) {
        throw std::invalid_argument(
            "NRD denoise requires a valid immutable prepared frame"
        );
    }
    if (!OwnsPreparedFrame(_frame)) {
        throw std::invalid_argument(
            "NRD denoise prepared frame belongs to another interface generation"
        );
    }
    // The external NRD/NRI integration owns mutable descriptor and native
    // command-buffer wrapper caches. Make SerialControl a backend invariant
    // for every caller, including the linear warm-up path; the graph policy is
    // retained as a declarative compiler contract.
    _cmd_list.SetTranslateExecutionClass(
        ERHITranslateExecutionClass::SerialControl
    );
    auto owner = std::static_pointer_cast<VkNRDInterface>(
        shared_from_this()
    );
    _cmd_list.AddCustomCommand(
        MakeUnique<VkNrdDenoiseCmd>(
            std::move(owner),
            std::move(_frame)
        ),
        _name
    );
}

} // namespace Moer::Render::Ext

#endif
