// #include "rhi/vulkan/VulkanRHI.h"
// #include "VulkanDevice.h"
// #include "rhi/vulkan/misc/VulkanMacroUtils.h"
// #include "vulkan/vulkan_core.h"
// #include "VulkanGUIShaders.h"
// #include "VulkanSwapChain.h"
// #include "VulkanGUI.h"
// #include "VulkanUtil.h"

// #include <vk_mem_alloc.h>
// #include <imgui.h>

// namespace Util = Moer::RHI::Vulkan::Util;

// bool GuiCreateVulkanDeviceObjects();
// void GuiCreateVulkanPipeline();
// void GuiCreateVulkanShaderModules();
// void GuiInitVulkanPlatformInterface();

// GuiVulkanData*
// GuiGetVulkanBackendData() {
//     return ImGui::GetCurrentContext() ? (GuiVulkanData*)ImGui::GetIO().BackendRendererUserData : nullptr;
// }

// bool VulkanRHIImpl::GUIInit(uint32_t _num_frames_in_flight) {
//     //todo: make sure vk functions are loaded

//     ImGuiIO& io = ImGui::GetIO();
//     assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

//     GuiVulkanData* vulkan_backend_data = IM_NEW(GuiVulkanData)();

//     io.BackendRendererUserData = vulkan_backend_data;
//     io.BackendRendererName     = "VulkanRHI";
//     io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
//     io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

//     auto& init_info           = vulkan_backend_data->vulkan_init_info;
//     init_info.instance        = m_instance;
//     init_info.physical_device = m_device->GetGpu();
//     init_info.queue           = m_device->GetGraphicsQueue();
//     init_info.queue_family    = m_device->GetQueueFamilyIndices().graphics.value();
//     init_info.surface_format  = m_swap_chain->GetSurfaceFormat();

//     VkDescriptorPoolSize pool_sizes[] = {
//         {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
//         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024},
//         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
//         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
//         {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},
//         {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
//         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
//         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
//         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
//         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
//         {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1024}};
//     VkDescriptorPoolCreateInfo pool_info{
//         VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
//         VK_NULL_HANDLE,
//         VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
//         1024 * IM_ARRAYSIZE(pool_sizes),
//         IM_ARRAYSIZE(pool_sizes),
//         pool_sizes};

//     VK_CHECK_RESULT(vkCreateDescriptorPool(m_device->GetDevice(), &pool_info, VK_NULL_HANDLE, &(init_info.descriptor_pool)));

//     ImGuiViewport* main_viewport    = ImGui::GetMainViewport();
//     main_viewport->RendererUserData = IM_NEW(GuiVulkanViewportData)();

//     if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
//         GuiInitVulkanPlatformInterface();

//     return true;
// }

// void VulkanRHIImpl::GUINewFrame() {
//     GuiVulkanData* bd = GuiGetVulkanBackendData();
//     IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplVulkan_Init()?");
//     IM_UNUSED(bd);
// }

// void VulkanRHIImpl::GUIShutDown() {
//     ImGuiIO& io = ImGui::GetIO();
//     if (GuiVulkanData* bd = GuiGetVulkanBackendData()) {
//         const auto& init_info = bd->vulkan_init_info;
//         //destroy ui descriptor pool
//         vkDestroyDescriptorPool(m_device->GetDevice(), init_info.descriptor_pool, nullptr);

//         // First destroy objects in all viewports
//         GuiVulkanInitInfo* v = &bd->vulkan_init_info;

//         ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
//         for (int n = 0; n < platform_io.Viewports.Size; n++) {
//             if (GuiVulkanViewportData* vd = (GuiVulkanViewportData*)platform_io.Viewports[n]->RendererUserData) {

//                 for (uint32_t n = 0; n < vd->render_buffers.count; n++) {
//                     auto buffers = vd->render_buffers.frame_render_buffers[n];
//                     if (buffers.vertex_buffer) {
//                         vmaDestroyBuffer(v->vma_allocator, buffers.vertex_buffer, buffers.vertex_allocation);
//                         buffers.vertex_buffer = VK_NULL_HANDLE;
//                     }
//                     if (buffers.index_buffer) {
//                         vmaDestroyBuffer(v->vma_allocator, buffers.index_buffer, buffers.index_allocation);
//                         buffers.index_buffer = VK_NULL_HANDLE;
//                     }

//                     buffers.vertex_buffer_size = 0;
//                     buffers.index_buffer_size  = 0;
//                 }
//                 auto buffers = vd->render_buffers;
//                 IM_FREE(buffers.frame_render_buffers);
//                 buffers.frame_render_buffers = nullptr;
//                 buffers.index                = 0;
//                 buffers.count                = 0;
//             }
//         }

//         if (bd->upload_buffer) {
//             vkDestroyBuffer(v->device, bd->upload_buffer, v->allocator);
//             bd->upload_buffer = VK_NULL_HANDLE;
//         }
//         if (bd->upload_buffer_memory) {
//             vkFreeMemory(v->device, bd->upload_buffer_memory, v->allocator);
//             bd->upload_buffer_memory = VK_NULL_HANDLE;
//         }

//         if (bd->shader_module_vert) {
//             vkDestroyShaderModule(v->device, bd->shader_module_vert, v->allocator);
//             bd->shader_module_vert = VK_NULL_HANDLE;
//         }
//         if (bd->shader_module_frag) {
//             vkDestroyShaderModule(v->device, bd->shader_module_frag, v->allocator);
//             bd->shader_module_frag = VK_NULL_HANDLE;
//         }
//         if (bd->font_view) {
//             vkDestroyImageView(v->device, bd->font_view, v->allocator);
//             bd->font_view = VK_NULL_HANDLE;
//         }
//         if (bd->font_image) {
//             vkDestroyImage(v->device, bd->font_image, v->allocator);
//             bd->font_image = VK_NULL_HANDLE;
//         }
//         if (bd->font_memory) {
//             vkFreeMemory(v->device, bd->font_memory, v->allocator);
//             bd->font_memory = VK_NULL_HANDLE;
//         }
//         if (bd->font_sampler) {
//             vkDestroySampler(v->device, bd->font_sampler, v->allocator);
//             bd->font_sampler = VK_NULL_HANDLE;
//         }
//         if (bd->descriptor_set_layout) {
//             vkDestroyDescriptorSetLayout(v->device, bd->descriptor_set_layout, v->allocator);
//             bd->descriptor_set_layout = VK_NULL_HANDLE;
//         }
//         if (bd->pipeline_layout) {
//             vkDestroyPipelineLayout(v->device, bd->pipeline_layout, v->allocator);
//             bd->pipeline_layout = VK_NULL_HANDLE;
//         }
//         if (bd->pipeline) {
//             vkDestroyPipeline(v->device, bd->pipeline, v->allocator);
//             bd->pipeline = VK_NULL_HANDLE;
//         }

