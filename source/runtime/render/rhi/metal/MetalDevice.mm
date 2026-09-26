#include "taskgraph/Event.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "rhi/metal/MetalDevice.h"
#include "log/LogSystem.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Moer::Render {
namespace {

[[noreturn]] void Unsupported(const char* operation) {
    throw std::runtime_error(std::string("Metal RHI has not implemented ") + operation);
}

MTLPixelFormat ToMetalFormat(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return MTLPixelFormatR8Unorm;
        case PF_R8G8B8A8_UNORM:
            return MTLPixelFormatRGBA8Unorm;
        case PF_R8G8B8A8_SRGB:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case PF_B8G8R8A8_UNORM:
            return MTLPixelFormatBGRA8Unorm;
        case PF_B8G8R8A8_SRGB:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case PF_A2R10G10B10_UNORM_PACK32:
            return MTLPixelFormatBGR10A2Unorm;
        case PF_R16G16B16A16_SFLOAT:
            return MTLPixelFormatRGBA16Float;
        case PF_R32G32B32A32_SFLOAT:
            return MTLPixelFormatRGBA32Float;
        case PF_D16_UNORM:
            return MTLPixelFormatDepth16Unorm;
        case PF_D32_SFLOAT:
            return MTLPixelFormatDepth32Float;
        case PF_D32_SFLOAT_S8_UINT:
            return MTLPixelFormatDepth32Float_Stencil8;
        default:
            Unsupported("this texture format");
    }
}

NSUInteger PixelStride(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return 1;
        case PF_D16_UNORM:
            return 2;
        case PF_R16G16B16A16_SFLOAT:
        case PF_D32_SFLOAT_S8_UINT:
            return 8;
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_SRGB:
        case PF_B8G8R8A8_UNORM:
        case PF_B8G8R8A8_SRGB:
        case PF_A2R10G10B10_UNORM_PACK32:
        case PF_D32_SFLOAT:
            return 4;
        case PF_R32G32B32A32_SFLOAT:
            return 16;
        default:
            Unsupported("this texture format");
    }
}

MTLVertexFormat ToMetalVertexFormat(EPixelFormat format) {
    switch (format) {
        case PF_R32_SFLOAT: return MTLVertexFormatFloat;
        case PF_R32G32_SFLOAT: return MTLVertexFormatFloat2;
        case PF_R32G32B32_SFLOAT: return MTLVertexFormatFloat3;
        case PF_R32G32B32A32_SFLOAT: return MTLVertexFormatFloat4;
        case PF_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
        default: Unsupported("this vertex attribute format");
    }
}

MTLBlendOperation ToMetalBlendOperation(EBlendOperation operation) {
    switch (operation) {
        case BO_ADD: return MTLBlendOperationAdd;
        case BO_SUBTRACT: return MTLBlendOperationSubtract;
        case BO_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case BO_MIN: return MTLBlendOperationMin;
        case BO_MAX: return MTLBlendOperationMax;
        default: Unsupported("this blend operation");
    }
}

MTLBlendFactor ToMetalBlendFactor(EBlendFactor factor) {
    switch (factor) {
        case BF_ZERO: return MTLBlendFactorZero;
        case BF_ONE: return MTLBlendFactorOne;
        case BF_SRC_COLOR: return MTLBlendFactorSourceColor;
        case BF_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case BF_DST_COLOR: return MTLBlendFactorDestinationColor;
        case BF_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case BF_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case BF_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case BF_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case BF_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case BF_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
        case BF_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
        case BF_SRC1_COLOR: return MTLBlendFactorSource1Color;
        case BF_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
        case BF_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
        case BF_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
        default: Unsupported("this blend factor");
    }
}

// The bootstrap queues wait for each command buffer to finish. This host
// timeline still distinguishes submitted, completed, and rejected values so
// callers cannot mistake a rejected dependency for completed GPU work.
class MetalFence final : public Fence {
public:
    uint64 GetValue() const override {
        std::lock_guard lock(mutex_);
        return completed_;
    }

    void Wait(uint64 value) override {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return completed_ >= value || IsRejectedLocked(value); });
    }

    void MarkSubmitted(uint64 value) override {
        {
            std::lock_guard lock(mutex_);
            submitted_ = std::max(submitted_, value);
        }
        cv_.notify_all();
    }

    bool WaitSubmitted(
        uint64 value, const std::atomic_bool* continue_waiting, EQueueType, uint32
    ) override {
        std::unique_lock lock(mutex_);
        while (submitted_ < value && !IsRejectedLocked(value) &&
               (continue_waiting == nullptr || continue_waiting->load(std::memory_order_acquire))) {
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        }
        return submitted_ >= value && !IsRejectedLocked(value);
    }

    void Reject(uint64 value) noexcept override {
        {
            std::lock_guard lock(mutex_);
            try {
                rejected_.insert(value);
            } catch (...) {
                reject_all_ = true;
            }
        }
        cv_.notify_all();
    }

    bool IsRejected(uint64 value) const override {
        std::lock_guard lock(mutex_);
        return IsRejectedLocked(value);
    }

    void Complete(uint64 value) {
        {
            std::lock_guard lock(mutex_);
            completed_ = std::max(completed_, value);
        }
        cv_.notify_all();
    }

private:
    bool IsRejectedLocked(uint64 value) const {
        return reject_all_ || rejected_.contains(value);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64 submitted_{0};
    uint64 completed_{0};
    std::unordered_set<uint64> rejected_;
    bool reject_all_{false};
};

class MetalBuffer final : public Buffer {
public:
    MetalBuffer(const BufferInfo& info, id<MTLBuffer> buffer) : Buffer(info), buffer_(buffer) {}

    id<MTLBuffer> Native() const noexcept { return buffer_; }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        buffer_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLBuffer> buffer_;
};

class MetalTexture final : public Texture {
public:
    MetalTexture(const TextureInfo& info, id<MTLTexture> texture) : Texture(info), texture_(texture) {}

    id<MTLTexture> Native() const noexcept { return texture_; }

    uint GetMipByteSize(uint mip) const override {
        if (mip != 0) Unsupported("texture mip sizes");
        return GetWidth() * GetHeight() * PixelStride(GetFormat());
    }

    void SetName(const std::string_view name) override {
        debug_name = std::string(name);
        texture_.label = [NSString stringWithUTF8String:debug_name->c_str()];
    }

private:
    id<MTLTexture> texture_;
};

