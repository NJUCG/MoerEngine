#pragma once

#include "misc/Types.h"
#include "rhi/backends/metal/MetalResource.h"

#include <mutex>
#include <vector>

namespace Moer::Render {
// The table layout mirrors the SPIRV-Cross MSL argument-buffer sets: set 1
// contains the indirect uint table and buffer addresses, set 2 texture IDs,
// and set 3 sampler IDs. Index zero stays unbound, as on Vulkan.
class MetalBindlessArray final : public BindlessArray {
public:
    static constexpr uint32 kSamplerCount = uint32(SF_Num) * uint32(SAM_Num) * uint32(SCF_Num);
    static_assert(kSamplerCount <= 256);
    static_assert(sizeof(MTLResourceID) == sizeof(uint64));
    static_assert(sizeof(MTLGPUAddress) == sizeof(uint64));

    MetalBindlessArray(id<MTLDevice> device, uint32 capacity);

    uint32 AllocateTexture(const TextureView& view, Sampler sampler) override;

    uint32 AllocateBuffer(BufferView view) override;

    void   UnbindTexture(uint32 index) override;
    void   UnbindBuffer(uint32 index) override;
    uint64 ArrayHandle() const override { return reinterpret_cast<uint64>(this); }

    void Apply(const Array<UpdateCmd>& updates);

    id<MTLBuffer> NativeIndices() const noexcept;
    id<MTLBuffer> NativeArguments(uint32 set) const noexcept;

    void UseRenderResources(id<MTLRenderCommandEncoder> encoder);

    void UseComputeResources(id<MTLComputeCommandEncoder> encoder);

protected:
    UniquePtr<Command> CreateUpdateCommand() override;

    void DiscardUpdateCommand(const Array<UpdateCmd>& updates) override;

private:
    enum class Kind { Empty, Texture, Buffer };
    struct AppliedSlot {
        TextureRef texture{};
        id<MTLTexture> texture_view{nil};
        BufferRef buffer{};
        uint64 generation{0};
    };
    struct Slot {
        TextureRef texture{};
        id<MTLTexture> texture_view{nil};
        BufferRef buffer{};
        uint64 generation{0};
        uint64 buffer_offset{0};
        uint32         sampler_index{0};
        Kind kind{Kind::Empty};
        bool active{false};
    };

    uint32 NextSlot();

    static uint64 NextGeneration(Slot& slot);

    static uint32 SamplerIndex(Sampler sampler);

    void EnsureSampler(Sampler sampler, uint32 index);

    void Unbind(uint32 index, Kind kind);

    id<MTLDevice> device_;
    id<MTLBuffer> indices_;
    id<MTLBuffer> buffer_arguments_;
    id<MTLBuffer> texture_arguments_;
    id<MTLBuffer> sampler_arguments_;
    id<MTLSamplerState> samplers_[kSamplerCount]{};
    std::vector<Slot> slots_;
    std::vector<AppliedSlot> applied_slots_;
    std::vector<uint32>      free_slots_;
    Array<UpdateCmd> pending_;
    const uint32             capacity_;
    uint32                   next_slot_{1};
    std::mutex mutex_;
};

} // namespace Moer::Render