//         // Manually delete main viewport render data in-case we haven't initialized for viewports
//         ImGuiViewport* main_viewport = ImGui::GetMainViewport();
//         if (GuiVulkanViewportData* vd = (GuiVulkanViewportData*)main_viewport->RendererUserData)
//             IM_DELETE(vd);
//         main_viewport->RendererUserData = nullptr;

//         // Clean up windows
//         ImGui::DestroyPlatformWindows();

//         io.BackendRendererName     = nullptr;
//         io.BackendRendererUserData = nullptr;
//         io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);
//         IM_DELETE(bd);
//     }
// }

// bool GuiCreateVulkanDeviceObjects() {
//     GuiVulkanData*     vulkan_backend_data = GuiGetVulkanBackendData();
//     GuiVulkanInitInfo* init_info           = &vulkan_backend_data->vulkan_init_info;

//     VkResult result;
//     if (!vulkan_backend_data->font_sampler) {
//         VkSamplerCreateInfo info = {};
//         info.sType               = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
//         info.magFilter           = VK_FILTER_LINEAR;
//         info.minFilter           = VK_FILTER_LINEAR;
//         info.mipmapMode          = VK_SAMPLER_MIPMAP_MODE_LINEAR;
//         info.addressModeU        = VK_SAMPLER_ADDRESS_MODE_REPEAT;
//         info.addressModeV        = VK_SAMPLER_ADDRESS_MODE_REPEAT;
//         info.addressModeW        = VK_SAMPLER_ADDRESS_MODE_REPEAT;
//         info.minLod              = -1000;
//         info.maxLod              = 1000;
//         info.maxAnisotropy       = 1.0f;
//         result                   = vkCreateSampler(init_info->device, &info, init_info->allocator, &vulkan_backend_data->font_sampler);
//         VK_CHECK_RESULT(result);
//     }

//     if (!vulkan_backend_data->pipeline_layout) {
//         // Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full 3d projection matrix
//         VkPushConstantRange push_constants[1]    = {};
//         push_constants[0].stageFlags             = VK_SHADER_STAGE_VERTEX_BIT;
//         push_constants[0].offset                 = sizeof(float) * 0;
//         push_constants[0].size                   = sizeof(float) * 4;
//         VkDescriptorSetLayout      set_layout[1] = {vulkan_backend_data->descriptor_set_layout};
//         VkPipelineLayoutCreateInfo layout_info   = {};
//         layout_info.sType                        = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
//         layout_info.setLayoutCount               = 1;
//         layout_info.pSetLayouts                  = set_layout;
//         layout_info.pushConstantRangeCount       = 1;
//         layout_info.pPushConstantRanges          = push_constants;
//         result                                   = vkCreatePipelineLayout(init_info->device, &layout_info, init_info->allocator, &vulkan_backend_data->pipeline_layout);
//         VK_CHECK_RESULT(result);
//     }
//     GuiCreateVulkanPipeline();
//     return true;
// };

// void GuiCreateVulkanPipeline() {
//     GuiVulkanData* vulkan_backend_data = GuiGetVulkanBackendData();
//     auto           msaa_samples        = vulkan_backend_data->vulkan_init_info.msaa_samples;

//     auto        swapchain_format = vulkan_backend_data->vulkan_init_info.surface_format.format;
//     const auto& init_info        = vulkan_backend_data->vulkan_init_info;

//     auto* device         = vulkan_backend_data->vulkan_init_info.device;
//     auto& pipeline_cache = init_info.pipeline_cache;
//     auto* allocator      = init_info.allocator;

//     GuiCreateVulkanShaderModules();

//     VkPipelineShaderStageCreateInfo stage[2]{};
//     stage[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//     stage[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
//     stage[0].module = vulkan_backend_data->shader_module_vert;
//     stage[0].pName  = "main";

//     stage[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//     stage[1].stage  = VK_SHADER_STAGE_VERTEX_BIT;
//     stage[1].module = vulkan_backend_data->shader_module_frag;
//     stage[1].pName  = "main";

//     VkVertexInputBindingDescription binding_desc[1] = {};
//     binding_desc[0].stride                          = sizeof(ImDrawVert);
//     binding_desc[0].inputRate                       = VK_VERTEX_INPUT_RATE_VERTEX;

//     VkVertexInputAttributeDescription attribute_desc[3] = {};
//     attribute_desc[0].location                          = 0;
//     attribute_desc[0].binding                           = binding_desc[0].binding;
//     attribute_desc[0].format                            = VK_FORMAT_R32G32_SFLOAT;
//     attribute_desc[0].offset                            = IM_OFFSETOF(ImDrawVert, pos);

//     attribute_desc[1].location = 1;
//     attribute_desc[1].binding  = binding_desc[0].binding;
//     attribute_desc[1].format   = VK_FORMAT_R32G32_SFLOAT;
//     attribute_desc[1].offset   = IM_OFFSETOF(ImDrawVert, uv);

//     attribute_desc[1].location = 2;
//     attribute_desc[1].binding  = binding_desc[0].binding;
//     attribute_desc[1].format   = VK_FORMAT_R8G8B8A8_UNORM;
//     attribute_desc[1].offset   = IM_OFFSETOF(ImDrawVert, col);

//     VkPipelineVertexInputStateCreateInfo vertex_info = {};
//     vertex_info.sType                                = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
//     vertex_info.vertexBindingDescriptionCount        = 1;
//     vertex_info.pVertexBindingDescriptions           = binding_desc;
//     vertex_info.vertexAttributeDescriptionCount      = 3;
//     vertex_info.pVertexAttributeDescriptions         = attribute_desc;

//     VkPipelineInputAssemblyStateCreateInfo input_assemble_info{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
//     input_assemble_info.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

//     VkPipelineViewportStateCreateInfo viewport_info{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
//     viewport_info.viewportCount = 1;
//     viewport_info.scissorCount  = 1;

//     VkPipelineRasterizationStateCreateInfo raster_info = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
//     raster_info.polygonMode                            = VK_POLYGON_MODE_FILL;
//     raster_info.cullMode                               = VK_CULL_MODE_NONE;
//     raster_info.frontFace                              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
//     raster_info.lineWidth                              = 1.0f;

//     VkPipelineMultisampleStateCreateInfo ms_info = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
//     ms_info.rasterizationSamples                 = (msaa_samples != 0) ? msaa_samples : VK_SAMPLE_COUNT_1_BIT;

//     VkPipelineColorBlendAttachmentState color_attachment[1] = {};
//     color_attachment[0].blendEnable                         = VK_TRUE;
//     color_attachment[0].srcColorBlendFactor                 = VK_BLEND_FACTOR_SRC_ALPHA;
//     color_attachment[0].dstColorBlendFactor                 = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
//     color_attachment[0].colorBlendOp                        = VK_BLEND_OP_ADD;
//     color_attachment[0].srcAlphaBlendFactor                 = VK_BLEND_FACTOR_ONE;
//     color_attachment[0].dstAlphaBlendFactor                 = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
//     color_attachment[0].alphaBlendOp                        = VK_BLEND_OP_ADD;
//     color_attachment[0].colorWriteMask                      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