class MetalPipelineState final : public PipelineState {
public:
    explicit MetalPipelineState(id<MTLRenderPipelineState> pipeline) : pipeline_(pipeline) {}
    id<MTLRenderPipelineState> Native() const noexcept { return pipeline_; }

private:
    id<MTLRenderPipelineState> pipeline_;
};

// The table layout mirrors the SPIRV-Cross MSL argument-buffer sets: set 1
// contains the indirect uint table and buffer addresses, set 2 texture IDs,
// and set 3 sampler IDs. Index zero stays unbound, as on Vulkan.
class MetalBindlessArray final : public BindlessArray {
public:
    static constexpr uint kSamplerCount = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
    static_assert(kSamplerCount <= 256);
    static_assert(sizeof(MTLResourceID) == sizeof(uint64_t));
    static_assert(sizeof(MTLGPUAddress) == sizeof(uint64_t));

    MetalBindlessArray(id<MTLDevice> device, uint capacity) :
        device_(device), capacity_(capacity) {
        if (capacity < 2 || capacity > (1u << 23)) {
            throw std::runtime_error("Metal bindless capacity is outside the shader handle range");
        }
        slots_.resize(capacity);
        const auto new_shared_buffer = [device](NSUInteger size) {
            id<MTLBuffer> buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
            if (buffer == nil) throw std::runtime_error("Cannot allocate Metal bindless table");
            std::memset(buffer.contents, 0, size);
            return buffer;
        };
        indices_ = new_shared_buffer(NSUInteger(capacity) * sizeof(uint32_t));
        buffer_arguments_ = new_shared_buffer(NSUInteger(capacity + 1) * sizeof(uint64_t));
        texture_arguments_ = new_shared_buffer(NSUInteger(capacity) * sizeof(uint64_t));
        sampler_arguments_ = new_shared_buffer(NSUInteger(kSamplerCount) * sizeof(uint64_t));
        const uint64_t index_address = indices_.gpuAddress;
        std::memcpy(buffer_arguments_.contents, &index_address, sizeof(index_address));
        free_slots_.reserve(capacity);
    }

    uint AllocateTexture(const TextureView& view, Sampler sampler) override {
        auto* texture = dynamic_cast<MetalTexture*>(view.GetTexture());
        if (texture == nullptr || view.format != texture->GetFormat() ||
            view.mip_level != 0 || view.num_mips != 1 || view.array_layer != 0 ||
            view.num_array != 1 || view.offset != uint3{0, 0, 0} ||
            view.extent != texture->GetExtent()) {
            Unsupported("this bindless texture view");
        }
        const uint sampler_index = SamplerIndex(sampler);
        EnsureSampler(sampler, sampler_index);
        std::lock_guard lock(mutex_);
        const uint index = NextSlot();
        Slot& slot = slots_[index];
        const uint64 generation = NextGeneration(slot);
        TextureRef reference(view.GetTexture());
        pending_.emplace_back(TextureUpdateInfo{
            reference, sampler, view.format, index, index, view.mip_level, view.num_mips,
            view.array_layer, view.num_array, generation, generation, 0, false
        });
        slot.texture = std::move(reference);
        slot.buffer = {};
        slot.sampler_index = sampler_index;
        slot.kind = Kind::Texture;
        slot.active = true;
        return index;
    }

    uint AllocateBuffer(BufferView view) override {
        auto* buffer = dynamic_cast<MetalBuffer*>(view.GetBuffer());
        if (buffer == nullptr || view.GetByteSize() == 0 ||
            view.GetByteOffset() > buffer->GetByteSize() ||
            view.GetByteSize() > buffer->GetByteSize() - view.GetByteOffset()) {
            Unsupported("this bindless buffer view");
        }
        if (view.format != PF_UNDEFINED) Unsupported("formatted bindless buffers");
        std::lock_guard lock(mutex_);
        const uint index = NextSlot();
        Slot& slot = slots_[index];
        const uint64 generation = NextGeneration(slot);
        BufferRef reference(view.GetBuffer());
        pending_.emplace_back(BufferUpdateInfo{
            reference, index, index, view.format, generation, generation, 0, false
        });
        slot.buffer = std::move(reference);
        slot.texture = {};
        slot.buffer_offset = view.GetByteOffset();
        slot.kind = Kind::Buffer;
        slot.active = true;
        return index;
    }

    void UnbindTexture(uint index) override { Unbind(index, Kind::Texture); }
    void UnbindBuffer(uint index) override { Unbind(index, Kind::Buffer); }
    uint64 ArrayHandle() const override { return reinterpret_cast<uint64>(this); }

    void Apply(const Array<UpdateCmd>& updates) {
        std::lock_guard lock(mutex_);
        auto* indices = static_cast<uint32_t*>(indices_.contents);
        auto* buffers = static_cast<uint64_t*>(buffer_arguments_.contents);
        auto* textures = static_cast<uint64_t*>(texture_arguments_.contents);
        for (const UpdateCmd& update : updates) {
            std::visit([&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                    if (item.array_idx == 0 || item.array_idx >= capacity_) return;
                    Slot& slot = slots_[item.array_idx];
                    if (slot.generation != item.array_generation) return;
                    if (item.free) {
                        if (slot.active || slot.kind == Kind::Empty) return;
                        indices[item.array_idx] = 0;
                        textures[item.array_idx] = 0;
                        buffers[item.array_idx + 1] = 0;
                        slot.texture = {};
                        slot.buffer = {};
                        slot.kind = Kind::Empty;
                        free_slots_.push_back(item.array_idx);
                    } else if (slot.active) {
                        if constexpr (std::is_same_v<T, TextureUpdateInfo>) {
                            textures[item.array_idx] =
                                static_cast<MetalTexture*>(slot.texture.Get())->Native().gpuResourceID._impl;
                            indices[item.array_idx] = (item.array_idx << 8) | slot.sampler_index;
                        } else {
                            buffers[item.array_idx + 1] =
                                static_cast<MetalBuffer*>(slot.buffer.Get())->Native().gpuAddress +
                                slot.buffer_offset;
                            indices[item.array_idx] = item.array_idx;
                        }
                    }
                }
            }, update);
        }
    }

    id<MTLBuffer> NativeIndices() const noexcept { return indices_; }
    id<MTLBuffer> NativeArguments(uint set) const noexcept {
        switch (set) {
            case 1: return buffer_arguments_;
            case 2: return texture_arguments_;
            case 3: return sampler_arguments_;
            default: return nil;
        }
    }

