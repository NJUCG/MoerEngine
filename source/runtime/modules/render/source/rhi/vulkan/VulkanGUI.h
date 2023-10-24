#ifndef MOER_VULKAN_GUI_H
#define MOER_VULKAN_GUI_H

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <cstring>

struct GuiVulkanFrame {
    VkCommandPool   command_pool;
    VkCommandBuffer command_buffer;
    VkFence         fence;
    VkImage         backbuffer;
    VkImageView     backbuffer_view;
    VkFramebuffer   framebuffer;
};

struct GuiVulkanFrameSemaphores {
    VkSemaphore image_acquired_semaphore;
    VkSemaphore render_complete_semaphore;
};
struct GuiVulkanWindow {
    int                       width;
    int                       height;
    VkSwapchainKHR            swapchain;
    VkSurfaceKHR              surface;
    VkSurfaceFormatKHR        surface_format;
    VkPresentModeKHR          present_mode;
    VkPipeline                pipeline;// The window pipeline may uses a different VkRenderPass than the one passed in ImGui_ImplVulkan_InitInfo
    bool                      use_dynamic_rendering;
    bool                      clear_enable;
    VkClearValue              clear_value;
    uint32_t                  frame_index;    // Current frame being rendered to (0 <= FrameIndex < FrameInFlightCount)
    uint32_t                  image_count;    // Number of simultaneous in-flight frames (returned by vkGetSwapchainImagesKHR, usually derived from min_image_count)
    uint32_t                  semaphore_index;// Current set of swapchain wait semaphores we're using (needs to be distinct from per frame data)
    GuiVulkanFrame*           frames;
    GuiVulkanFrameSemaphores* frame_semaphores;

    GuiVulkanWindow() {
        memset((void*)this, 0, sizeof(*this));
        present_mode = (VkPresentModeKHR)~0;// Ensure we get an error if user doesn't set this.
        clear_enable = true;
    }
};
struct GuiVulkanFrameRenderBuffers {
    // VkDeviceMemory vertex_buffer_memory;
    // VkDeviceMemory index_buffer_memory;
    VkDeviceSize vertex_buffer_size;
    VkDeviceSize index_buffer_size;

    VkBuffer      vertex_buffer;
    VmaAllocation vertex_allocation;
    VkBuffer      index_buffer;
    VmaAllocation index_allocation;
};

struct GuiVulkanWindowRenderBuffers {
    uint32_t                     index;
    uint32_t                     count;
    GuiVulkanFrameRenderBuffers* frame_render_buffers;
};

struct GuiVulkanViewportData {
    bool                         window_owned;
    GuiVulkanWindow              window;        // Used by secondary viewports only
    GuiVulkanWindowRenderBuffers render_buffers;// Used by all viewports

    GuiVulkanViewportData() {
        window_owned = false;
        memset(&render_buffers, 0, sizeof(render_buffers));
    }
    ~GuiVulkanViewportData() {}
};

struct GuiVulkanInitInfo {
    VkInstance                   instance;
    VkPhysicalDevice             physical_device;
    VkDevice                     device;
    uint32_t                     queue_family;
    VkQueue                      queue;
    VkPipelineCache              pipeline_cache;
    VkDescriptorPool             descriptor_pool;
    uint32_t                     min_image_count;// >= 2
    uint32_t                     image_count;    // >= MinImageCount
    VkSampleCountFlagBits        msaa_samples;   // >= VK_SAMPLE_COUNT_1_BIT (0 -> default to VK_SAMPLE_COUNT_1_BIT)
    VkSurfaceFormatKHR           surface_format;
    VmaAllocator                 vma_allocator;
    const VkAllocationCallbacks* allocator;
    void (*check_vk_result_fn)(VkResult err);
};
struct GuiVulkanData {
    GuiVulkanInitInfo     vulkan_init_info;
    VkRenderPass          render_pass;
    VkDeviceSize          buffer_memory_alignment;
    VkPipelineCreateFlags pipeline_create_flags;
    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout      pipeline_layout;
    VkPipeline            pipeline;
    VkShaderModule        shader_module_vert;
    VkShaderModule        shader_module_frag;

    // Font data
    VkSampler       font_sampler;
    VkDeviceMemory  font_memory;
    VkImage         font_image;
    VkImageView     font_view;
    VkDescriptorSet font_descriptor_set;
    VkDeviceMemory  upload_buffer_memory;
    VkBuffer        upload_buffer;

    // Render buffers for main window
    GuiVulkanWindowRenderBuffers main_window_render_buffers;

    GuiVulkanData() {
        memset((void*)this, 0, sizeof(*this));
        buffer_memory_alignment = 256;
    }
};

void VulkanCreateOrResizeWindow(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, GuiVulkanWindow* vulkan_window, uint32_t queue_family, const VkAllocationCallbacks* allocator, int w, int h, uint32_t min_image_count);
void VulkanDestroyWindow(VkInstance instance, VkDevice device, GuiVulkanWindow* wnd, const VkAllocationCallbacks* allocator);

#endif