//     VkPipelineDepthStencilStateCreateInfo depth_info = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

//     VkPipelineColorBlendStateCreateInfo blend_info = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
//     blend_info.attachmentCount                     = 1;
//     blend_info.pAttachments                        = color_attachment;

//     VkDynamicState                   dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
//     VkPipelineDynamicStateCreateInfo dynamic_state     = {};
//     dynamic_state.sType                                = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
//     dynamic_state.dynamicStateCount                    = (uint32_t)IM_ARRAYSIZE(dynamic_states);
//     dynamic_state.pDynamicStates                       = dynamic_states;

//     const VkPipelineRenderingCreateInfo pipeline_rendering_create_info{
//         .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
//         .colorAttachmentCount    = 1,
//         .pColorAttachmentFormats = &swapchain_format,
//     };
//     VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
//     info.pNext                        = &pipeline_rendering_create_info;
//     info.flags                        = vulkan_backend_data->pipeline_create_flags;
//     info.stageCount                   = 2;
//     info.pStages                      = stage;
//     info.pVertexInputState            = &vertex_info;
//     info.pInputAssemblyState          = &input_assemble_info;
//     info.pViewportState               = &viewport_info;
//     info.pRasterizationState          = &raster_info;
//     info.pMultisampleState            = &ms_info;
//     info.pDepthStencilState           = &depth_info;
//     info.pColorBlendState             = &blend_info;
//     info.pDynamicState                = &dynamic_state;
//     info.layout                       = vulkan_backend_data->pipeline_layout;
//     info.renderPass                   = nullptr;
//     info.subpass                      = 0;
//     VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipeline_cache, 1, &info, allocator, &vulkan_backend_data->pipeline));
// }

// void GuiCreateVulkanShaderModules() {
//     GuiVulkanData* vulkan_backend_data = GuiGetVulkanBackendData();
//     auto*          device              = vulkan_backend_data->vulkan_init_info.device;
//     auto*          allocator           = vulkan_backend_data->vulkan_init_info.allocator;
//     if (vulkan_backend_data->shader_module_vert == VK_NULL_HANDLE) {
//         VkShaderModuleCreateInfo vert_info{
//             VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
//             VK_NULL_HANDLE,
//             0,
//             sizeof(glsl_shader_vert_spv),
//             (uint32_t*)glsl_shader_vert_spv};
//         VK_CHECK_RESULT(vkCreateShaderModule(device, &vert_info, allocator, &vulkan_backend_data->shader_module_vert));
//     }
//     if (vulkan_backend_data->shader_module_frag == VK_NULL_HANDLE) {

//         VkShaderModuleCreateInfo vert_info{
//             VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
//             VK_NULL_HANDLE,
//             0,
//             sizeof(glsl_shader_frag_spv),
//             (uint32_t*)glsl_shader_frag_spv};
//         VK_CHECK_RESULT(vkCreateShaderModule(device, &vert_info, allocator, &vulkan_backend_data->shader_module_frag));
//     }
// }

// void GuiCreateVulkanWindow(ImGuiViewport* viewport);
// void GuiDestroyVulkanWindow(ImGuiViewport* viewport);
// void GuiSetVulkanWindowSize(ImGuiViewport* viewport, ImVec2 size);
// void GuiRenderVulkanWindow(ImGuiViewport* viewport, void*);
// void GuiSwapVulkanBuffers(ImGuiViewport* viewport, void*);

// //register io functions
// void GuiInitVulkanPlatformInterface() {
//     ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
//     if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
//         IM_ASSERT(platform_io.Platform_CreateVkSurface != nullptr && "Platform needs to setup the CreateVkSurface handler.");
//     platform_io.Renderer_CreateWindow  = GuiCreateVulkanWindow;
//     platform_io.Renderer_DestroyWindow = GuiDestroyVulkanWindow;
//     platform_io.Renderer_SetWindowSize = GuiSetVulkanWindowSize;
//     platform_io.Renderer_RenderWindow  = GuiRenderVulkanWindow;
//     platform_io.Renderer_SwapBuffers   = GuiSwapVulkanBuffers;
// }

// #pragma region interface implement
// /**
//  * @brief Destroy UI frame fence, commandbuffer, commandpool .etc
//  *
//  * @param device
//  * @param fd
//  * @param allocator
//  */
// void GuiDestroyVulkanFrame(VkDevice device, GuiVulkanFrame* fd, const VkAllocationCallbacks* allocator);
// /**
//  * @brief Destroy UI frame semaphores: image_aquire, render_complete .etc
//  *
//  * @param device
//  * @param frame_semaphores
//  * @param allocator
//  */
// void GuiDestroyVulkanFrameSemaphores(VkDevice device, GuiVulkanFrameSemaphores* fsd, const VkAllocationCallbacks* allocator);
// void GuiDestroyVulkanFrameRenderBuffers(VkDevice device, GuiVulkanFrameRenderBuffers* buffers, VmaAllocator allocator);
// void GuiDestroyVulkanWindowRenderBuffers(VkDevice device, GuiVulkanWindowRenderBuffers* buffers, VmaAllocator allocator);
// void GuiDestroyAllViewportsRenderBuffers(VkDevice device, VmaAllocator allocator);
// void GuiCreateVulkanWindowCommandBuffers(VkPhysicalDevice physical_device, VkDevice device, GuiVulkanWindow* wd, uint32_t queue_family, const VkAllocationCallbacks* allocator);

// int  GuiGetMinImageCountFromPresentMode(VkPresentModeKHR present_mode);
// void GuiRender(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline);

// void GuiCreateVulkanWindowSwapChain(
//     VkPhysicalDevice             physical_device,
//     VkDevice                     device,
//     GuiVulkanWindow*             wd,
//     const VkAllocationCallbacks* allocator,
//     int                          w,
//     int                          h,
//     uint32_t                     min_image_count);

// void VulkanCreateOrResizeWindow(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, GuiVulkanWindow* wd, uint32_t queue_family, const VkAllocationCallbacks* allocator, int width, int height, uint32_t min_image_count) {
//     (void)instance;
//     GuiCreateVulkanWindowSwapChain(physical_device, device, wd, allocator, width, height, min_image_count);
//     //ImGui_ImplVulkan_CreatePipeline(device, allocator, VK_NULL_HANDLE, wd->RenderPass, VK_SAMPLE_COUNT_1_BIT, &wd->Pipeline, g_VulkanInitInfo.Subpass);
//     GuiCreateVulkanWindowCommandBuffers(physical_device, device, wd, queue_family, allocator);
// }