protected:
    UniquePtr<Command> CreateUpdateCommand() override {
        std::lock_guard lock(mutex_);
        Array<UpdateCmd> snapshot = pending_;
        auto command = MakeUnique<UpdateBindlessArrayCmd>(
            this, std::move(snapshot), Array<byte>{}, Array<std::pair<uint, uint>>{},
            Array<byte>{}, Array<std::pair<uint, uint>>{}, Array<byte>{},
            Array<std::pair<uint, uint>>{}
        );
        pending_.clear();
        return command;
    }

    void DiscardUpdateCommand(const Array<UpdateCmd>& updates) override {
        std::lock_guard lock(mutex_);
        Array<UpdateCmd> recovered;
        for (const UpdateCmd& update : updates) {
            std::visit([&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                    if (item.array_idx > 0 && item.array_idx < capacity_) {
                        const Slot& slot = slots_[item.array_idx];
                        if (slot.generation == item.array_generation && slot.active != item.free) {
                            recovered.emplace_back(update);
                        }
                    }
                }
            }, update);
        }
        for (const UpdateCmd& update : pending_) recovered.emplace_back(update);
        pending_ = std::move(recovered);
    }

private:
    enum class Kind { Empty, Texture, Buffer };
    struct Slot {
        TextureRef texture{};
        BufferRef buffer{};
        uint64 generation{0};
        uint64 buffer_offset{0};
        uint sampler_index{0};
        Kind kind{Kind::Empty};
        bool active{false};
    };

    uint NextSlot() {
        if (!free_slots_.empty()) {
            const uint index = free_slots_.back();
            if (slots_[index].generation == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("Metal bindless slot generation exhausted");
            }
            free_slots_.pop_back();
            return index;
        }
        if (next_slot_ >= capacity_) throw std::runtime_error("Metal bindless capacity exhausted");
        if (slots_[next_slot_].generation == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("Metal bindless slot generation exhausted");
        }
        return next_slot_++;
    }

    static uint64 NextGeneration(Slot& slot) {
        if (slot.generation == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("Metal bindless slot generation exhausted");
        }
        return ++slot.generation;
    }

    static uint SamplerIndex(Sampler sampler) {
        if (sampler.filter >= SF_Num || sampler.address_mode >= SAM_Num ||
            sampler.compare_function >= SCF_Num) {
            Unsupported("this bindless sampler");
        }
        return (uint(SF_Num) * uint(SAM_Num)) * uint(sampler.compare_function) +
               uint(SF_Num) * uint(sampler.address_mode) + uint(sampler.filter);
    }

    void EnsureSampler(Sampler sampler, uint index) {
        std::lock_guard lock(mutex_);
        if (samplers_[index] != nil) return;
        MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
        const bool nearest = sampler.filter == SF_NEAREST || sampler.filter == SF_ANISOTROPIC_NEAREST;
        desc.minFilter = nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        desc.magFilter = nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        desc.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        desc.maxAnisotropy = sampler.filter == SF_ANISOTROPIC_NEAREST ||
                             sampler.filter == SF_ANISOTROPIC_LINEAR ? 16 : 1;
        MTLSamplerAddressMode address = MTLSamplerAddressModeRepeat;
        switch (sampler.address_mode) {
            case SAM_REPEAT: break;
            case SAM_MIRRORED_REPEAT: address = MTLSamplerAddressModeMirrorRepeat; break;
            case SAM_CLAMP_TO_EDGE: address = MTLSamplerAddressModeClampToEdge; break;
            case SAM_CLAMP_TO_BORDER: address = MTLSamplerAddressModeClampToBorderColor; break;
            default: Unsupported("this bindless sampler address mode");
        }
        desc.sAddressMode = address;
        desc.tAddressMode = address;
        desc.rAddressMode = address;
        desc.supportArgumentBuffers = YES;
        if (sampler.compare_function != SCF_NEVER) {
            static constexpr MTLCompareFunction kCompare[] = {
                MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
                MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual,
                MTLCompareFunctionAlways
            };
            desc.compareFunction = kCompare[sampler.compare_function];
        }
        id<MTLSamplerState> native = [device_ newSamplerStateWithDescriptor:desc];
        if (native == nil) throw std::runtime_error("Cannot create Metal bindless sampler");
        samplers_[index] = native;
        static_cast<uint64_t*>(sampler_arguments_.contents)[index] = native.gpuResourceID._impl;
    }

    void Unbind(uint index, Kind kind) {
        std::lock_guard lock(mutex_);
        if (index == 0 || index >= capacity_ || !slots_[index].active ||
            slots_[index].kind != kind) {
            throw std::runtime_error("Metal bindless handle is not allocated with this resource type");
        }
        Slot& slot = slots_[index];
        if (kind == Kind::Texture) {
            pending_.emplace_back(TextureUpdateInfo{
                slot.texture, Sampler(SF_NEAREST, SAM_REPEAT), slot.texture->GetFormat(),
                index, index, 0, 1, 0, 1, slot.generation, slot.generation, 0, true
            });
        } else {
            pending_.emplace_back(BufferUpdateInfo{
                slot.buffer, index, index, PF_UNDEFINED,
                slot.generation, slot.generation, 0, true
            });
        }
        slot.active = false;
    }

    id<MTLDevice> device_;
    id<MTLBuffer> indices_;
    id<MTLBuffer> buffer_arguments_;
    id<MTLBuffer> texture_arguments_;
    id<MTLBuffer> sampler_arguments_;
    id<MTLSamplerState> samplers_[kSamplerCount]{};
    std::vector<Slot> slots_;
    std::vector<uint> free_slots_;
    Array<UpdateCmd> pending_;
    const uint capacity_;
    uint next_slot_{1};
    std::mutex mutex_;
};

