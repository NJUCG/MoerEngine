#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/RHI.h"
#include "rhi/RHIExecutor.h"
#include "rhi/metal/MetalDevice.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class SmokeWindowSource final : public Moer::Render::WindowSurfaceSource {
public:
    explicit SmokeWindowSource(GLFWwindow* window) : window_(window) {}

    Moer::Render::WindowSurfaceIdentity GetIdentity() const noexcept override {
        return {Moer::Render::EWindowSystemType::GLFW,
                reinterpret_cast<uintptr_t>(window_), 0, 1};
    }

    Moer::Render::WindowNativeHandle GetNativeWindow() const noexcept override {
        return {Moer::Render::EWindowSystemType::GLFW,
                reinterpret_cast<uintptr_t>(window_), 0};
    }

private:
    GLFWwindow* window_;
};

Moer::Render::TextureRef ClearAndCheckTexture(int width, int height) {
    using namespace Moer::Render;
    TextureRef texture = RenderDevice::Get().CreateTexture(
        Extent2D(width, height), PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC
    );
    id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(texture.Get());
    if (native == nil) throw std::runtime_error("Metal texture bridge failed");
    id<MTLCommandQueue> queue = [native.device newCommandQueue];

    CommandList clear(EQueueType::Graphics);
    clear.ClearResource(texture->GetView(), Moer::float4{0.08f, 0.4f, 0.75f, 1.0f});
    RHIExecutor::Get().Submit(EQueueType::Graphics, clear.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const NSUInteger row_bytes = ((static_cast<NSUInteger>(width) * 4 + 255) / 256) * 256;
    id<MTLBuffer> readback = [native.device newBufferWithLength:row_bytes * height
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:row_bytes
       destinationBytesPerImage:row_bytes * height];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    const auto* pixel = static_cast<const uint8_t*>(readback.contents);
    if (command.status != MTLCommandBufferStatusCompleted ||
        pixel[0] < 15 || pixel[0] > 30 ||
        pixel[1] < 95 || pixel[1] > 110 ||
        pixel[2] < 185 || pixel[2] > 200 || pixel[3] != 255) {
        throw std::runtime_error("Metal clear readback has an unexpected color");
    }
    std::cout << "clear/readback " << width << "x" << height << ": "
              << int(pixel[0]) << "," << int(pixel[1]) << ","
              << int(pixel[2]) << "," << int(pixel[3]) << std::endl;
    return texture;
}

void UploadAndCheckTexture(
    EPixelFormat format,
    ETextureUsageFlags usage,
    std::span<const uint8_t> input,
    size_t pixel_stride
) {
    using namespace Moer::Render;
    constexpr int width = 7;
    constexpr int height = 5;
    if (input.size_bytes() != width * height * pixel_stride) {
        throw std::runtime_error("Metal upload test input has the wrong size");
    }
    TextureRef texture = RenderDevice::Get().CreateTexture(Extent2D(width, height), format, usage);
    FenceRef copy_fence = RenderDevice::Get().GetCopyQueue().GetFenceHandle();
    const uint64_t before_upload = copy_fence->GetValue();
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        texture->GetView()
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint64_t after_upload = copy_fence->GetValue();
    if (after_upload <= before_upload ||
        !copy_fence->WaitSubmitted(after_upload) || copy_fence->IsRejected(after_upload)) {
        throw std::runtime_error("Metal copy fence did not complete the upload");
    }

    TextureRef copied = RenderDevice::Get().CreateTexture(Extent2D(width, height), format, usage);
    CommandList copy(EQueueType::Copy);
    copy.CopyFrom(texture->GetView(), copied->GetView());
    RHIExecutor::Get().Submit(EQueueType::Copy, copy.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const uint64_t after_copy = copy_fence->GetValue();
    if (after_copy <= after_upload ||
        !copy_fence->WaitSubmitted(after_copy) || copy_fence->IsRejected(after_copy)) {
        throw std::runtime_error("Metal copy fence did not complete the texture copy");
    }

    Moer::Array<ExportTexture> exports;
    exports.emplace_back(copied->GetView(), ETextureState::SAMPLE);
    CommandList export_command(EQueueType::Copy);
    export_command.ExportResourcesToQueue(EQueueType::Graphics, std::move(exports), {});
    RHIExecutor::Get().Submit(EQueueType::Copy, export_command.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    Moer::Array<ImportTexture> imports;
    imports.emplace_back(copied->GetView(), ETextureState::SAMPLE);
    CommandList import_command(EQueueType::Graphics);
    import_command.ImportResourcesFromQueue(EQueueType::Copy, std::move(imports), {});
    RHIExecutor::Get().Submit(
        EQueueType::Graphics, import_command.Submit().Wait(copy_fence, after_copy)
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    id<MTLTexture> native = (__bridge id<MTLTexture>)GetMetalNativeTexture(copied.Get());
    if (native == nil) throw std::runtime_error("Metal upload texture bridge failed");
    const NSUInteger row_bytes = (width * pixel_stride + 255) & ~NSUInteger(255);
    id<MTLBuffer> readback = [native.device newBufferWithLength:row_bytes * height
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [native.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:native sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:row_bytes
       destinationBytesPerImage:row_bytes * height];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        throw std::runtime_error("Metal upload readback failed");
    }
    const auto* output = static_cast<const uint8_t*>(readback.contents);
    for (int y = 0; y < height; ++y) {
        for (size_t x = 0; x < width * pixel_stride; ++x) {
            if (output[y * row_bytes + x] != input[y * width * pixel_stride + x]) {
                throw std::runtime_error("Metal texture upload readback differs from input");
            }
        }
    }
    std::cout << "texture upload/copy/transfer/readback " << width << "x" << height
              << " stride " << pixel_stride << ": success" << std::endl;
}

void CheckBufferUpload() {
    using namespace Moer::Render;
    constexpr size_t offset = 16;
    std::vector<uint8_t> input(20);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i * 11);
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal smoke buffer", BufferInfo{64, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        buffer->GetView(offset, input.size())
    );
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    BufferRef copied = RenderDevice::Get().CreateBuffer(
        "Metal smoke copied buffer", BufferInfo{64, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    constexpr size_t destination_offset = 8;
    CommandList copy(EQueueType::Copy);
    copy.CopyFrom(buffer->GetView(offset, input.size()),
                  copied->GetView(destination_offset, input.size()));
    RHIExecutor::Get().Submit(EQueueType::Copy, copy.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    id<MTLBuffer> native = (__bridge id<MTLBuffer>)GetMetalNativeBuffer(copied.Get());
    if (native == nil) throw std::runtime_error("Metal buffer bridge failed");
    id<MTLBuffer> readback = [native.device newBufferWithLength:input.size()
                                                       options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [native.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:native sourceOffset:destination_offset toBuffer:readback
      destinationOffset:0 size:input.size()];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted ||
        !std::equal(input.begin(), input.end(), static_cast<const uint8_t*>(readback.contents))) {
        throw std::runtime_error("Metal buffer upload readback differs from input");
    }
    std::cout << "buffer upload/copy/readback at offsets " << offset << "/"
              << destination_offset << ": success" << std::endl;
}

void CheckCopyCompletionCallbacks() {
    using namespace Moer::Render;
    auto callback_state = std::make_shared<std::atomic<int>>(0);
    BufferRef buffer = RenderDevice::Get().CreateBuffer(
        "Metal callback smoke buffer", BufferInfo{32, 1, EBufferUsageFlags::TRANSFER_SRC}
    );
    std::vector<uint8_t> input(32, 0x5a);
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(
        std::span<const Moer::byte>(reinterpret_cast<const Moer::byte*>(input.data()), input.size()),
        buffer->GetView()
    );
    upload.AddCallback([callback_state] {
        callback_state->fetch_add(1);
        CommandList nested(EQueueType::Copy);
        nested.AddSuccessCallback([callback_state] { callback_state->fetch_add(10); });
        RHIExecutor::Get().Submit(EQueueType::Copy, nested.Submit());
    });
    upload.AddSuccessCallback([callback_state] {
        if (callback_state->load() == 1) callback_state->fetch_add(100);
    });
    RHIExecutor::Get().Submit(EQueueType::Copy, upload.Submit());
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (callback_state->load() < 101) {
        throw std::runtime_error("Metal copy Sync returned before the outer callbacks finished");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callback_state->load() != 111 && std::chrono::steady_clock::now() < deadline) {
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (callback_state->load() != 111) {
        throw std::runtime_error("Metal copy completion callbacks did not run in order");
    }
    std::cout << "copy completion callbacks and reentrant submit: success" << std::endl;
}

void Present(Moer::Render::SwapchainRef swapchain, Moer::Render::TextureRef texture) {
    using namespace Moer::Render;
    auto receipt = std::make_shared<PresentReceipt>();
    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Present(
        swapchain, texture->GetView(), receipt
    );
    const auto result = receipt->WaitForSubmission();
    if (!result.resolved || !result.submitted) {
        throw std::runtime_error("Metal Present was rejected");
    }
    RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        GLFWwindow* window = nullptr;
        bool task_system_initialized = false;
        try {
            if (!glfwInit()) throw std::runtime_error("glfwInit failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(640, 480, "MoerEngine Metal RHI smoke", nullptr, nullptr);
            if (window == nullptr) throw std::runtime_error("glfwCreateWindow failed");
            NSWindow* native_window = glfwGetCocoaWindow(window);
            std::cout << "smoke window id: " << native_window.windowNumber << std::endl;

            using namespace Moer::Render;
            Moer::TaskSystem::Init();
            task_system_initialized = true;
            RenderDevice::Init(DeviceInitInfo{.rhi_type = ERHIType::Metal, .name = "MetalRHISmoke"});
            if (RenderDevice::Get().GetShaderPlatform() != EShaderPlatform::SP_METAL_MSL) {
                throw std::runtime_error("Metal device selected the wrong shader platform");
            }
            FenceRef rejected_fence = RenderDevice::Get().CreateFence();
            rejected_fence->Reject(1);
            bool rejected_dependency_failed = false;
            try {
                RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Wait(
                    WaitEvent{uint64_t(rejected_fence.Get()), 1}
                );
            } catch (const std::runtime_error&) {
                rejected_dependency_failed = true;
            }
            if (!rejected_dependency_failed) {
                throw std::runtime_error("Metal graphics queue accepted a rejected dependency");
            }
            std::vector<uint8_t> rgba8(7 * 5 * 4);
            for (size_t i = 0; i < rgba8.size(); ++i) rgba8[i] = static_cast<uint8_t>(i * 17);
            UploadAndCheckTexture(PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED, rgba8, 4);
            std::vector<float> rgba32f(7 * 5 * 4);
            for (size_t i = 0; i < rgba32f.size(); ++i) rgba32f[i] = float(i) / 100.0f;
            UploadAndCheckTexture(
                PF_R32G32B32A32_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rgba32f.data()),
                                         rgba32f.size() * sizeof(float)),
                16
            );
            CheckBufferUpload();
            CheckCopyCompletionCallbacks();
            auto source = std::make_shared<SmokeWindowSource>(window);
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width <= 0 || height <= 0) throw std::runtime_error("window has no drawable");
            SwapchainCreateInfo info{
                .surface = SwapchainSurfaceInfo{source},
                .size = Extent2D(width, height),
                .preferred_format = PF_R8G8B8A8_UNORM,
            };
            SwapchainRef swapchain = RenderDevice::Get().CreateSwapchain(info);
            if (!swapchain || !swapchain->IsPresentationReady()) {
                throw std::runtime_error("Metal swapchain creation failed");
            }
            TextureRef texture = ClearAndCheckTexture(width, height);
            Present(swapchain, texture);
            std::cout << "first present: success" << std::endl;

            glfwSetWindowSize(window, 800, 500);
            glfwPollEvents();
            glfwGetFramebufferSize(window, &width, &height);
            if (width <= 0 || height <= 0) throw std::runtime_error("resize produced no drawable");
            info.size = Extent2D(width, height);
            if (!swapchain->Recreate(info)) throw std::runtime_error("Metal swapchain resize failed");
            texture = ClearAndCheckTexture(width, height);
            Present(swapchain, texture);
            std::cout << "resize present: success" << std::endl;

            const int hold_seconds = argc > 1 ? std::max(0, std::atoi(argv[1])) : 2;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
            while (std::chrono::steady_clock::now() < until && !glfwWindowShouldClose(window)) {
                glfwPollEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            texture = {};
            swapchain = {};
            source.reset();
            RenderDevice::Dispose();
            Moer::TaskSystem::ShutDown();
            task_system_initialized = false;
            glfwDestroyWindow(window);
            glfwTerminate();
            std::cout << "Metal RHI smoke passed" << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Metal RHI smoke failed: " << error.what() << std::endl;
            if (Moer::Render::RenderDevice::IsInitialized()) Moer::Render::RenderDevice::Dispose();
            if (task_system_initialized) Moer::TaskSystem::ShutDown();
            if (window != nullptr) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }
}