// void GuiCreateVulkanWindow(ImGuiViewport* viewport) {
//     GuiVulkanData*         vulkan_backend_data = GuiGetVulkanBackendData();
//     GuiVulkanViewportData* viewport_data       = IM_NEW(GuiVulkanViewportData)();
//     viewport->RendererUserData                 = viewport_data;
//     GuiVulkanWindow*   window_data             = &viewport_data->window;
//     GuiVulkanInitInfo* init_info               = &vulkan_backend_data->vulkan_init_info;

//     // Create surface
//     ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
//     VK_CHECK_RESULT((VkResult)platform_io.Platform_CreateVkSurface(viewport, (ImU64)init_info->instance, (const void*)init_info->allocator, (ImU64*)&window_data->surface));

//     // Check for WSI support
//     VkBool32 res;
//     vkGetPhysicalDeviceSurfaceSupportKHR(init_info->physical_device, init_info->queue_family, window_data->surface, &res);
//     if (res != VK_TRUE) {
//         IM_ASSERT(0);// Error: no WSI support on physical device
//         return;
//     }

//     // Select Surface Format
//     const VkFormat        request_surface_image_format[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
//     const VkColorSpaceKHR request_surface_color_space    = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
//     window_data->surface_format                          = Util::SelectSurfaceFormat(init_info->physical_device, window_data->surface, request_surface_image_format, (size_t)IM_ARRAYSIZE(request_surface_image_format), request_surface_color_space);

//     // Select Present Mode
//     // FIXME-VULKAN: Even thought mailbox seems to get us maximum framerate with a single window, it halves framerate with a second window etc. (w/ Nvidia and SDK 1.82.1)
//     VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
//     window_data->present_mode        = Util::SelectPresentMode(init_info->physical_device, window_data->surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
//     //printf("[vulkan] Secondary window selected PresentMode = %d\n", wd->PresentMode);

//     // Create SwapChain, RenderPass, Framebuffer, etc.
//     window_data->clear_enable          = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) ? false : true;
//     window_data->use_dynamic_rendering = true;
//     VulkanCreateOrResizeWindow(init_info->instance, init_info->physical_device, init_info->device, window_data, init_info->queue_family, init_info->allocator, (int)viewport->Size.x, (int)viewport->Size.y, init_info->min_image_count);
//     viewport_data->window_owned = true;
// }
// void GuiDestroyVulkanWindow(ImGuiViewport* viewport) {
//     // The main viewport (owned by the application) will always have RendererUserData == 0 since we didn't create the data for it.
//     GuiVulkanData* bd = GuiGetVulkanBackendData();
//     if (GuiVulkanViewportData* vd = (GuiVulkanViewportData*)viewport->RendererUserData) {
//         GuiVulkanInitInfo* v = &bd->vulkan_init_info;
//         if (vd->window_owned)
//             VulkanDestroyWindow(v->instance, v->device, &vd->window, v->allocator);
//         GuiDestroyVulkanWindowRenderBuffers(v->device, &vd->render_buffers, v->vma_allocator);
//         IM_DELETE(vd);
//     }
//     viewport->RendererUserData = nullptr;
// }
// void GuiSetVulkanWindowSize(ImGuiViewport* viewport, ImVec2 size) {
//     GuiVulkanData*         vulkan_backend_data = GuiGetVulkanBackendData();
//     GuiVulkanViewportData* viewport_data       = (GuiVulkanViewportData*)viewport->RendererUserData;
//     if (viewport_data == nullptr)// This is nullptr for the main viewport (which is left to the user/app to handle)
//         return;
//     GuiVulkanInitInfo* init_info       = &vulkan_backend_data->vulkan_init_info;
//     viewport_data->window.clear_enable = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) ? false : true;
//     VulkanCreateOrResizeWindow(
//         init_info->instance,
//         init_info->physical_device,
//         init_info->device,
//         &viewport_data->window,
//         init_info->queue_family,
//         init_info->allocator,
//         (int)size.x,
//         (int)size.y,
//         init_info->min_image_count);
// }
// void GuiRenderVulkanWindow(ImGuiViewport* viewport, void*) {
//     GuiVulkanData*         vulkan_backend_data = GuiGetVulkanBackendData();
//     GuiVulkanViewportData* viewport_data       = (GuiVulkanViewportData*)viewport->RendererUserData;
//     GuiVulkanWindow*       window_data         = &viewport_data->window;
//     GuiVulkanInitInfo*     init_info           = &vulkan_backend_data->vulkan_init_info;
//     VkResult               err;

//     GuiVulkanFrame*           frame_data       = &window_data->frames[window_data->frame_index];
//     GuiVulkanFrameSemaphores* frame_semaphores = &window_data->frame_semaphores[window_data->semaphore_index];
//     {
//         {
//             VK_CHECK_RESULT(
//                 vkAcquireNextImageKHR(
//                     init_info->device,
//                     window_data->swapchain,
//                     UINT64_MAX,
//                     frame_semaphores->image_acquired_semaphore,
//                     VK_NULL_HANDLE,
//                     &window_data->frame_index));
//             frame_data = &window_data->frames[window_data->frame_index];
//         }
//         for (;;) {
//             err = vkWaitForFences(init_info->device, 1, &frame_data->fence, VK_TRUE, 100);
//             if (err == VK_SUCCESS) break;
//             if (err == VK_TIMEOUT) continue;
//             VK_CHECK_RESULT(err);
//         }
//         {
//             VK_CHECK_RESULT(vkResetCommandPool(init_info->device, frame_data->command_pool, 0));
//             VkCommandBufferBeginInfo info = {};
//             info.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
//             info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
//             VK_CHECK_RESULT(vkBeginCommandBuffer(frame_data->command_buffer, &info));
//         }
//         {
//             ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
//             memcpy(&window_data->clear_value.color.float32[0], &clear_color, 4 * sizeof(float));
//         }
//     }

//     // Transition swapchain image to a layout suitable for drawing.
//     VkImageMemoryBarrier barrier        = {};
//     barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
//     barrier.dstAccessMask               = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
//     barrier.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
//     barrier.newLayout                   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
//     barrier.image                       = frame_data->backbuffer;
//     barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
//     barrier.subresourceRange.levelCount = 1;
//     barrier.subresourceRange.layerCount = 1;
//     vkCmdPipelineBarrier(frame_data->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

//     VkRenderingAttachmentInfo attachment_info = {};
//     attachment_info.sType                     = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
//     attachment_info.imageView                 = frame_data->backbuffer_view;
//     attachment_info.imageLayout               = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
//     attachment_info.resolveMode               = VK_RESOLVE_MODE_NONE;
//     attachment_info.loadOp                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
//     attachment_info.storeOp                   = VK_ATTACHMENT_STORE_OP_STORE;
//     attachment_info.clearValue                = window_data->clear_value;