void ValidateQueueTransfer(const QueueTransferCmd& transfer, EQueueType current_queue) {
    const EQueueType other_queue = transfer.IsImport() ? transfer.src_queue : transfer.dst_queue;
    if ((current_queue != EQueueType::Graphics && current_queue != EQueueType::Copy) ||
        (other_queue != EQueueType::Graphics && other_queue != EQueueType::Copy) ||
        current_queue == other_queue) {
        Unsupported("this queue transfer");
    }
    const auto validate_texture = [](TextureView view) {
        if (dynamic_cast<MetalTexture*>(view.GetTexture()) == nullptr) {
            Unsupported("a foreign texture in queue transfer");
        }
    };
    const auto validate_buffer = [](BufferView view) {
        if (dynamic_cast<MetalBuffer*>(view.GetBuffer()) == nullptr) {
            Unsupported("a foreign buffer in queue transfer");
        }
    };
    for (const auto& item : transfer.ImportTextures()) validate_texture(item.texture);
    for (const auto& item : transfer.ExportTextures()) validate_texture(item.texture);
    for (const auto& item : transfer.ImportBuffers()) validate_buffer(item.buffer);
    for (const auto& item : transfer.ExportBuffers()) validate_buffer(item.buffer);
}

// Completion callbacks may submit more RHI work. Run them after Execute has
// returned from the executor publication gate, never on that gate's owner.
class MetalCompletionDispatcher final {
public:
    struct Packet {
        uint64 timeline{0};
        Array<std::function<void()>> callbacks;
        Array<std::function<void()>> success_callbacks;
    };
    static_assert(std::is_nothrow_move_assignable_v<Array<std::function<void()>>>);

    MetalCompletionDispatcher() : worker_([this] { Run(); }) {}

    ~MetalCompletionDispatcher() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    void Enqueue(std::list<Packet>& prepared) noexcept {
        {
            std::lock_guard lock(mutex_);
            pending_.splice(pending_.end(), prepared);
        }
        cv_.notify_all();
    }

    void WaitThrough(uint64 timeline) {
        if (std::this_thread::get_id() == worker_.get_id()) {
            throw std::runtime_error("Metal completion callback cannot wait for copy callbacks");
        }
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return finished_timeline_ >= timeline; });
    }

private:
    void Run() {
        RHIThreadRoleScope role(ERHIThreadRole::Completion);
        for (;;) {
            std::list<Packet> ready;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (pending_.empty() && stopping_) return;
                ready.splice(ready.end(), pending_, pending_.begin());
            }
            Packet& packet = ready.front();
            auto invoke = [](Array<std::function<void()>>& callbacks) {
                for (auto& callback : callbacks) {
                    try {
                        if (callback) callback();
                    } catch (const std::exception& error) {
                        try { LOG_ERROR("[Metal] completion callback failed: {}", error.what()); } catch (...) {}
                    } catch (...) {
                        try { LOG_ERROR("[Metal] completion callback failed"); } catch (...) {}
                    }
                }
            };
            invoke(packet.callbacks);
            invoke(packet.success_callbacks);
            const uint64 timeline = packet.timeline;
            ready.clear();
            {
                std::lock_guard lock(mutex_);
                finished_timeline_ = timeline;
            }
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::list<Packet> pending_;
    uint64 finished_timeline_{0};
    bool stopping_{false};
    std::thread worker_;
};

class MetalSwapchain final : public Swapchain {
public:
    MetalSwapchain(id<MTLDevice> device, const SwapchainCreateInfo& info) : device_(device) {
        Recreate(info);
    }

    ~MetalSwapchain() override { Detach(); }

    bool Recreate(const SwapchainCreateInfo& info) override {
        if (!info.surface.IsValid() || info.size.x == 0 || info.size.y == 0 ||
            ![NSThread isMainThread]) {
            return false;
        }
        const WindowNativeHandle handle = info.surface.source->GetNativeWindow();
        if (handle.window_system != EWindowSystemType::GLFW) return false;
        auto* glfw_window = reinterpret_cast<GLFWwindow*>(handle.window_system_handle);
        NSWindow* window = glfwGetCocoaWindow(glfw_window);
        NSView* view = window.contentView;
        if (view == nil) return false;

        const MTLPixelFormat pixel_format = ToMetalFormat(info.preferred_format);
        const WindowSurfaceIdentity identity = info.surface.GetIdentity();
        if (view_ != view || identity_ != identity || info.force_surface_recreate) {
            Detach();
            view_ = view;
            previous_layer_ = view.layer;
            previous_wants_layer_ = view.wantsLayer;
            layer_ = [CAMetalLayer layer];
            layer_.device = device_;
            layer_.framebufferOnly = NO; // Present copies the RHI framebuffer to the drawable.
            view.wantsLayer = YES;
            view.layer = layer_;
        }
        layer_.pixelFormat = pixel_format;
        layer_.frame = view.bounds;
        layer_.contentsScale = window.backingScaleFactor;
        layer_.drawableSize = CGSizeMake(info.size.x, info.size.y);
        source_ = info.surface.source;
        identity_ = identity;
        format = info.preferred_format;
        size = info.size;
        return true;
    }

    bool IsPresentationReady() const noexcept override { return layer_ != nil && source_ != nullptr; }
    WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override { return identity_; }

    id<CAMetalDrawable> NextDrawable() const { return [layer_ nextDrawable]; }

private:
    void Detach() {
        if (view_ != nil && view_.layer == layer_) {
            view_.layer = previous_layer_;
            view_.wantsLayer = previous_wants_layer_;
        }
        layer_ = nil;
        view_ = nil;
        previous_layer_ = nil;
        source_.reset();
        identity_ = {};
    }

    id<MTLDevice> device_;
    CAMetalLayer* layer_{nil};
    NSView* view_{nil};
    CALayer* previous_layer_{nil};
    bool previous_wants_layer_{false};
    WindowSurfaceSourceRef source_{};
    WindowSurfaceIdentity identity_{};
};

class MetalCommandQueue final : public CommandQueue {
public:
    explicit MetalCommandQueue(id<MTLCommandQueue> queue) : queue_(queue) {}

