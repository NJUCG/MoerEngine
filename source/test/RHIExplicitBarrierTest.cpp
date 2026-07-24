#include "rhi/RHIImpl.h"
#include "rhi/vulkan/RHICmdReorderer.h"
#include "rhi/vulkan/VulkanResourceTracker.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

bool FalseResourceFlag(uint64) {
    return false;
}

bool FalseBindlessMembership(uint64, uint64) {
    return false;
}

void NoopBindlessLock(uint64) {}

FunctionTable MakeFunctionTable() {
    return {
        .is_resource_write       = &FalseResourceFlag,
        .is_resource_read        = &FalseResourceFlag,
        .is_texture_sampled      = &FalseResourceFlag,
        .is_resource_in_bindless = &FalseBindlessMembership,
        .lock_bdls_array         = &NoopBindlessLock,
        .unlock_bdls_array       = &NoopBindlessLock,
    };
}

int64 FindCommandLayer(
    const CmdReorderer& _reorderer,
    const Command*      _command
) {
    for (size_t layer = 0; layer < _reorderer.m_cmd_lists.size(); ++layer) {
        for (const auto* node = _reorderer.m_cmd_lists[layer].head;
             node != nullptr;
             node = node->next) {
            if (node->cmd == _command) {
                return static_cast<int64>(layer);
            }
        }
    }
    return -1;
}

void ExplicitPayloadAndOwnershipArePreserved() {
    constexpr uint64 buffer_handle  = 0x1000;
    constexpr uint64 texture_handle = 0x2000;
    const BufferView buffer(
        reinterpret_cast<Buffer*>(buffer_handle),
        64,
        32,
        4
    );
    TextureView texture{};
    texture.texture     = reinterpret_cast<Texture*>(texture_handle);
    texture.mip_level   = 2;
    texture.num_mips    = 3;
    texture.array_layer = 1;
    texture.num_array   = 2;

    const BarrierState buffer_src = BarrierState::Buffer(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_WRITE
    );
    const BarrierState buffer_dst = BarrierState::Buffer(
        ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
        ERHIAccessFlags::SHADER_READ
    );
    const BarrierState texture_src = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT,
        ERHIAccessFlags::COLOR_ATTACHMENT_WRITE,
        ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT
    );
    const BarrierState texture_dst = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_FRAGMENT_SHADER,
        ERHIAccessFlags::SHADER_SAMPLED_READ,
        ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );

    CommandList list(EQueueType::Graphics);
    list.Barriers({
        BarrierCreateInfo::Transition(buffer, buffer_src, buffer_dst),
        BarrierCreateInfo::Transition(
            texture,
            texture_src,
            texture_dst,
            ETextureAspectFlags::DEPTH_SLICE
        ),
    });
    CmdSubmit submit = list.Submit();

    Expect(
        submit.HasExplicitResourceStateOwnership(),
        "explicit barrier did not transfer state ownership to CmdSubmit"
    );
    Expect(submit.cmds.size() == 1, "explicit barriers were split into multiple commands");
    const auto& command = *static_cast<const BarrierCmd*>(submit.cmds.front().get());
    Expect(command.HasExplicitBarriers(), "BarrierCmd lost explicit payload");
    Expect(command.ReadBuffers().empty(), "explicit buffer leaked into legacy read payload");
    Expect(command.WriteTextures().empty(), "explicit texture leaked into legacy write payload");
    Expect(command.ExplicitBuffers().size() == 1, "explicit buffer count mismatch");
    Expect(command.ExplicitTextures().size() == 1, "explicit texture count mismatch");

    const ExplicitBufferBarrier& recorded_buffer =
        command.ExplicitBuffers().front();
    Expect(recorded_buffer.handle == buffer_handle, "explicit buffer identity changed");
    Expect(recorded_buffer.offset == 64, "explicit buffer offset changed");
    Expect(recorded_buffer.byte_size == 128, "explicit buffer size changed");
    Expect(recorded_buffer.src_state == buffer_src, "explicit buffer src state changed");
    Expect(recorded_buffer.dst_state == buffer_dst, "explicit buffer dst state changed");

    const ExplicitTextureBarrier& recorded_texture =
        command.ExplicitTextures().front();
    Expect(recorded_texture.handle == texture_handle, "explicit texture identity changed");
    Expect(
        recorded_texture.texture_aspects == ETextureAspectFlags::DEPTH_SLICE,
        "explicit texture aspect changed"
    );
    Expect(
        recorded_texture.mip_level == 2 && recorded_texture.mip_count == 3 &&
            recorded_texture.array_layer == 1 &&
            recorded_texture.array_count == 2,
        "explicit texture subresource range changed"
    );
    Expect(recorded_texture.src_state == texture_src, "explicit texture src state changed");
    Expect(recorded_texture.dst_state == texture_dst, "explicit texture dst state changed");

    // Ownership is per immutable submission, not sticky CommandList state.
    CmdSubmit reused = list.Submit();
    Expect(
        !reused.HasExplicitResourceStateOwnership(),
        "CommandList retained explicit ownership across Submit"
    );
}