//     VkRenderingInfo rendering_info          = {};
//     rendering_info.sType                    = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
//     rendering_info.renderArea.extent.width  = window_data->width;
//     rendering_info.renderArea.extent.height = window_data->height;
//     rendering_info.layerCount               = 1;
//     rendering_info.viewMask                 = 0;
//     rendering_info.colorAttachmentCount     = 1;
//     rendering_info.pColorAttachments        = &attachment_info;

//     vkCmdBeginRendering(frame_data->command_buffer, &rendering_info);

//     GuiRender(viewport->DrawData, frame_data->command_buffer, window_data->pipeline);
//     {
//         vkCmdEndRendering(frame_data->command_buffer);

//         // Transition image to a layout suitable for presentation
//         VkImageMemoryBarrier barrier        = {};
//         barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
//         barrier.srcAccessMask               = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
//         barrier.oldLayout                   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
//         barrier.newLayout                   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
//         barrier.image                       = frame_data->backbuffer;
//         barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
//         barrier.subresourceRange.levelCount = 1;
//         barrier.subresourceRange.layerCount = 1;
//         vkCmdPipelineBarrier(frame_data->command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
//     }

//     VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
//     VkSubmitInfo         info       = {};
//     info.sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO;
//     info.waitSemaphoreCount         = 1;
//     info.pWaitSemaphores            = &frame_semaphores->image_acquired_semaphore;
//     info.pWaitDstStageMask          = &wait_stage;
//     info.commandBufferCount         = 1;
//     info.pCommandBuffers            = &frame_data->command_buffer;
//     info.signalSemaphoreCount       = 1;
//     info.pSignalSemaphores          = &frame_semaphores->render_complete_semaphore;

//     err = vkEndCommandBuffer(frame_data->command_buffer);
//     VK_CHECK_RESULT(err);
//     err = vkResetFences(init_info->device, 1, &frame_data->fence);
//     VK_CHECK_RESULT(err);
//     err = vkQueueSubmit(init_info->queue, 1, &info, frame_data->fence);
//     VK_CHECK_RESULT(err);
// }

// void GuiSwapVulkanBuffers(ImGuiViewport* viewport, void*) {
//     GuiVulkanData*         bd = GuiGetVulkanBackendData();
//     GuiVulkanViewportData* vd = (GuiVulkanViewportData*)viewport->RendererUserData;
//     GuiVulkanWindow*       wd = &vd->window;
//     GuiVulkanInitInfo*     v  = &bd->vulkan_init_info;

//     VkResult err;
//     uint32_t present_index = wd->frame_index;

//     GuiVulkanFrameSemaphores* fsd  = &wd->frame_semaphores[wd->semaphore_index];
//     VkPresentInfoKHR          info = {};
//     info.sType                     = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
//     info.waitSemaphoreCount        = 1;
//     info.pWaitSemaphores           = &fsd->render_complete_semaphore;
//     info.swapchainCount            = 1;
//     info.pSwapchains               = &wd->swapchain;
//     info.pImageIndices             = &present_index;
//     err                            = vkQueuePresentKHR(v->queue, &info);
//     if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
//         VulkanCreateOrResizeWindow(
//             v->instance,
//             v->physical_device,
//             v->device,
//             &vd->window,
//             v->queue_family,
//             v->allocator,
//             (int)viewport->Size.x,
//             (int)viewport->Size.y,
//             v->min_image_count);
//     else
//         VK_CHECK_RESULT(err);

//     wd->frame_index     = (wd->frame_index + 1) % wd->image_count;    // This is for the next vkWaitForFences()
//     wd->semaphore_index = (wd->semaphore_index + 1) % wd->image_count;// Now we can use the next set of semaphores
// }

// int GuiGetMinImageCountFromPresentMode(VkPresentModeKHR present_mode) {
//     if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
//         return 3;
//     if (present_mode == VK_PRESENT_MODE_FIFO_KHR || present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
//         return 2;
//     if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
//         return 1;
//     IM_ASSERT(0);
//     return 1;
// }
// #pragma endregion

// void GuiDestroyVulkanFrame(VkDevice device, GuiVulkanFrame* fd, const VkAllocationCallbacks* allocator) {
//     vkDestroyFence(device, fd->fence, allocator);
//     vkFreeCommandBuffers(device, fd->command_pool, 1, &fd->command_buffer);
//     vkDestroyCommandPool(device, fd->command_pool, allocator);
//     fd->fence          = VK_NULL_HANDLE;
//     fd->command_buffer = VK_NULL_HANDLE;
//     fd->command_pool   = VK_NULL_HANDLE;

//     vkDestroyImageView(device, fd->backbuffer_view, allocator);
//     //we use dynamic rendering, so we don't care about framebuffer
//     // vkDestroyFramebuffer(device, fd->framebuffer, allocator);
// }

// void GuiDestroyVulkanFrameSemaphores(VkDevice device, GuiVulkanFrameSemaphores* frame_semaphores, const VkAllocationCallbacks* allocator) {
//     vkDestroySemaphore(device, frame_semaphores->image_acquired_semaphore, allocator);
//     vkDestroySemaphore(device, frame_semaphores->render_complete_semaphore, allocator);
//     frame_semaphores->image_acquired_semaphore = frame_semaphores->render_complete_semaphore = VK_NULL_HANDLE;
// }

// //todo: use vma
// void GuiDestroyVulkanFrameRenderBuffers(VkDevice device, GuiVulkanFrameRenderBuffers* buffers, VmaAllocator allocator) {
//     if (buffers->vertex_buffer) {
//         vmaDestroyBuffer(allocator, buffers->vertex_buffer, buffers->vertex_allocation);
//         buffers->vertex_buffer = VK_NULL_HANDLE;
//     }
//     if (buffers->index_buffer) {
//         vmaDestroyBuffer(allocator, buffers->index_buffer, buffers->index_allocation);
//         buffers->index_buffer = VK_NULL_HANDLE;
//     }
//     buffers->vertex_buffer_size = 0;
//     buffers->index_buffer_size  = 0;
// }
// void GuiDestroyVulkanWindowRenderBuffers(VkDevice device, GuiVulkanWindowRenderBuffers* buffers, VmaAllocator allocator) {
//     for (uint32_t n = 0; n < buffers->count; n++)
//         GuiDestroyVulkanFrameRenderBuffers(device, &buffers->frame_render_buffers[n], allocator);
//     IM_FREE(buffers->frame_render_buffers);
//     buffers->frame_render_buffers = nullptr;
//     buffers->index                = 0;
//     buffers->count                = 0;
// }
// void GuiDestroyAllViewportsRenderBuffers(
//     VkDevice     device,
//     VmaAllocator allocator) {
//     ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
//     for (int n = 0; n < platform_io.Viewports.Size; n++)
//         if (GuiVulkanViewportData* viewport_data = (GuiVulkanViewportData*)platform_io.Viewports[n]->RendererUserData)
//             GuiDestroyVulkanWindowRenderBuffers(device, &viewport_data->render_buffers, allocator);
// }
// void GuiCreateVulkanWindowSwapChain(
//     VkPhysicalDevice             physical_device,
//     VkDevice                     device,
//     GuiVulkanWindow*             wd,
//     const VkAllocationCallbacks* allocator,
//     int                          w,
//     int                          h,
//     uint32_t                     min_image_count) {