    void Wait(WaitEvent event) override {
        auto* fence = dynamic_cast<MetalFence*>(reinterpret_cast<Fence*>(event.timeline_handle));
        const std::atomic_bool do_not_wait{false};
        if (fence == nullptr ||
            !fence->WaitSubmitted(event.value, &do_not_wait, EQueueType::Graphics, 1)) {
            throw std::runtime_error("Metal graphics dependency was not submitted or was rejected");
        }
        fence->Wait(event.value);
        if (fence->IsRejected(event.value)) {
            throw std::runtime_error("Metal graphics dependency was rejected");
        }
    }
    WaitEvent Execute(CmdSubmit&& submit) override {
        if (!submit.signal_events.empty() || !submit.callbacks.empty() ||
            !submit.success_callbacks.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("submission synchronization, callbacks, or profiling");
        }
        // Validate the complete submit before encoding any GPU work. A later
        // unsupported command must not leave a partially executed submit.
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            if (command->Type() == Command::EType::QueueTransfer) {
                ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Graphics);
                continue;
            }
            if (command->Type() == Command::EType::UpdateBindlessArray) {
                const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
                if (dynamic_cast<MetalBindlessArray*>(update.Handle()) == nullptr) {
                    Unsupported("a foreign bindless array");
                }
                continue;
            }
            if (command->Type() != Command::EType::ClearResource) {
                Unsupported("this graphics command");
            }
            has_gpu_work = true;
            const auto& clear = static_cast<const ClearResourceCmd&>(*command);
            if (!clear.IsTexture() || !clear.IsFloat4()) {
                Unsupported("this clear value or resource");
            }
            const TextureView& view = clear.Texture();
            auto* texture = dynamic_cast<MetalTexture*>(view.GetTexture());
            if (texture == nullptr || view.format != texture->GetFormat() ||
                view.mip_level != 0 || view.num_mips != 1 ||
                view.array_layer != 0 || view.num_array != 1 ||
                view.offset != uint3{0, 0, 0} || view.extent != texture->GetExtent()) {
                Unsupported("this texture clear view");
            }
        }
        for (const WaitEvent& event : submit.wait_events) Wait(event);
        for (const auto& command : submit.cmds) {
            if (command->Type() != Command::EType::UpdateBindlessArray) continue;
            const auto& update = static_cast<const UpdateBindlessArrayCmd&>(*command);
            if (update.HandOffUpdates()) {
                static_cast<MetalBindlessArray*>(update.Handle())->Apply(update.UpdateCommands());
            }
        }
        if (!has_gpu_work) return {0, 0};

        @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            if (command_buffer == nil) throw std::runtime_error("Cannot create Metal command buffer");
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::QueueTransfer ||
                    command->Type() == Command::EType::UpdateBindlessArray) continue;
                const auto& clear = static_cast<const ClearResourceCmd&>(*command);
                auto* texture = static_cast<MetalTexture*>(clear.Texture().GetTexture());
                const float4 color = clear.Float4Value();
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = texture->Native();
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(
                    color.x, color.y, color.z, color.w
                );
                id<MTLRenderCommandEncoder> encoder =
                    [command_buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) throw std::runtime_error("Cannot encode Metal texture clear");
                [encoder endEncoding];
            }
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal texture clear failed: " +
                    std::string([command_buffer.error.localizedDescription UTF8String] ?: "unknown GPU error"));
            }
        }
        return {0, 0};
    }

    void Present(SwapchainRef swapchain, TextureView source, PresentReceiptRef receipt) override {
        auto* target = dynamic_cast<MetalSwapchain*>(swapchain.Get());
        auto* texture = dynamic_cast<MetalTexture*>(source.texture);
        if (target == nullptr || texture == nullptr || !target->IsPresentationReady()) {
            if (receipt) receipt->Resolve(false);
            return;
        }
        @autoreleasepool {
            id<CAMetalDrawable> drawable = target->NextDrawable();
            id<MTLTexture> source_texture = texture->Native();
            if (drawable == nil || source_texture.pixelFormat != drawable.texture.pixelFormat ||
                source_texture.width != drawable.texture.width ||
                source_texture.height != drawable.texture.height) {
                if (receipt) receipt->Resolve(false, true);
                return;
            }
            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) {
                if (receipt) receipt->Resolve(false);
                return;
            }
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (blit == nil) {
                if (receipt) receipt->Resolve(false);
                return;
            }
            [blit copyFromTexture:source_texture
                     sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(source_texture.width, source_texture.height, 1)
                       toTexture:drawable.texture
                destinationSlice:0 destinationLevel:0
               destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [command presentDrawable:drawable];
            [command commit];
            if (receipt) receipt->Resolve(true);
        }
    }

    void Sync() override {
        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        [command commit];
        [command waitUntilCompleted];
    }

    ProfileData GetProfilerEntry() override { return {}; }

private:
    id<MTLCommandQueue> queue_;
};

class MetalCopyQueue final : public CopyQueue {
public:
    MetalCopyQueue(id<MTLCommandQueue> queue, MetalCompletionDispatcher& completions) :
        queue_(queue), completions_(completions), timeline_fence_(MoerNew(MetalFence)()) {}

