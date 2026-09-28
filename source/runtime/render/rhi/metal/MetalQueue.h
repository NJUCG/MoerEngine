#pragma once

#include "rhi/metal/MetalDevice.h"
#import <Metal/Metal.h>

#include <memory>

namespace Moer::Render {
using MetalGraphicsQueuePtr = std::unique_ptr<CommandQueue, void (*)(CommandQueue*)>;
using MetalCopyQueuePtr = std::unique_ptr<CopyQueue, void (*)(CopyQueue*)>;

MetalGraphicsQueuePtr CreateMetalGraphicsQueue(id<MTLCommandQueue> queue);
MetalCopyQueuePtr CreateMetalCopyQueue(id<MTLCommandQueue> queue);
} // namespace Moer::Render