//     VkSwapchainKHR old_swapchain = wd->swapchain;
//     wd->swapchain                = VK_NULL_HANDLE;
//     VK_CHECK_RESULT(vkDeviceWaitIdle(device));

//     // We don't use GuiDestroyVulkanWindow() because we want to preserve the old swapchain to create the new one.
//     // Destroy old Per-Frame Data
//     for (uint32_t i = 0; i < wd->image_count; i++) {
//         GuiDestroyVulkanFrame(device, &wd->frames[i], allocator);
//         GuiDestroyVulkanFrameSemaphores(device, &wd->frame_semaphores[i], allocator);
//     }
//     IM_FREE(wd->frames);
//     IM_FREE(wd->frame_semaphores);
//     wd->frames           = nullptr;
//     wd->frame_semaphores = nullptr;
//     wd->image_count      = 0;

//     if (wd->pipeline)
//         vkDestroyPipeline(device, wd->pipeline, allocator);

//     // If min image count was not specified, request different count of images dependent on selected present mode
//     if (min_image_count == 0)
//         min_image_count = GuiGetMinImageCountFromPresentMode(wd->present_mode);

//     // Create Swapchain
//     {
//         VkSwapchainCreateInfoKHR info = {};
//         info.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
//         info.surface                  = wd->surface;
//         info.minImageCount            = min_image_count;
//         info.imageFormat              = wd->surface_format.format;
//         info.imageColorSpace          = wd->surface_format.colorSpace;
//         info.imageArrayLayers         = 1;
//         info.imageUsage               = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
//         info.imageSharingMode         = VK_SHARING_MODE_EXCLUSIVE;// Assume that graphics family == present family
//         info.preTransform             = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
//         info.compositeAlpha           = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
//         info.presentMode              = wd->present_mode;
//         info.clipped                  = VK_TRUE;
//         info.oldSwapchain             = old_swapchain;
//         VkSurfaceCapabilitiesKHR cap;
//         VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, wd->surface, &cap));
//         if (info.minImageCount < cap.minImageCount)
//             info.minImageCount = cap.minImageCount;
//         else if (cap.maxImageCount != 0 && info.minImageCount > cap.maxImageCount)
//             info.minImageCount = cap.maxImageCount;

//         if (cap.currentExtent.width == 0xffffffff) {
//             info.imageExtent.width = wd->width = w;
//             info.imageExtent.height = wd->height = h;
//         } else {
//             info.imageExtent.width = wd->width = cap.currentExtent.width;
//             info.imageExtent.height = wd->height = cap.currentExtent.height;
//         }
//         VK_CHECK_RESULT(vkCreateSwapchainKHR(device, &info, allocator, &wd->swapchain));
//         VK_CHECK_RESULT(vkGetSwapchainImagesKHR(device, wd->swapchain, &wd->image_count, nullptr));

//         VkImage backbuffers[16] = {};
//         IM_ASSERT(wd->image_count >= min_image_count);
//         IM_ASSERT(wd->image_count < IM_ARRAYSIZE(backbuffers));
//         VK_CHECK_RESULT(vkGetSwapchainImagesKHR(device, wd->swapchain, &wd->image_count, backbuffers));

//         IM_ASSERT(wd->frames == nullptr);
//         wd->frames           = (GuiVulkanFrame*)IM_ALLOC(sizeof(GuiVulkanFrame) * wd->image_count);
//         wd->frame_semaphores = (GuiVulkanFrameSemaphores*)IM_ALLOC(sizeof(GuiVulkanFrameSemaphores) * wd->image_count);
//         memset(wd->frames, 0, sizeof(wd->frames[0]) * wd->image_count);
//         memset(wd->frame_semaphores, 0, sizeof(wd->frame_semaphores[0]) * wd->image_count);
//         for (uint32_t i = 0; i < wd->image_count; i++)
//             wd->frames[i].backbuffer = backbuffers[i];
//     }
//     if (old_swapchain)
//         vkDestroySwapchainKHR(device, old_swapchain, allocator);

//     // Create The Image Views
//     {
//         VkImageViewCreateInfo info          = {};
//         info.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
//         info.viewType                       = VK_IMAGE_VIEW_TYPE_2D;
//         info.format                         = wd->surface_format.format;
//         info.components.r                   = VK_COMPONENT_SWIZZLE_R;
//         info.components.g                   = VK_COMPONENT_SWIZZLE_G;
//         info.components.b                   = VK_COMPONENT_SWIZZLE_B;
//         info.components.a                   = VK_COMPONENT_SWIZZLE_A;
//         VkImageSubresourceRange image_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
//         info.subresourceRange               = image_range;
//         for (uint32_t i = 0; i < wd->image_count; i++) {
//             GuiVulkanFrame* fd = &wd->frames[i];
//             info.image         = fd->backbuffer;
//             VK_CHECK_RESULT(vkCreateImageView(device, &info, allocator, &fd->backbuffer_view));
//         }
//     }
// }

// void GuiCreateVulkanWindowCommandBuffers(
//     VkPhysicalDevice             physical_device,
//     VkDevice                     device,
//     GuiVulkanWindow*             wd,
//     uint32_t                     queue_family,
//     const VkAllocationCallbacks* allocator) {
//     assert(physical_device != VK_NULL_HANDLE && device != VK_NULL_HANDLE);
//     (void)physical_device;
//     (void)allocator;

//     // Create Command Buffers
//     VkResult err;
//     for (uint32_t i = 0; i < wd->image_count; i++) {
//         GuiVulkanFrame*           fd  = &wd->frames[i];
//         GuiVulkanFrameSemaphores* fsd = &wd->frame_semaphores[i];
//         {
//             VkCommandPoolCreateInfo info = {};
//             info.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
//             info.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
//             info.queueFamilyIndex        = queue_family;
//             VK_CHECK_RESULT(vkCreateCommandPool(device, &info, allocator, &fd->command_pool));
//         }
//         {
//             VkCommandBufferAllocateInfo info = {};
//             info.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
//             info.commandPool                 = fd->command_pool;
//             info.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
//             info.commandBufferCount          = 1;
//             VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &info, &fd->command_buffer));
//         }
//         {
//             VkFenceCreateInfo info = {};
//             info.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
//             info.flags             = VK_FENCE_CREATE_SIGNALED_BIT;
//             VK_CHECK_RESULT(vkCreateFence(device, &info, allocator, &fd->fence));
//         }
//         {
//             VkSemaphoreCreateInfo info = {};
//             info.sType                 = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
//             VK_CHECK_RESULT(vkCreateSemaphore(device, &info, allocator, &fsd->image_acquired_semaphore));
//             VK_CHECK_RESULT(vkCreateSemaphore(device, &info, allocator, &fsd->render_complete_semaphore));
//         }
//     }
// };