    IOWaitEvt Execute(IOQueueSubmission&&) override { Unsupported("IO queue submission"); }
    IOWaitEvt Execute(CmdSubmit&& submit) override {
        std::lock_guard lock(mutex_);
        if (!submit.wait_events.empty() || !submit.signal_events.empty() ||
            !submit.gpu_completion_tokens.empty() || !submit.query_tokens.empty() ||
            submit.b_tick_profiling || submit.profiling_phase != ERHIProfilingPhase::Disabled ||
            submit.b_delete_resources) {
            Unsupported("copy submission synchronization or profiling");
        }
        bool has_gpu_work = false;
        for (const auto& command : submit.cmds) {
            switch (command->Type()) {
                case Command::EType::QueueTransfer:
                    ValidateQueueTransfer(static_cast<const QueueTransferCmd&>(*command), EQueueType::Copy);
                    break;
                case Command::EType::UploadTexture: {
                    has_gpu_work = true;
                    const auto& upload = static_cast<const UploadTextureCmd&>(*command);
                    auto* texture = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
                    const uint3 size = upload.Size();
                    if (texture == nullptr || upload.Format() != texture->GetFormat() ||
                        upload.MipLevel() != 0 || upload.ArrayLayer() != 0 ||
                        upload.Offset() != uint3{0, 0, 0} || size != texture->GetExtent() ||
                        upload.Data().size_bytes() != size_t(size.x) * size.y * PixelStride(upload.Format())) {
                        Unsupported("this texture upload layout");
                    }
                    break;
                }
                case Command::EType::UploadBuffer: {
                    has_gpu_work = true;
                    const auto& upload = static_cast<const UploadBufferCmd&>(*command);
                    auto* buffer = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
                    if (buffer == nullptr || upload.ByteSize() == 0 ||
                        upload.Offset() > buffer->GetByteSize() ||
                        upload.ByteSize() > buffer->GetByteSize() - upload.Offset() ||
                        upload.Data().size_bytes() != upload.ByteSize()) {
                        Unsupported("this buffer upload layout");
                    }
                    break;
                }
                case Command::EType::BufferToBuffer: {
                    has_gpu_work = true;
                    const auto& copy = static_cast<const CopyBufferCmd&>(*command);
                    auto* source = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.SrcHandle()));
                    auto* destination = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.DstHandle()));
                    if (source == nullptr || destination == nullptr || copy.ByteSize() == 0 ||
                        copy.SrcOffset() > source->GetByteSize() ||
                        copy.ByteSize() > source->GetByteSize() - copy.SrcOffset() ||
                        copy.DstOffset() > destination->GetByteSize() ||
                        copy.ByteSize() > destination->GetByteSize() - copy.DstOffset()) {
                        Unsupported("this buffer copy layout");
                    }
                    break;
                }
                case Command::EType::TextureToTexture: {
                    has_gpu_work = true;
                    const auto& copy = static_cast<const CopyTextureCmd&>(*command);
                    auto* source = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.SrcHandle()));
                    auto* destination = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.DstHandle()));
                    if (source == nullptr || destination == nullptr ||
                        copy.Format() != source->GetFormat() || copy.Format() != destination->GetFormat() ||
                        copy.SrcMipLevel() != 0 || copy.DstMipLevel() != 0 ||
                        copy.SrcOffset() != uint3{0, 0, 0} || copy.DstOffset() != uint3{0, 0, 0} ||
                        copy.Size() != source->GetExtent() || copy.Size() != destination->GetExtent()) {
                        Unsupported("this texture copy layout");
                    }
                    break;
                }
                default:
                    Unsupported("this copy command");
            }
        }
        std::list<MetalCompletionDispatcher::Packet> prepared;
        prepared.emplace_back(); // Allocate the completion node before GPU work is accepted.

        if (has_gpu_work) @autoreleasepool {
            id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
            if (blit == nil) throw std::runtime_error("Cannot encode Metal copy commands");
            NSMutableArray<id<MTLBuffer>>* staging_buffers = [NSMutableArray array];
            for (const auto& command : submit.cmds) {
                if (command->Type() == Command::EType::QueueTransfer) continue;
                if (command->Type() == Command::EType::BufferToBuffer) {
                    const auto& copy = static_cast<const CopyBufferCmd&>(*command);
                    auto* source = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.SrcHandle()));
                    auto* destination = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(copy.DstHandle()));
                    [blit copyFromBuffer:source->Native() sourceOffset:copy.SrcOffset()
                               toBuffer:destination->Native() destinationOffset:copy.DstOffset()
                                   size:copy.ByteSize()];
                    continue;
                }
                if (command->Type() == Command::EType::TextureToTexture) {
                    const auto& copy = static_cast<const CopyTextureCmd&>(*command);
                    auto* source = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.SrcHandle()));
                    auto* destination = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(copy.DstHandle()));
                    [blit copyFromTexture:source->Native() sourceSlice:0 sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(copy.Size().x, copy.Size().y, 1)
                               toTexture:destination->Native()
                        destinationSlice:0 destinationLevel:0
                       destinationOrigin:MTLOriginMake(0, 0, 0)];
                    continue;
                }
                if (command->Type() == Command::EType::UploadBuffer) {
                    const auto& upload = static_cast<const UploadBufferCmd&>(*command);
                    auto* buffer = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
                    id<MTLBuffer> staging = [queue_.device
                        newBufferWithBytes:upload.Data().data()
                                   length:upload.ByteSize()
                                  options:MTLResourceStorageModeShared];
                    if (staging == nil) throw std::runtime_error("Cannot allocate Metal buffer staging");
                    [staging_buffers addObject:staging];
                    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer->Native()
                      destinationOffset:upload.Offset() size:upload.ByteSize()];
                    continue;
                }
                const auto& upload = static_cast<const UploadTextureCmd&>(*command);
                auto* texture = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
                const NSUInteger width = upload.Size().x;
                const NSUInteger height = upload.Size().y;
                const NSUInteger source_row_bytes = width * PixelStride(upload.Format());
                const NSUInteger staging_row_bytes = (source_row_bytes + 255) & ~NSUInteger(255);
                id<MTLBuffer> staging = [queue_.device
                    newBufferWithLength:staging_row_bytes * height
                                options:MTLResourceStorageModeShared];
                if (staging == nil) throw std::runtime_error("Cannot allocate Metal upload staging buffer");
                auto* destination = static_cast<uint8_t*>(staging.contents);
                const auto* source = reinterpret_cast<const uint8_t*>(upload.Data().data());
                for (NSUInteger row = 0; row < height; ++row) {
                    std::memcpy(destination + row * staging_row_bytes,
                                source + row * source_row_bytes, source_row_bytes);
                }
                [staging_buffers addObject:staging];
                [blit copyFromBuffer:staging sourceOffset:0
                  sourceBytesPerRow:staging_row_bytes
                sourceBytesPerImage:staging_row_bytes * height
                         sourceSize:MTLSizeMake(width, height, 1)
                          toTexture:texture->Native() destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
            [blit endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal copy submission failed: " +
                    std::string([command_buffer.error.localizedDescription UTF8String] ?: "unknown GPU error"));
            }
        }
        const uint64 timeline = ++next_timeline_;
        timeline_fence_->MarkSubmitted(timeline);
        static_cast<MetalFence*>(timeline_fence_.Get())->Complete(timeline);
        prepared.front().timeline = timeline;
        prepared.front().callbacks = std::move(submit.callbacks);
        prepared.front().success_callbacks = std::move(submit.success_callbacks);
        completions_.Enqueue(prepared);
        return {uint64(timeline_fence_.Get()), timeline};
    }
    void CopyFrom(BufferView, BufferView) override { Unsupported("buffer copy"); }
    void CopyFrom(TextureView, TextureView) override { Unsupported("texture copy"); }
    void CopyFrom(TextureView, BufferView) override { Unsupported("texture readback"); }
    void CopyFrom(BufferView, TextureView) override { Unsupported("texture upload"); }
    void CopyFrom(std::span<byte>, BufferView) override { Unsupported("buffer upload"); }
    void CopyFrom(std::span<byte>, TextureView) override { Unsupported("texture upload"); }
    FenceRef GetFenceHandle() override { return timeline_fence_; }
    void Sync(uint64 timeline) override {
        timeline_fence_->Wait(timeline);
        if (timeline_fence_->IsRejected(timeline)) {
            throw std::runtime_error("Metal copy timeline was rejected");
        }
        completions_.WaitThrough(timeline);
    }

