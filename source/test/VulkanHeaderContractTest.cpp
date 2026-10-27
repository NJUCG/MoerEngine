// This order is intentional: it reproduces the conflict that occurs when
// vulkan_core.h declares function prototypes before Volk declares its entry
// point variables.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan_core.h>

#ifndef VK_NO_PROTOTYPES
#error "Moer::Render must propagate VK_NO_PROTOTYPES to every consumer."
#endif

#include "rhi/RHIWindowSurface.h"
#include "rhi/vulkan/VulkanBindlessArray.h"
#include "rhi/vulkan/VulkanCommandContext.h"
#include "rhi/vulkan/VulkanCommandProfiler.h"
#include "rhi/vulkan/VulkanCommandQueries.h"
#include "rhi/vulkan/VulkanCommandRecorder.h"
#include "rhi/vulkan/VulkanDebugCallback.h"
#include "rhi/vulkan/VulkanDescriptor.h"
#include "rhi/vulkan/VulkanDeviceFeature.h"
#include "rhi/vulkan/VulkanDeviceObject.h"
#include "rhi/vulkan/VulkanDeviceProperty.h"
#include "rhi/vulkan/VulkanEnumTranslator.h"
#include "rhi/vulkan/VulkanFault.h"
#include "rhi/vulkan/VulkanMemoryAllocator.h"
#include "rhi/vulkan/VulkanPipeline.h"
#include "rhi/vulkan/VulkanPlatform.h"
#include "rhi/vulkan/VulkanRaytracingResource.h"
#include "rhi/vulkan/VulkanSubmission.h"
#include "rhi/vulkan/VulkanSurfaceSupport.h"
#include "rhi/vulkan/VulkanTemporaryBufferAllocator.h"
#include "rhi/vulkan/VulkanTimeline.h"
#include "rhi/vulkan/VulkanTypeDefs.h"
#include "window/WindowFrameSnapshot.h"

#include "platform/Platform.h"

#include <type_traits>

#if PLATFORM_WINDOWS
static_assert(std::is_base_of_v<Moer::Render::VulkanGenericPlatform, Moer::Render::VulkanPlatform>);
#endif

// Command resources own native handles and cannot be copied or relocated while
// command lists retain pointers to their pool.
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_copy_assignable_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandPool>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandList>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandList>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandContext>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandContext>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandRecorder>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandRecorder>);
// Submission packets carry the ownership gate and native resource lifetimes
// across threads. Handoff cannot copy them or fail halfway through a move.
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanRecordedSubmission>);
static_assert(std::is_nothrow_move_constructible_v<Moer::Render::VulkanRecordedSubmission>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanRecordedCopySubmission>);
static_assert(std::is_nothrow_move_constructible_v<Moer::Render::VulkanRecordedCopySubmission>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanTemporaryBufferAllocator>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanTemporaryBufferAllocator>);

static_assert(!std::is_copy_constructible_v<Moer::Render::VkNativeQueryPool>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanCommandQueries>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanCommandQueries>);

static_assert(!std::is_default_constructible_v<Moer::Render::VulkanDescriptorHeap>);
static_assert(!std::is_copy_constructible_v<Moer::Render::VulkanDescriptorHeap>);
static_assert(!std::is_move_constructible_v<Moer::Render::VulkanDescriptorHeap>);

int main() {
    return VK_SUCCESS == 0 ? 0 : 1;
}