// void VulkanDestroyWindow(VkInstance instance, VkDevice device, GuiVulkanWindow* vulkan_window, const VkAllocationCallbacks* allocator) {

//     vkDeviceWaitIdle(device);// FIXME: We could wait on the Queue if we had the queue in wd-> (otherwise VulkanH functions can't use globals)
//     //vkQueueWaitIdle(bd->Queue);

//     for (uint32_t i = 0; i < vulkan_window->image_count; i++) {
//         GuiDestroyVulkanFrame(device, &vulkan_window->frames[i], allocator);
//         GuiDestroyVulkanFrameSemaphores(device, &vulkan_window->frame_semaphores[i], allocator);
//     }
//     IM_FREE(vulkan_window->frames);
//     IM_FREE(vulkan_window->frame_semaphores);
//     vulkan_window->frames           = nullptr;
//     vulkan_window->frame_semaphores = nullptr;
//     vkDestroyPipeline(device, vulkan_window->pipeline, allocator);
//     //no render passes
//     // vkDestroyRenderPass(device, wd->render_pass, allocator);
//     vkDestroySwapchainKHR(device, vulkan_window->swapchain, allocator);
//     vkDestroySurfaceKHR(instance, vulkan_window->surface, allocator);

//     *vulkan_window = GuiVulkanWindow();
// }

// #pragma rengion draw

// void CreateOrResizeBuffer(VkBuffer& buffer, VmaAllocation& buffer_allocation, VkDeviceSize& p_buffer_size, size_t new_size, VkBufferUsageFlagBits usage);

// void GuiSetupRenderState(ImDrawData* draw_data, VkPipeline pipeline, VkCommandBuffer command_buffer, GuiVulkanFrameRenderBuffers* rb, int fb_width, int fb_height) {
//     GuiVulkanData* bd = GuiGetVulkanBackendData();

//     // Bind pipeline:
//     {
//         vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
//     }

//     // Bind Vertex And Index Buffer:
//     if (draw_data->TotalVtxCount > 0) {
//         VkBuffer     vertex_buffers[1] = {rb->vertex_buffer};
//         VkDeviceSize vertex_offset[1]  = {0};
//         vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, vertex_offset);
//         vkCmdBindIndexBuffer(command_buffer, rb->index_buffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
//     }

//     // Setup viewport:
//     {
//         VkViewport viewport;
//         viewport.x        = 0;
//         viewport.y        = 0;
//         viewport.width    = (float)fb_width;
//         viewport.height   = (float)fb_height;
//         viewport.minDepth = 0.0f;
//         viewport.maxDepth = 1.0f;
//         vkCmdSetViewport(command_buffer, 0, 1, &viewport);
//     }

//     // Setup scale and translation:
//     // Our visible imgui space lies from draw_data->DisplayPps (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
//     {
//         float scale[2];
//         scale[0] = 2.0f / draw_data->DisplaySize.x;
//         scale[1] = 2.0f / draw_data->DisplaySize.y;
//         float translate[2];
//         translate[0] = -1.0f - draw_data->DisplayPos.x * scale[0];
//         translate[1] = -1.0f - draw_data->DisplayPos.y * scale[1];
//         vkCmdPushConstants(command_buffer, bd->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 0, sizeof(float) * 2, scale);
//         vkCmdPushConstants(command_buffer, bd->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 2, sizeof(float) * 2, translate);
//     }
// }
// void GuiRender(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline) {
//     // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
//     int fb_width  = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
//     int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
//     if (fb_width <= 0 || fb_height <= 0)
//         return;

//     GuiVulkanData*     bd = GuiGetVulkanBackendData();
//     GuiVulkanInitInfo* v  = &bd->vulkan_init_info;
//     if (pipeline == VK_NULL_HANDLE)
//         pipeline = bd->pipeline;

//     // Allocate array to store enough vertex/index buffers. Each unique viewport gets its own storage.
//     GuiVulkanViewportData* viewport_renderer_data = (GuiVulkanViewportData*)draw_data->OwnerViewport->RendererUserData;
//     IM_ASSERT(viewport_renderer_data != nullptr);
//     GuiVulkanWindowRenderBuffers* wrb = &viewport_renderer_data->render_buffers;
//     if (wrb->frame_render_buffers == nullptr) {
//         wrb->index                = 0;
//         wrb->count                = v->image_count;
//         wrb->frame_render_buffers = (GuiVulkanFrameRenderBuffers*)IM_ALLOC(sizeof(GuiVulkanFrameRenderBuffers) * wrb->count);
//         memset(wrb->frame_render_buffers, 0, sizeof(GuiVulkanFrameRenderBuffers) * wrb->count);
//     }
//     IM_ASSERT(wrb->count == v->image_count);
//     wrb->index                      = (wrb->index + 1) % wrb->count;
//     GuiVulkanFrameRenderBuffers* rb = &wrb->frame_render_buffers[wrb->index];

//     if (draw_data->TotalVtxCount > 0) {
//         // Create or resize the vertex/index buffers
//         size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
//         size_t index_size  = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
//         if (rb->vertex_buffer == VK_NULL_HANDLE || rb->vertex_buffer_size < vertex_size)
//             CreateOrResizeBuffer(rb->vertex_buffer, rb->vertex_allocation, rb->vertex_buffer_size, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
//         if (rb->index_buffer == VK_NULL_HANDLE || rb->index_buffer_size < index_size)
//             CreateOrResizeBuffer(rb->index_buffer, rb->index_allocation, rb->index_buffer_size, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

