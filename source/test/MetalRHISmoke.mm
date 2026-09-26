#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/RHI.h"
#include "rhi/metal/MetalDevice.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

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

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = native;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.4, 0.75, 1.0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        throw std::runtime_error("Metal clear did not complete");
    }

    const NSUInteger row_bytes = ((static_cast<NSUInteger>(width) * 4 + 255) / 256) * 256;
    id<MTLBuffer> readback = [native.device newBufferWithLength:row_bytes * height
                                                       options:MTLResourceStorageModeShared];
    command = [queue commandBuffer];
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
        try {
            if (!glfwInit()) throw std::runtime_error("glfwInit failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(640, 480, "MoerEngine Metal RHI smoke", nullptr, nullptr);
            if (window == nullptr) throw std::runtime_error("glfwCreateWindow failed");
            NSWindow* native_window = glfwGetCocoaWindow(window);
            std::cout << "smoke window id: " << native_window.windowNumber << std::endl;

            using namespace Moer::Render;
            RenderDevice::Init(DeviceInitInfo{.rhi_type = ERHIType::Metal, .name = "MetalRHISmoke"});
            if (RenderDevice::Get().GetShaderPlatform() != EShaderPlatform::SP_METAL_MSL) {
                throw std::runtime_error("Metal device selected the wrong shader platform");
            }
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
            glfwDestroyWindow(window);
            glfwTerminate();
            std::cout << "Metal RHI smoke passed" << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Metal RHI smoke failed: " << error.what() << std::endl;
            if (Moer::Render::RenderDevice::IsInitialized()) Moer::Render::RenderDevice::Dispose();
            if (window != nullptr) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }
}
