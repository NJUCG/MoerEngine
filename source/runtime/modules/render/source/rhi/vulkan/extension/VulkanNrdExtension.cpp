#include "VulkanNrdExtension.h"

#include "../../vulkan/VulkanDevice.h"
#include "../../vulkan/VulkanRHIResource.h"
#include "../../vulkan/VulkanDescriptor.h"
#include "../../vulkan/VulkanCustomCommand.h"

#include <NRDIntegration.hpp>

namespace Moer::Render::Ext {

    class VkNRDInterface final : public NRDInterface {
        friend class VkNrdDenoiseCmd;

    public:
        VkNRDInterface(VulkanDevice* _device, uint8 _max_frame_in_flight, uint16 _frame_width, uint16 _frame_height) {
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
                device_desc.minorVersion        = VK_VERSION_MINOR(_device->GetCoreProperties().core_1_0.apiVersion);
                device_desc.enableNRIValidation = false;

                CHECK_ASSERT(nri::nriCreateDeviceFromVkDevice(device_desc, nri_entry.device) == nri::Result::SUCCESS,
                             "Failed to create NRI device");

                // Get core functionality
                CHECK_ASSERT(nri::nriGetInterface(*nri_entry.device,
                                                  NRI_INTERFACE(nri::CoreInterface),
                                                  (nri::CoreInterface*)&nri_entry.rhi) == nri::Result::SUCCESS,
                             "Failed to get NRI Core Interface");

                CHECK_ASSERT(nri::nriGetInterface(*nri_entry.device,
                                                  NRI_INTERFACE(nri::HelperInterface),
                                                  (nri::HelperInterface*)&nri_entry.rhi) == nri::Result::SUCCESS,
                             "Failed to get NRI Helper Interface");

                // Get appropriate "wrapper" extension (XXX - can be D3D11, D3D12 or VULKAN)
                CHECK_ASSERT(nri::nriGetInterface(*nri_entry.device,
                                                  NRI_INTERFACE(nri::WrapperVKInterface),
                                                  (nri::WrapperVKInterface*)&nri_entry.rhi) == nri::Result::SUCCESS,
                             "Failed to get NRI WrapperVK Interface");

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
                Array<nrd::DenoiserDesc> denoiser_descs = {
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
                    // {NRD_ID(SIGMA_SHADOW_TRANSLUCENCY), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
                    // {NRD_ID(SIGMA_SHADOW), nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
                    // REFERENCE
                    // {NRD_ID(REFERENCE), nrd::Denoiser::REFERENCE},
                };

                nrd::InstanceCreationDesc instance_desc = {};
                instance_desc.denoisers                 = denoiser_descs.data();
                instance_desc.denoisersNum              = denoiser_descs.size();

                // NRD itself is flexible and supports any kind of dynamic resolution scaling, but NRD INTEGRATION pre-
                // allocates resources with statically defined dimensions. DRS is only supported by adjusting the viewport
                // via "CommonSettings::rectSize"
                CHECK_ASSERT(nrd.integration.Initialize(
                                 integration_desc,
                                 instance_desc,
                                 *nri_entry.device,
                                 nri_entry.rhi,
                                 nri_entry.rhi),
                             "Failed to initialize NRD Integration");

                LOG_INFO("[NRD]: NRD Integration created or recreated.");
            }
        }

        void Begin() override {
            resource_usages.clear();
        }

        void Denoise(CommandList& _cmd_list) override;

        void SetInput(EInResource _index, TextureRef _texture) override {
            auto* vk_tex = ResourceCast(_texture);
            // Wrap required textures (better do it only once on initialization)
            auto& tex_barrier_desc = texture_barrier_descs[uint8(_index)];

            nri::TextureVKDesc tex_desc = {};

            tex_desc.vkImage     = uint64(vk_tex->GetHandle());
            tex_desc.vkFormat    = Moer::Render::VulkanEnumTranslator::METoVKFormat(vk_tex->GetFormat());
            tex_desc.vkImageType = Moer::Render::VulkanEnumTranslator::METoVKImageType(vk_tex->GetDimension());
            tex_desc.width       = vk_tex->GetWidth();
            tex_desc.height      = vk_tex->GetHeight();
            tex_desc.depth       = vk_tex->GetDepth();
            tex_desc.mipNum      = vk_tex->GetNumMips();
            tex_desc.layerNum    = vk_tex->GetNumArray();
            tex_desc.sampleNum   = 1;// 1x texture sampling

            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, (nri::Texture*&)tex_barrier_desc.texture);

            // You need to specify the current state of the resource here, after denoising NRD can modify
            // this state. Application must continue state tracking from this point.
            // Useful information:
            //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
            //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL
            tex_barrier_desc.after.access = nri::AccessBits::SHADER_RESOURCE;
            tex_barrier_desc.after.layout = nri::Layout::SHADER_RESOURCE;
            tex_barrier_desc.after.stages = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.mipOffset    = 0;
            tex_barrier_desc.mipNum       = vk_tex->GetNumMips();
            tex_barrier_desc.layerOffset  = 0;
            tex_barrier_desc.layerNum     = vk_tex->GetNumArray();
            tex_barrier_desc.planes       = nri::PlaneBits::COLOR;

            // Resource usage
            VulkanShaderResourceState pipeline_flags{};
            pipeline_flags.desc_type     = SpvReflectDescriptorType::SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            pipeline_flags.b_sampled     = 1;
            pipeline_flags.resource_type = SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV;