private:
    id<MTLCommandQueue> queue_;
    std::mutex mutex_;
    MetalCompletionDispatcher& completions_;
    FenceRef timeline_fence_;
    uint64 next_timeline_{0};
};

} // namespace

struct MetalDevice::Native {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    MetalCompletionDispatcher completions;
    MetalCommandQueue graphics;
    MetalCopyQueue copy;

    Native(id<MTLDevice> metal_device, id<MTLCommandQueue> metal_queue) :
        device(metal_device), queue(metal_queue), graphics(metal_queue), copy(metal_queue, completions) {}
};

MetalDevice::MetalDevice() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) throw std::runtime_error("No Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) throw std::runtime_error("Cannot create Metal command queue");
    native_ = std::make_unique<Native>(device, queue);
    LOG_INFO("[Metal] Device initialized: {}", [device.name UTF8String]);
}

MetalDevice::~MetalDevice() = default;

FenceRef MetalDevice::CreateFence() { return FenceRef(MoerNew(MetalFence)()); }
BufferRef MetalDevice::CreateBuffer(
    std::string_view name, uint count, uint stride, EBufferUsageFlags usage, EPixelFormat format
) {
    if (count == 0 || stride == 0) Unsupported("zero-sized buffers");
    const BufferInfo info(count, stride, usage, format);
    const MTLResourceOptions options =
        (usage & EBufferUsageFlags::CPU_VISIBLE) != EBufferUsageFlags::NONE ?
            MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
    id<MTLBuffer> buffer = [native_->device newBufferWithLength:info.size * info.stride
                                                       options:options];
    if (buffer == nil) throw std::runtime_error("Cannot allocate Metal buffer");
    auto result = BufferRef(MoerNew(MetalBuffer)(info, buffer));
    result->SetName(name);
    return result;
}
TextureRef MetalDevice::CreateTexture(std::string_view name, const TextureInfo& info) {
    if (info.dimension != ETextureDimension::TEX_2D || info.array_size != 1 ||
        info.num_mips != 1 || info.num_samples != 1 || info.extent.x <= 0 || info.extent.y <= 0) {
        Unsupported("this texture layout");
    }
    const MTLPixelFormat native_format = ToMetalFormat(info.format);
    const bool is_depth = info.format == PF_D16_UNORM || info.format == PF_D32_SFLOAT ||
                          info.format == PF_D32_SFLOAT_S8_UINT;
    if ((is_depth && (info.usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (!is_depth && (info.usage & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) ||
        (is_depth && (info.usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED)) {
        Unsupported("this texture format and usage combination");
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:native_format
                                 width:info.extent.x height:info.extent.y mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead;
    if ((info.usage & ETextureUsageFlags::COLOR_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageRenderTarget;
    }
    if ((info.usage & ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageRenderTarget;
    }
    if ((info.usage & ETextureUsageFlags::UNORDERED_ACCESS) != ETextureUsageFlags::UNDEFINED) {
        desc.usage |= MTLTextureUsageShaderWrite;
    }
    id<MTLTexture> texture = [native_->device newTextureWithDescriptor:desc];
    if (texture == nil) throw std::runtime_error("Cannot allocate Metal texture");
    auto result = TextureRef(MoerNew(MetalTexture)(info, texture));
    result->SetName(name);
    return result;
}
BindlessArrayRef MetalDevice::CreateBindlessArray(uint capacity) {
    return BindlessArrayRef(MoerNew(MetalBindlessArray)(native_->device, capacity));
}
RaytracingGeometryRef MetalDevice::CreateRaytracingGeometry(const RaytracingGeometryInfo&) {
    Unsupported("raytracing geometry");
}
RaytracingSceneRef MetalDevice::CreateRaytracingScene() { Unsupported("raytracing scene"); }
CommandQueue& MetalDevice::GetCommandQueue(EQueueType type) {
    if (type != EQueueType::Graphics) Unsupported("this command queue");
    return native_->graphics;
}
CopyQueue& MetalDevice::GetCopyQueue() { return native_->copy; }
RHIQueueTopology MetalDevice::GetQueueTopology() const {
    RHIQueueTopology topology{};
    topology.compute.available = false;
    return topology;
}
SwapchainRef MetalDevice::CreateSwapchain(const SwapchainCreateInfo& info) {
    auto result = SwapchainRef(MoerNew(MetalSwapchain)(native_->device, info));
    return result->IsPresentationReady() ? result : SwapchainRef{};
}
PipelineHandle MetalDevice::CreatePipeline(GfxPsoCreateInfo&& create_info, PipelineShaderInfo&& shader_info) {
    if (!std::holds_alternative<ShaderVsPs>(shader_info.shader_group) ||
        create_info.primitive_topology != EPrimitiveTopology::TRIANGLE_LIST ||
        create_info.view_mask != 0 || create_info.multi_view_count != 1 ||
        create_info.multisample_info.sample_count != 1 ||
        create_info.color_attachment_count > 8 ||
        shader_info.layout_hash.size() != shader_info.arg_cpp_info.size() ||
        shader_info.layout_hash.size() > 64) {
        Unsupported("this graphics pipeline layout");
    }
    const auto& shaders = std::get<ShaderVsPs>(shader_info.shader_group);
    const auto compile_function = [this](const SingleShaderInfo& shader) -> id<MTLFunction> {
        if (shader.shader_data.empty() || shader.entry_point.empty()) {
            throw std::runtime_error("Metal pipeline shader source or entry point is empty");
        }
        NSString* source = [[NSString alloc]
            initWithBytes:shader.shader_data.data()
                   length:shader.shader_data.size()
                 encoding:NSUTF8StringEncoding];
        if (source == nil) throw std::runtime_error("Metal pipeline shader source is not UTF-8 MSL");
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        id<MTLLibrary> library = [native_->device newLibraryWithSource:source options:options error:&error];
        if (library == nil) {
            throw std::runtime_error("Metal shader compilation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        NSString* entry = [[NSString alloc] initWithBytes:shader.entry_point.data()
                                                  length:shader.entry_point.size()
                                                encoding:NSUTF8StringEncoding];
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (function == nil) {
            throw std::runtime_error("Metal shader entry point was not found: " +
                std::string(shader.entry_point));
        }
        return function;
    };
    @autoreleasepool {
        id<MTLFunction> vertex = compile_function(shaders.vs);
        id<MTLFunction> fragment = compile_function(shaders.ps);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.rasterSampleCount = 1;
        descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;

        MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
        uint attribute_index = 0;
        for (uint binding_index = 0; binding_index < create_info.vertex_stream.bindings.size(); ++binding_index) {
            const VertexBinding& binding = create_info.vertex_stream.bindings[binding_index];
            const uint metal_buffer_index = 16 + binding_index; // Reserve low slots for MSL resource sets.
            if (metal_buffer_index >= 31) Unsupported("too many vertex streams");
            NSUInteger stride = 0;
            for (const VertexElement& element : binding.vertex_elements) {
                if (attribute_index >= 31) Unsupported("too many vertex attributes");
                auto* attribute = vertex_descriptor.attributes[attribute_index++];
                attribute.format = ToMetalVertexFormat(element.format);
                attribute.offset = stride;
                attribute.bufferIndex = metal_buffer_index;
                stride += GetByteFromPixelFormat(element.format);
            }
            auto* layout = vertex_descriptor.layouts[metal_buffer_index];
            layout.stride = stride;
            layout.stepFunction = binding.input_rate == VIR_INSTANCE ?
                MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            layout.stepRate = 1;
        }
        descriptor.vertexDescriptor = vertex_descriptor;

        for (uint index = 0; index < create_info.color_attachment_count; ++index) {
            auto* attachment = descriptor.colorAttachments[index];
            const RHIColorAttachmentInfo& info = create_info.color_attachments_info[index];
            attachment.pixelFormat = ToMetalFormat(info.pixel_format);
            const RHIBlendAttachmentInfo& blend = info.blend_state_info;
            attachment.blendingEnabled =
                blend.color_blend_op != BO_ADD || blend.color_src_blend_factor != BF_ONE ||
                blend.color_dst_blend_factor != BF_ZERO || blend.alpha_blend_op != BO_ADD ||
                blend.alpha_src_blend_factor != BF_ONE || blend.alpha_dst_blend_factor != BF_ZERO;
            attachment.rgbBlendOperation = ToMetalBlendOperation(blend.color_blend_op);
            attachment.alphaBlendOperation = ToMetalBlendOperation(blend.alpha_blend_op);
            attachment.sourceRGBBlendFactor = ToMetalBlendFactor(blend.color_src_blend_factor);
            attachment.destinationRGBBlendFactor = ToMetalBlendFactor(blend.color_dst_blend_factor);
            attachment.sourceAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_src_blend_factor);
            attachment.destinationAlphaBlendFactor = ToMetalBlendFactor(blend.alpha_dst_blend_factor);
            MTLColorWriteMask write_mask = MTLColorWriteMaskNone;
            if (blend.color_write_mask & CW_RED) write_mask |= MTLColorWriteMaskRed;
            if (blend.color_write_mask & CW_GREEN) write_mask |= MTLColorWriteMaskGreen;
            if (blend.color_write_mask & CW_BLUE) write_mask |= MTLColorWriteMaskBlue;
            if (blend.color_write_mask & CW_ALPHA) write_mask |= MTLColorWriteMaskAlpha;
            attachment.writeMask = write_mask;
        }
        if (create_info.depth_stencil_format != PF_UNDEFINED) {
            descriptor.depthAttachmentPixelFormat = ToMetalFormat(create_info.depth_stencil_format);
            if (create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT) {
                descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> native_pipeline =
            [native_->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (native_pipeline == nil) {
            throw std::runtime_error("Metal render pipeline creation failed: " +
                std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        }
        PipelineHandle handle{};
        handle.binding_infos.resize(shader_info.layout_hash.size());
        for (uint index = 0; index < shader_info.layout_hash.size(); ++index) {
            handle.hash_2_info_index[GetHash(shader_info.layout_hash[index])] = index;
            const auto mark_active = [&](const SingleShaderInfo& shader) {
                if (shader.shader_param_map == nullptr) return;
                const auto& reflection = shader.shader_param_map->reflect_map;
                const bool bindless = shader_info.arg_cpp_info[index].type == SDA_BindlessArray;
                const auto found = reflection.find(std::string(
                    bindless ? ReflectParamInfo::bdls_name : shader_info.layout_hash[index]
                ));
                if (found == reflection.end()) return;
                bool active = false;
                if (bindless) {
                    const auto& resources = found->second.spirv.bindless;
                    active = (resources.array && resources.array->custom_flag.active) ||
                             (resources.buffer && resources.buffer->custom_flag.active) ||
                             (resources.image && resources.image->custom_flag.active) ||
                             (resources.sampler && resources.sampler->custom_flag.active);
                } else if (const auto* resource = std::get_if<ReflectParamInfo::Resource>(
                               &found->second.spirv.resources.data)) {
                    active = resource->custom_flag.active;
                } else if (const auto* constant = std::get_if<ReflectParamInfo::Constant>(
                               &found->second.spirv.resources.data)) {
                    active = constant->custom_flag.active;
                }
                if (active) handle.valid_bits |= uint64(1) << index;
            };
            mark_active(shaders.vs);
            mark_active(shaders.ps);
            if (shader_info.arg_cpp_info[index].type == SDA_Constant &&
                (handle.valid_bits & (uint64(1) << index))) {
                handle.constant_idx = index;
            }
        }
        handle.handle = reinterpret_cast<uint64>(MoerNew(MetalPipelineState)(native_pipeline));
        return handle;
    }
}
PipelineHandle MetalDevice::CreatePipeline(PipelineShaderInfo&&) { Unsupported("compute pipelines"); }
void MetalDevice::WaitIdle() { native_->graphics.Sync(); }

void* GetMetalNativeTexture(Texture* texture) noexcept {
    auto* metal_texture = dynamic_cast<MetalTexture*>(texture);
    return metal_texture == nullptr ? nullptr : (__bridge void*)metal_texture->Native();
}

void* GetMetalNativeBuffer(Buffer* buffer) noexcept {
    auto* metal_buffer = dynamic_cast<MetalBuffer*>(buffer);
    return metal_buffer == nullptr ? nullptr : (__bridge void*)metal_buffer->Native();
}

void* GetMetalNativeBindlessIndexBuffer(BindlessArray* array) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeIndices();
}

void* GetMetalNativeBindlessArgumentBuffer(BindlessArray* array, uint set) noexcept {
    auto* metal = dynamic_cast<MetalBindlessArray*>(array);
    return metal == nullptr ? nullptr : (__bridge void*)metal->NativeArguments(set);
}

} // namespace Moer::Render