//         // Upload vertex/index data into a single contiguous GPU buffer
//         ImDrawVert* vtx_dst = nullptr;
//         ImDrawIdx*  idx_dst = nullptr;
//         VK_CHECK_RESULT(vmaMapMemory(v->vma_allocator, rb->vertex_allocation, (void**)(&vtx_dst)));
//         VK_CHECK_RESULT(vmaMapMemory(v->vma_allocator, rb->index_allocation, (void**)(&idx_dst)));
//         for (int n = 0; n < draw_data->CmdListsCount; n++) {
//             const ImDrawList* cmd_list = draw_data->CmdLists[n];
//             memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
//             memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
//             vtx_dst += cmd_list->VtxBuffer.Size;
//             idx_dst += cmd_list->IdxBuffer.Size;
//         }
//         // VkMappedMemoryRange range[2] = {};
//         // range[0].sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
//         // range[0].memory              = rb->vertex_buffer_memory;
//         // range[0].size                = VK_WHOLE_SIZE;
//         // range[1].sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
//         // range[1].memory              = rb->index_buffer_memory;
//         // range[1].size                = VK_WHOLE_SIZE;
//         VmaAllocation allocations[2] = {rb->vertex_allocation, rb->index_allocation};
//         size_t        offsets[2]     = {0, 0};
//         size_t        sizes[2]       = {VK_WHOLE_SIZE, VK_WHOLE_SIZE};
//         VK_CHECK_RESULT(vmaFlushAllocations(v->vma_allocator, 2, allocations, offsets, sizes));
//         vmaUnmapMemory(v->vma_allocator, rb->vertex_allocation);
//         vmaUnmapMemory(v->vma_allocator, rb->index_allocation);
//     }

//     // Setup desired Vulkan state
//     GuiSetupRenderState(draw_data, pipeline, command_buffer, rb, fb_width, fb_height);

//     // Will project scissor/clipping rectangles into framebuffer space
//     ImVec2 clip_off   = draw_data->DisplayPos;      // (0,0) unless using multi-viewports
//     ImVec2 clip_scale = draw_data->FramebufferScale;// (1,1) unless using retina display which are often (2,2)

//     // Render command lists
//     // (Because we merged all buffers into a single one, we maintain our own offset into them)
//     int global_vtx_offset = 0;
//     int global_idx_offset = 0;
//     for (int n = 0; n < draw_data->CmdListsCount; n++) {
//         const ImDrawList* cmd_list = draw_data->CmdLists[n];
//         for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
//             const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
//             if (pcmd->UserCallback != nullptr) {
//                 // User callback, registered via ImDrawList::AddCallback()
//                 // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
//                 if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
//                     GuiSetupRenderState(draw_data, pipeline, command_buffer, rb, fb_width, fb_height);
//                 else
//                     pcmd->UserCallback(cmd_list, pcmd);
//             } else {
//                 // Project scissor/clipping rectangles into framebuffer space
//                 ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
//                 ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

//                 // Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
//                 if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
//                 if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
//                 if (clip_max.x > fb_width) { clip_max.x = (float)fb_width; }
//                 if (clip_max.y > fb_height) { clip_max.y = (float)fb_height; }
//                 if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
//                     continue;

//                 // Apply scissor/clipping rectangle
//                 VkRect2D scissor;
//                 scissor.offset.x      = (int32_t)(clip_min.x);
//                 scissor.offset.y      = (int32_t)(clip_min.y);
//                 scissor.extent.width  = (uint32_t)(clip_max.x - clip_min.x);
//                 scissor.extent.height = (uint32_t)(clip_max.y - clip_min.y);
//                 vkCmdSetScissor(command_buffer, 0, 1, &scissor);

//                 // Bind DescriptorSet with font or user texture
//                 VkDescriptorSet desc_set[1] = {(VkDescriptorSet)pcmd->TextureId};
//                 if (sizeof(ImTextureID) < sizeof(ImU64)) {
//                     // We don't support texture switches if ImTextureID hasn't been redefined to be 64-bit. Do a flaky check that other textures haven't been used.
//                     IM_ASSERT(pcmd->TextureId == (ImTextureID)bd->font_descriptor_set);
//                     desc_set[0] = bd->font_descriptor_set;
//                 }
//                 vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bd->pipeline_layout, 0, 1, desc_set, 0, nullptr);

//                 // Draw
//                 vkCmdDrawIndexed(command_buffer, pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset, 0);
//             }
//         }
//         global_idx_offset += cmd_list->IdxBuffer.Size;
//         global_vtx_offset += cmd_list->VtxBuffer.Size;
//     }

//     // Note: at this point both vkCmdSetViewport() and vkCmdSetScissor() have been called.
//     // Our last values will leak into user/application rendering IF:
//     // - Your app uses a pipeline with VK_DYNAMIC_STATE_VIEWPORT or VK_DYNAMIC_STATE_SCISSOR dynamic state
//     // - And you forgot to call vkCmdSetViewport() and vkCmdSetScissor() yourself to explicitly set that state.
//     // If you use VK_DYNAMIC_STATE_VIEWPORT or VK_DYNAMIC_STATE_SCISSOR you are responsible for setting the values before rendering.
//     // In theory we should aim to backup/restore those values but I am not sure this is possible.
//     // We perform a call to vkCmdSetScissor() to set back a full viewport which is likely to fix things for 99% users but technically this is not perfect. (See github #4644)
//     VkRect2D scissor = {{0, 0}, {(uint32_t)fb_width, (uint32_t)fb_height}};
//     vkCmdSetScissor(command_buffer, 0, 1, &scissor);
// }
// #pragma endregion

// #pragma region utils

// void CreateOrResizeBuffer(VkBuffer& buffer, VmaAllocation& buffer_allocation, VkDeviceSize& p_buffer_size, size_t new_size, VkBufferUsageFlagBits usage) {
//     GuiVulkanData*     bd = GuiGetVulkanBackendData();
//     GuiVulkanInitInfo* v  = &bd->vulkan_init_info;
//     VkResult           err;
//     if (buffer != VK_NULL_HANDLE)
//         vmaDestroyBuffer(v->vma_allocator, buffer, VK_NULL_HANDLE);
//     // vkDestroyBuffer(v->device, buffer, v->allocator);
//     if (buffer_allocation != VK_NULL_HANDLE)
//         vmaDestroyBuffer(v->vma_allocator, buffer, buffer_allocation);

//     VkDeviceSize       vertex_buffer_size_aligned = ((new_size - 1) / bd->buffer_memory_alignment + 1) * bd->buffer_memory_alignment;
//     VkBufferCreateInfo buffer_info                = {};
//     buffer_info.sType                             = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
//     buffer_info.size                              = vertex_buffer_size_aligned;
//     buffer_info.usage                             = usage;
//     buffer_info.sharingMode                       = VK_SHARING_MODE_EXCLUSIVE;
//     // err                                           = vkCreateBuffer(v->device, &buffer_info, v->allocator, &buffer);

//     VmaAllocationCreateInfo vma_info{};
//     vma_info.flags = 0;
//     vma_info.usage = VMA_MEMORY_USAGE_AUTO;

//     VkMemoryRequirements req;
//     vkGetBufferMemoryRequirements(v->device, buffer, &req);
//     bd->buffer_memory_alignment = (bd->buffer_memory_alignment > req.alignment) ? bd->buffer_memory_alignment : req.alignment;
//     vmaCreateBufferWithAlignment(v->vma_allocator, &buffer_info, &vma_info, bd->buffer_memory_alignment, &buffer, &buffer_allocation, nullptr);

//     p_buffer_size = req.size;
// }
// #pragma endregion