void LegacyBarrierRemainsCompatible() {
    constexpr uint64 buffer_handle = 0x3000;
    CommandList      list(EQueueType::Graphics);
    list.Barriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Graphics,
        ReadBuffer{
            BufferView(reinterpret_cast<Buffer*>(buffer_handle), 0, 4, 4),
            EBufferState::SHADER_RESOURCE,
        }
    );
    CmdSubmit submit = list.Submit();
    Expect(
        !submit.HasExplicitResourceStateOwnership(),
        "legacy barrier unexpectedly disabled backend state ownership"
    );
    const auto& command = *static_cast<const BarrierCmd*>(submit.cmds.front().get());
    Expect(!command.HasExplicitBarriers(), "legacy barrier became explicit");
    Expect(command.ReadBuffers().size() == 1, "legacy barrier payload changed");
}

void EmptyExplicitBarrierBatchFailsWithoutChangingOwnership() {
    CommandList list(EQueueType::Graphics);
    bool        rejected = false;
    try {
        list.Barriers(std::span<const BarrierCreateInfo>{});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Expect(rejected, "empty explicit barrier batch was not rejected");
    CmdSubmit submit = list.Submit();
    Expect(submit.cmds.empty(), "empty explicit barrier batch recorded a command");
    Expect(
        !submit.HasExplicitResourceStateOwnership(),
        "empty explicit barrier batch changed submit state ownership"
    );
}

void ExplicitBarrierOwnsAnExclusiveReorderLayer() {
    constexpr uint64 buffer_handle = 0x4000;
    const BufferView view(
        reinterpret_cast<Buffer*>(buffer_handle),
        0,
        4,
        4
    );

    BarrierCmd prior_read(
        0,
        0,
        1,
        0,
        EQueueType::Graphics,
        EQueueType::Graphics
    );
    prior_read.ReadBuffer(
        view,
        EBufferState::SHADER_RESOURCE,
        EPassType::Graphics
    );

    BarrierCmd explicit_barrier(
        1,
        EQueueType::Graphics,
        EQueueType::Graphics
    );
    explicit_barrier.AddExplicitBarrier(
        BarrierCreateInfo::Transition(
            view,
            BarrierState::Buffer(
                ERHIPipelineStageFlags::PS_FRAGMENT_SHADER,
                ERHIAccessFlags::SHADER_READ
            ),
            BarrierState::Buffer(
                ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
                ERHIAccessFlags::SHADER_READ
            )
        )
    );

    std::array<byte, 16> data{};
    UploadBufferCmd following_write(
        buffer_handle,
        0,
        data.size(),
        data.data()
    );

    TCachedArgArray cached_args;
    CmdReorderer reorderer(MakeFunctionTable(), cached_args);
    reorderer.AcceptCmd(&prior_read);
    reorderer.AcceptCmd(&explicit_barrier);
    reorderer.AcceptCmd(&following_write);

    Expect(FindCommandLayer(reorderer, &prior_read) == 0, "prior read layer changed");
    Expect(
        FindCommandLayer(reorderer, &explicit_barrier) == 1,
        "explicit barrier did not own the next exclusive layer"
    );
    Expect(
        FindCommandLayer(reorderer, &following_write) == 2,
        "following work crossed the explicit barrier layer"
    );
}

void TextureRangeOverlapMatchesAtomicExplicitTracking() {
    auto* texture = reinterpret_cast<VulkanTexture*>(0x5000);
    const TextureSubresourceKeyT<VulkanTexture> whole{
        texture, 0, 1, 0, 4
    };
    const TextureSubresourceKeyT<VulkanTexture> subset{
        texture, 0, 1, 1, 2
    };
    const TextureSubresourceKeyT<VulkanTexture> disjoint_layer{
        texture, 0, 1, 4, 1
    };
    const TextureSubresourceKeyT<VulkanTexture> disjoint_mip{
        texture, 1, 1, 1, 2
    };
    const TextureSubresourceKeyT<VulkanTexture> other_texture{
        reinterpret_cast<VulkanTexture*>(0x6000), 0, 1, 1, 2
    };

    Expect(
        TextureSubresourceRangesOverlap(whole, subset) &&
            TextureSubresourceRangesOverlap(subset, whole),
        "texture overlap must be symmetric for aggregate and atomic ranges"
    );
    Expect(
        !TextureSubresourceRangesOverlap(whole, disjoint_layer) &&
            !TextureSubresourceRangesOverlap(whole, disjoint_mip) &&
            !TextureSubresourceRangesOverlap(whole, other_texture),
        "texture overlap accepted a disjoint subresource or identity"
    );
}

} // namespace

int main() {
    try {
        ExplicitPayloadAndOwnershipArePreserved();
        LegacyBarrierRemainsCompatible();
        EmptyExplicitBarrierBatchFailsWithoutChangingOwnership();
        ExplicitBarrierOwnsAnExclusiveReorderLayer();
        TextureRangeOverlapMatchesAtomicExplicitTracking();
    } catch (const std::exception& error) {
        std::cerr << "RHI explicit barrier contract failed: " << error.what()
                  << '\n';
        return 1;
    }
    std::cout << "RHI explicit barrier contract passed\n";
    return 0;
}
