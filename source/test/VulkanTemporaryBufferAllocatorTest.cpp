#include "rhi/vulkan/VulkanRHIResource.h"
#include "rhi/vulkan/VulkanTemporaryBufferAllocator.h"

#include <iostream>
#include <stdexcept>

namespace {
int live_buffers = 0;
int allocation_attempts = 0;
int fail_on_attempt = -1;

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename F>
void CheckAllocationFailure(F&& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    Check(rejected, "injected allocation failure was not propagated");
    fail_on_attempt = -1;
}
} // namespace

// This isolated target links the real allocator, but no Render/Vulkan device
// implementation. Buffers count native allocations and inject creation failures.
void RHIResource::Destroy() {
    MoerDelete(this);
}

namespace Moer::Render {
class VulkanDevice {};

// MSVC emits the exported inline DepthBuffer::GetView even though this
// buffer-only fixture never calls it. Keep the fixture independent of Render.
TextureView Texture::GetView(uint8, uint8) {
    throw std::logic_error("texture access is outside the temporary-buffer fixture");
}

VulkanBuffer::VulkanBuffer(
    std::string_view, const BufferInfo& info, VulkanDevice& device,
    const VulkanBufferAllocationInfo&, bool
) : Buffer(info), VulkanDeviceObject(&device) {
    if (++allocation_attempts == fail_on_attempt) {
        throw std::bad_alloc();
    }
    ++live_buffers;
}

VulkanBuffer::~VulkanBuffer() {
    --live_buffers;
}

void VulkanBuffer::Destroy() {
    MoerDelete(this);
}

void VulkanBuffer::SetName(std::string_view) {}
} // namespace Moer::Render

int main() {
    using namespace Moer::Render;
    VulkanDevice device;

    // Failure of the second member must release the first member's buffer.
    fail_on_attempt = 2;
    CheckAllocationFailure([&] { VulkanTemporaryBufferAllocator allocator(device); });
    Check(live_buffers == 0, "partial allocator construction leaked the upload block");

    {
        VulkanTemporaryBufferAllocator allocator(device);
        const auto first = allocator.AllocateUploadBuffer(32 * 1024, 256);
        const auto second = allocator.AllocateUploadBuffer(32 * 1024, 256);
        Check(first.buffer == second.buffer && first.byte_offset + first.GetByteSize() <= second.byte_offset,
              "linear allocations overlap");
        const auto third = allocator.AllocateUploadBuffer(32 * 1024, 256);
        Check(third.buffer != first.buffer && live_buffers == 3, "linear growth did not retain the old block");

        fail_on_attempt = allocation_attempts + 1;
        CheckAllocationFailure([&] { allocator.Reset(); });
        Check(live_buffers == 3, "failed reset discarded retained blocks");
        allocator.Reset();
        Check(live_buffers == 2, "successful reset did not merge staging blocks");

        const auto reused = allocator.AllocateUploadBuffer(16, 16);
        allocator.Reset();
        Check(allocator.AllocateUploadBuffer(16, 16).buffer == reused.buffer,
              "single staging block was not reused");

        allocator.AllocateUploadBuffer(128 * 1024, 16);
        allocator.AllocateReadbackBuffer(128 * 1024, 16);
        allocator.AllocateScratch(1024);
        allocator.AllocateShaderBuffer(1024);
        Check(live_buffers == 6, "independent buffers were released before recording retirement");
        fail_on_attempt = allocation_attempts + 1;
        CheckAllocationFailure([&] { allocator.AllocateScratch(1024); });
        Check(live_buffers == 6, "failed allocation changed existing ownership");
        allocator.Reset();
        Check(live_buffers == 2, "retirement did not release independent buffers");
        allocator.AllocateShaderBuffer(1024);
    }
    Check(live_buffers == 0, "allocator destruction leaked buffers");
    std::cout << "Temporary buffer construction failure, growth, retirement and destruction passed\n";
}