            ParamInfoFlags read_flag = {
                pipeline_flags(),
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

            resource_usages.emplace_back(_texture, read_flag);

            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(uint8(_index)), &tex_barrier_desc);
        }

        void SetOutput(EOutResource _index, TextureRef _texture) override {
            auto* vk_tex = ResourceCast(_texture);
            // Wrap required textures (better do it only once on initialization)
            auto& tex_barrier_desc = texture_barrier_descs[uint8(_index)];

            nri::TextureVKDesc tex_desc = {};

            tex_desc.vkImage     = uint64(vk_tex->GetHandle());
            tex_desc.vkFormat    = Moer::Render::VulkanEnumTranslator::METoVKFormat(vk_tex->GetFormat());
            tex_desc.vkImageType = Moer::Render::VulkanEnumTranslator::METoVKImageType(vk_tex->GetDimension());
            tex_desc.width       = vk_tex->GetWidth();
            tex_desc.height      = vk_tex->GetHeight();
            tex_desc.depth       = vk_tex->GetDepth();
            tex_desc.mipNum      = vk_tex->GetNumMips();
            tex_desc.layerNum    = vk_tex->GetNumArray();
            tex_desc.sampleNum   = 1;// 1x texture sampling

            nrd.nri.rhi.CreateTextureVK(*nrd.nri.device, tex_desc, (nri::Texture*&)tex_barrier_desc.texture);

            // You need to specify the current state of the resource here, after denoising NRD can modify
            // this state. Application must continue state tracking from this point.
            // Useful information:
            //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
            //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL
            tex_barrier_desc.after.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.after.layout = nri::Layout::SHADER_RESOURCE_STORAGE;
            tex_barrier_desc.after.stages = nri::StageBits::COMPUTE_SHADER;
            tex_barrier_desc.mipOffset    = 0;
            tex_barrier_desc.mipNum       = vk_tex->GetNumMips();
            tex_barrier_desc.layerOffset  = 0;
            tex_barrier_desc.layerNum     = vk_tex->GetNumArray();
            tex_barrier_desc.planes       = nri::PlaneBits::COLOR;

            // Resource usage
            VulkanShaderResourceState pipeline_flags{};
            pipeline_flags.desc_type     = SpvReflectDescriptorType::SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            pipeline_flags.b_sampled     = 0;
            pipeline_flags.resource_type = SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_UAV;

            ParamInfoFlags write_flag = {
                pipeline_flags(),
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};

            resource_usages.emplace_back(_texture, write_flag);

            nrd::Integration_SetResource(nrd.user_pool, ResourceSlot(uint8(EInResource::INPUT_NUM) + uint8(_index)), &tex_barrier_desc);
        };

    private:
        Array<CustomDispatchCmd::ResourceUsage> resource_usages;

        constexpr nrd::ResourceType ResourceSlot(const uint8 _index) {
            switch (_index) {
                case uint8(EInResource::MOTION_VECTOR): return nrd::ResourceType::IN_MV;
                case uint8(EInResource::NORMAL_ROUGHNESS): return nrd::ResourceType::IN_NORMAL_ROUGHNESS;
                case uint8(EInResource::VIEW_Z): return nrd::ResourceType::IN_VIEWZ;
                case uint8(EInResource::BASECOLOR_METALNESS): return nrd::ResourceType::IN_BASECOLOR_METALNESS;
                case uint8(EInResource::IN_DIFFUSE): return nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST;
                case uint8(EInResource::IN_SPECULAR): return nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST;
                case uint8(EInResource::INPUT_NUM) + uint8(EOutResource::OUT_DIFFUSE): return nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST;
                case uint8(EInResource::INPUT_NUM) + uint8(EOutResource::OUT_SPECULAR): return nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST;
                default: return nrd::ResourceType::MAX_NUM;
            }
        }
    };

    UniquePtr<NRDInterface> VkNRDExtension::CreateInterface(
        uint8  _max_frame_in_flight,
        uint16 _frame_width,
        uint16 _frame_height) {

        return MakeUnique<VkNRDInterface>(
            m_device,
            _max_frame_in_flight,
            _frame_width,
            _frame_height);
    }

    class VkNrdDenoiseCmd final : public VkCustomDispatchCmd {
    private:
        std::span<const ResourceUsage> GetResourceUsages() const override {
            return nrd_interface.resource_usages;
        }

    private:
        VkNRDInterface& nrd_interface;

    public:
        VkNrdDenoiseCmd(VkNRDInterface& _nrd)
            : nrd_interface(_nrd) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        void Execute(VkCommandBuffer _cmd_list) const override {
            const auto& denoiser_type   = nrd_interface.type;
            auto&       nrd_integration = nrd_interface.nrd.integration;
            auto&       nri             = nrd_interface.nrd.nri;
            //=======================================================================================================
            // RENDER - DENOISE
            //=======================================================================================================
            // Wrap a command buffer
            nri::CommandBufferVKDesc cmd_buffer_desc = {};
            cmd_buffer_desc.commandQueueType         = nri::CommandQueueType::GRAPHICS;
            cmd_buffer_desc.vkCommandBuffer          = _cmd_list;

            nri.rhi.CreateCommandBufferVK(*nri.device, cmd_buffer_desc, nri.cmd_list);

            // Must be called once on a frame start
            nrd_integration.NewFrame();

            const nrd::Identifier denoiser = nrd::Identifier(denoiser_type);
            nrd_integration.Denoise(&denoiser, 1, *nri.cmd_list, nrd_interface.nrd.user_pool);

            // IMPORTANT: NRD integration binds own descriptor pool, don't forget to re-bind back your pool (heap)
        }
    };

    void VkNRDInterface::Denoise(CommandList& _cmd_list) {
        _cmd_list.AddCustomCommand(
            MakeUnique<VkNrdDenoiseCmd>(*this),
            "NRD Denoising");
    }

}// namespace Moer::Render::Ext
