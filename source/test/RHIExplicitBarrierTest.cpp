#include "rhi/RHIImpl.h"
#include "rhi/vulkan/RHICmdReorderer.h"
#include "rhi/vulkan/VulkanResourceTracker.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace Moer;
using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

class TestBuffer final : public Buffer {
public:
    explicit TestBuffer(uint64 _byte_size) :
        Buffer(BufferInfo{
            _byte_size,
            1,
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC |
                EBufferUsageFlags::TRANSFER_DST,
        }) {}

    void SetName(const std::string_view) override {}
};

class TestTexture final : public Texture {
public:
    TestTexture(uint8 _num_mips, uint16 _num_arrays, ETextureAspectFlags _aspects) :
        Texture(MakeInfo(_num_mips, _num_arrays, _aspects)) {}

    uint GetMipByteSize(uint) const override {
        return 4;
    }

    void SetName(const std::string_view) override {}

private:
    static TextureInfo MakeInfo(uint8 _num_mips, uint16 _num_arrays, ETextureAspectFlags _aspects) {
        TextureInfo info{};
        info.usage = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
                     ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT |
                     ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST;
        info.extent       = {64, 64};
        info.num_mips     = _num_mips;
        info.array_size   = _num_arrays;
        info.aspect_flags = _aspects;
        return info;
    }
};

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

template<typename TCallable>
void ExpectInvalidArgument(TCallable&& _callable, const char* _message) {
    try {
        std::forward<TCallable>(_callable)();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(_message);
}

void LegacyVulkanStateMappingIsStageCompatible() {
    const auto compute_srv = VkTracker::ResolveTextureState(
        ETextureState::SHADER_RESOURCE,
        EPassType::Compute,
        false,
        false
    );
    Expect(
        std::get<0>(compute_srv) == VK_ACCESS_2_SHADER_READ_BIT &&
            std::get<1>(compute_srv) ==
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            std::get<2>(compute_srv) ==
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        "compute SRV texture mapping is not shader-stage compatible"
    );

    const auto raytracing_sampled = VkTracker::ResolveTextureState(
        ETextureState::SAMPLE,
        EPassType::Raytracing,
        false,
        false
    );
    Expect(
        std::get<0>(raytracing_sampled) ==
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
            std::get<2>(raytracing_sampled) ==
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        "raytracing sampled texture mapping lost its shader stage"
    );

    const auto graphics_srv = VkTracker::ResolveTextureState(
        ETextureState::SHADER_RESOURCE,
        EPassType::Graphics,
        false,
        false
    );
    Expect(
        std::get<2>(graphics_srv) ==
            (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
        "graphics SRV texture mapping must cover vertex and fragment shaders"
    );

    const auto graphics_render_target_read =
        VkTracker::ResolveTextureState(
            ETextureState::RENDER_TARGET,
            EPassType::Graphics,
            false,
            false
        );
    Expect(
        std::get<0>(graphics_render_target_read) ==
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT &&
            std::get<1>(graphics_render_target_read) ==
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            std::get<2>(graphics_render_target_read) ==
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        "graphics render-target read mapping lost queue-transfer compatibility"
    );

    const auto copy_src = VkTracker::ResolveTextureState(
        ETextureState::TRANSFER,
        EPassType::Copy,
        false,
        false
    );
    const auto copy_dst = VkTracker::ResolveTextureState(
        ETextureState::TRANSFER,
        EPassType::Copy,
        true,
        false
    );
    Expect(
        std::get<0>(copy_src) == VK_ACCESS_2_TRANSFER_READ_BIT &&
            std::get<1>(copy_src) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            std::get<2>(copy_src) == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
            std::get<0>(copy_dst) == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
            std::get<1>(copy_dst) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            std::get<2>(copy_dst) == VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        "copy texture mapping is not transfer-stage compatible"
    );

    const auto depth_read = VkTracker::ResolveTextureState(
        ETextureState::DEPTH_STENCIL,
        EPassType::Graphics,
        false,
        true
    );
    Expect(
        std::get<0>(depth_read) ==
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT &&
            std::get<1>(depth_read) ==
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
            std::get<2>(depth_read) ==
                (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT),
        "depth read mapping is not attachment-stage compatible"
    );

    const auto compute_uav = VkTracker::ResolveTextureState(
        ETextureState::UNORDERED_ACCESS,
        EPassType::Compute,
        true,
        false
    );
    Expect(
        std::get<0>(compute_uav) ==
                (VK_ACCESS_2_SHADER_READ_BIT |
                 VK_ACCESS_2_SHADER_WRITE_BIT) &&
            std::get<1>(compute_uav) == VK_IMAGE_LAYOUT_GENERAL &&
            std::get<2>(compute_uav) ==
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        "compute UAV texture mapping is not shader-stage compatible"
    );

    const auto copy_buffer_src = VkTracker::ResolveBufferState(
        EBufferState::TRANSFER,
        EPassType::Copy,
        false
    );
    const auto raytracing_buffer_srv = VkTracker::ResolveBufferState(
        EBufferState::SHADER_RESOURCE,
        EPassType::Raytracing,
        false
    );
    const auto graphics_buffer_srv = VkTracker::ResolveBufferState(
        EBufferState::SHADER_RESOURCE,
        EPassType::Graphics,
        false
    );
    Expect(
        std::get<0>(copy_buffer_src) == VK_ACCESS_2_TRANSFER_READ_BIT &&
            std::get<1>(copy_buffer_src) ==
                VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
            std::get<0>(raytracing_buffer_srv) ==
                VK_ACCESS_2_SHADER_READ_BIT &&
            std::get<1>(raytracing_buffer_srv) ==
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR &&
            std::get<1>(graphics_buffer_srv) ==
                (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
        "legacy buffer mapping lost copy or raytracing stage semantics"
    );

    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveTextureState(
                ETextureState::SHADER_RESOURCE,
                EPassType::Copy,
                false,
                false
            );
        },
        "copy pass accepted a shader-resource texture"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveTextureState(
                ETextureState::RENDER_TARGET,
                EPassType::Compute,
                true,
                false
            );
        },
        "compute pass accepted a render-target attachment"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveTextureState(
                ETextureState::SAMPLE,
                EPassType::Graphics,
                true,
                false
            );
        },
        "write path accepted a sampled texture"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveBufferState(
                EBufferState::SHADER_RESOURCE,
                EPassType::Copy,
                false
            );
        },
        "copy pass accepted a shader-resource buffer"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveBufferState(
                EBufferState::VERTEX,
                EPassType::Graphics,
                true
            );
        },
        "write path accepted a vertex buffer"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveTextureState(
                ETextureState::TRANSFER,
                static_cast<EPassType>(99),
                false,
                false
            );
        },
        "texture mapping accepted an unknown pass type"
    );
    ExpectInvalidArgument(
        [] {
            (void)VkTracker::ResolveBufferState(
                static_cast<EBufferState>(99),
                EPassType::Graphics,
                false
            );
        },
        "buffer mapping accepted an unknown state"
    );
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

void PresentationSourceUsageSelectsGeneralPreferredLayout() {
    const ETextureUsageFlags sampled_output_usage =
        ETextureUsageFlags::SAMPLED |
        ETextureUsageFlags::COLOR_ATTACHMENT |
        ETextureUsageFlags::TRANSFER_SRC |
        ETextureUsageFlags::TRANSFER_DST;

    Expect(
        VulkanTexture::PreferredLayoutForUsage(
            sampled_output_usage |
            ETextureUsageFlags::PRESENTATION_SOURCE
        ) == VK_IMAGE_LAYOUT_GENERAL,
        "sampled presentation source must prefer GENERAL"
    );
    Expect(
        VulkanTexture::PreferredLayoutForUsage(
            sampled_output_usage
        ) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "ordinary sampled texture must keep its shader-read preferred layout"
    );
}

void LocalExplicitPayloadAndOwnershipArePreserved() {
    TestBuffer       buffer_resource(256);
    TestTexture      texture_resource(6, 4, ETextureAspectFlags::DEPTH_SLICE);
    const uint64     buffer_handle  = reinterpret_cast<uint64>(&buffer_resource);
    const uint64     texture_handle = reinterpret_cast<uint64>(&texture_resource);
    const BufferView buffer(&buffer_resource, 64, 32, 4);
    TextureView      texture{};
    texture.texture     = &texture_resource;
    texture.mip_level   = 2;
    texture.num_mips    = 3;
    texture.array_layer = 1;
    texture.num_array   = 2;

    const BarrierState buffer_src =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE);
    const BarrierState buffer_dst =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER, ERHIAccessFlags::SHADER_READ);
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
        BarrierCreateInfo::Transition(texture, texture_src, texture_dst, ETextureAspectFlags::DEPTH_SLICE),
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

    const ExplicitBufferBarrier& recorded_buffer = command.ExplicitBuffers().front();
    Expect(recorded_buffer.handle == buffer_handle, "explicit buffer identity changed");
    Expect(recorded_buffer.offset == 64, "explicit buffer offset changed");
    Expect(recorded_buffer.byte_size == 128, "explicit buffer size changed");
    Expect(recorded_buffer.src_state == buffer_src, "explicit buffer src state changed");
    Expect(recorded_buffer.dst_state == buffer_dst, "explicit buffer dst state changed");
    Expect(
        recorded_buffer.queue_transfer == BarrierQueueTransfer{},
        "local explicit buffer acquired queue-transfer metadata"
    );

    const ExplicitTextureBarrier& recorded_texture = command.ExplicitTextures().front();
    Expect(recorded_texture.handle == texture_handle, "explicit texture identity changed");
    Expect(
        recorded_texture.texture_aspects == ETextureAspectFlags::DEPTH_SLICE,
        "explicit texture aspect changed"
    );
    Expect(
        recorded_texture.mip_level == 2 && recorded_texture.mip_count == 3 &&
            recorded_texture.array_layer == 1 && recorded_texture.array_count == 2,
        "explicit texture subresource range changed"
    );
    Expect(recorded_texture.src_state == texture_src, "explicit texture src state changed");
    Expect(recorded_texture.dst_state == texture_dst, "explicit texture dst state changed");
    Expect(
        recorded_texture.queue_transfer == BarrierQueueTransfer{},
        "local explicit texture acquired queue-transfer metadata"
    );

    // Ownership is per immutable submission, not sticky CommandList state.
    CmdSubmit reused = list.Submit();
    Expect(
        !reused.HasExplicitResourceStateOwnership(), "CommandList retained explicit ownership across Submit"
    );
}

void ExpectQueueTransferPayload(
    const CmdSubmit&            _submit,
    EQueueType                  _recording_queue,
    const BufferView&           _buffer,
    const TextureView&          _texture,
    const BarrierState&         _buffer_src,
    const BarrierState&         _buffer_dst,
    const BarrierState&         _texture_src,
    const BarrierState&         _texture_dst,
    const BarrierQueueTransfer& _transfer
) {
    Expect(
        _submit.HasExplicitResourceStateOwnership(),
        "queue-transfer barrier did not preserve explicit state ownership"
    );
    Expect(_submit.cmds.size() == 1, "queue-transfer barriers were split into multiple commands");
    const auto& command = *static_cast<const BarrierCmd*>(_submit.cmds.front().get());
    Expect(command.HasExplicitBarriers(), "queue-transfer payload was lost");
    Expect(
        command.GetSrcQueue() == _recording_queue && command.GetDstQueue() == _recording_queue,
        "BarrierCmd affinity did not remain on the recording endpoint"
    );
    Expect(
        command.ExplicitBuffers().size() == 1 && command.ExplicitTextures().size() == 1,
        "queue-transfer explicit payload count changed"
    );

    const ExplicitBufferBarrier& recorded_buffer = command.ExplicitBuffers().front();
    Expect(
        recorded_buffer.handle == reinterpret_cast<uint64>(_buffer.GetBuffer()),
        "queue-transfer buffer identity changed"
    );
    Expect(
        recorded_buffer.offset == _buffer.GetByteOffset() &&
            recorded_buffer.byte_size == _buffer.GetByteSize(),
        "queue-transfer buffer range changed"
    );
    Expect(
        recorded_buffer.src_state == _buffer_src && recorded_buffer.dst_state == _buffer_dst,
        "queue-transfer buffer states changed"
    );
    Expect(recorded_buffer.queue_transfer == _transfer, "queue-transfer buffer endpoints or phase changed");

    const ExplicitTextureBarrier& recorded_texture = command.ExplicitTextures().front();
    Expect(
        recorded_texture.handle == reinterpret_cast<uint64>(_texture.GetTexture()),
        "queue-transfer texture identity changed"
    );
    Expect(
        recorded_texture.texture_aspects == ETextureAspectFlags::DEPTH_SLICE,
        "queue-transfer texture aspect changed"
    );
    Expect(
        recorded_texture.mip_level == _texture.mip_level && recorded_texture.mip_count == _texture.num_mips &&
            recorded_texture.array_layer == _texture.array_layer &&
            recorded_texture.array_count == _texture.num_array,
        "queue-transfer texture range changed"
    );
    Expect(
        recorded_texture.src_state == _texture_src && recorded_texture.dst_state == _texture_dst,
        "queue-transfer texture states changed"
    );
    Expect(recorded_texture.queue_transfer == _transfer, "queue-transfer texture endpoints or phase changed");
}

void ReleaseAndAcquirePayloadsPreserveEndpointAffinity() {
    TestBuffer       buffer_resource(256);
    TestTexture      texture_resource(4, 3, ETextureAspectFlags::DEPTH_SLICE);
    const BufferView buffer(&buffer_resource, 32, 16, 4);
    TextureView      texture{};
    texture.texture     = &texture_resource;
    texture.mip_level   = 1;
    texture.num_mips    = 2;
    texture.array_layer = 1;
    texture.num_array   = 2;

    const BarrierState buffer_src =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE);
    const BarrierState buffer_dst =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER, ERHIAccessFlags::SHADER_READ);
    const BarrierState texture_src = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT,
        ERHIAccessFlags::COLOR_ATTACHMENT_WRITE,
        ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT
    );
    const BarrierState texture_dst = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
        ERHIAccessFlags::SHADER_SAMPLED_READ,
        ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );

    const auto make_barriers = [&](BarrierQueueTransfer _transfer) {
        BarrierCreateInfo buffer_barrier  = BarrierCreateInfo::Transition(buffer, buffer_src, buffer_dst);
        buffer_barrier.queue_transfer     = _transfer;
        BarrierCreateInfo texture_barrier = BarrierCreateInfo::Transition(
            texture, texture_src, texture_dst, ETextureAspectFlags::DEPTH_SLICE
        );
        texture_barrier.queue_transfer = _transfer;
        return std::array{
            std::move(buffer_barrier),
            std::move(texture_barrier),
        };
    };

    const BarrierQueueTransfer release =
        BarrierQueueTransfer::Release(EQueueType::Graphics, EQueueType::Compute);
    const auto  release_barriers = make_barriers(release);
    CommandList release_list(EQueueType::Graphics);
    release_list.Barriers(release_barriers);
    const CmdSubmit release_submit = release_list.Submit();
    ExpectQueueTransferPayload(
        release_submit,
        EQueueType::Graphics,
        buffer,
        texture,
        buffer_src,
        buffer_dst,
        texture_src,
        texture_dst,
        release
    );

    const BarrierQueueTransfer acquire =
        BarrierQueueTransfer::Acquire(EQueueType::Graphics, EQueueType::Compute);
    const auto  acquire_barriers = make_barriers(acquire);
    CommandList acquire_list(EQueueType::Compute);
    acquire_list.Barriers(acquire_barriers);
    const CmdSubmit acquire_submit = acquire_list.Submit();
    ExpectQueueTransferPayload(
        acquire_submit,
        EQueueType::Compute,
        buffer,
        texture,
        buffer_src,
        buffer_dst,
        texture_src,
        texture_dst,
        acquire
    );

    const BarrierQueueTransfer copy_release =
        BarrierQueueTransfer::Release(EQueueType::Graphics, EQueueType::Copy);
    const auto  copy_release_barriers = make_barriers(copy_release);
    CommandList copy_release_list(EQueueType::Graphics);
    copy_release_list.Barriers(copy_release_barriers);
    const CmdSubmit copy_release_submit = copy_release_list.Submit();
    ExpectQueueTransferPayload(
        copy_release_submit,
        EQueueType::Graphics,
        buffer,
        texture,
        buffer_src,
        buffer_dst,
        texture_src,
        texture_dst,
        copy_release
    );

    const BarrierQueueTransfer copy_acquire =
        BarrierQueueTransfer::Acquire(EQueueType::Graphics, EQueueType::Copy);
    const auto  copy_acquire_barriers = make_barriers(copy_acquire);
    CommandList copy_acquire_list(EQueueType::Copy);
    copy_acquire_list.Barriers(copy_acquire_barriers);
    const CmdSubmit copy_acquire_submit = copy_acquire_list.Submit();
    ExpectQueueTransferPayload(
        copy_acquire_submit,
        EQueueType::Copy,
        buffer,
        texture,
        buffer_src,
        buffer_dst,
        texture_src,
        texture_dst,
        copy_acquire
    );
}

void ExpectRejectedWithoutMutation(
    CommandList&                       _list,
    std::span<const BarrierCreateInfo> _barriers,
    const char*                        _rejection_message
) {
    bool rejected = false;
    try {
        _list.Barriers(_barriers);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Expect(rejected, _rejection_message);
    Expect(
        !_list.HasExplicitResourceStateOwnership(), "rejected explicit barrier changed CommandList ownership"
    );
    CmdSubmit submit = _list.Submit();
    Expect(submit.cmds.empty(), "rejected explicit barrier recorded a partial command");
    Expect(
        !submit.HasExplicitResourceStateOwnership(), "rejected explicit barrier changed CmdSubmit ownership"
    );
}

void InvalidQueueTransferEndpointsFailWithoutMutation() {
    TestBuffer         buffer_resource(64);
    const BufferView   buffer(&buffer_resource, 0, 16, 4);
    const BarrierState src =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE);
    const BarrierState dst =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER, ERHIAccessFlags::SHADER_READ);

    BarrierCreateInfo release = BarrierCreateInfo::Transition(buffer, src, dst);
    release.queue_transfer    = BarrierQueueTransfer::Release(EQueueType::Graphics, EQueueType::Compute);
    const std::array release_batch{release};
    CommandList      wrong_release_endpoint(EQueueType::Compute);
    ExpectRejectedWithoutMutation(
        wrong_release_endpoint, release_batch, "release barrier was accepted on its destination endpoint"
    );

    BarrierCreateInfo acquire = BarrierCreateInfo::Transition(buffer, src, dst);
    acquire.queue_transfer    = BarrierQueueTransfer::Acquire(EQueueType::Graphics, EQueueType::Compute);
    const std::array acquire_batch{acquire};
    CommandList      wrong_acquire_endpoint(EQueueType::Graphics);
    ExpectRejectedWithoutMutation(
        wrong_acquire_endpoint, acquire_batch, "acquire barrier was accepted on its source endpoint"
    );

    BarrierCreateInfo same_endpoint = BarrierCreateInfo::Transition(buffer, src, dst);
    same_endpoint.queue_transfer =
        BarrierQueueTransfer::Release(EQueueType::Graphics, EQueueType::Graphics);
    const std::array same_endpoint_batch{same_endpoint};
    CommandList      same_endpoint_list(EQueueType::Graphics);
    ExpectRejectedWithoutMutation(
        same_endpoint_list,
        same_endpoint_batch,
        "ownership transfer accepted identical source and destination endpoints"
    );
}

void InvalidResourceRangeFailsWithoutMutation() {
    TestBuffer         buffer_resource(128);
    const BarrierState src =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE);
    const BarrierState dst =
        BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER, ERHIAccessFlags::SHADER_READ);
    const BarrierQueueTransfer release =
        BarrierQueueTransfer::Release(EQueueType::Graphics, EQueueType::Compute);

    BarrierCreateInfo valid = BarrierCreateInfo::Transition(BufferView(&buffer_resource, 0, 16, 4), src, dst);
    valid.queue_transfer    = release;
    BarrierCreateInfo out_of_range =
        BarrierCreateInfo::Transition(BufferView(&buffer_resource, 96, 16, 4), src, dst);
    out_of_range.queue_transfer = release;

    const std::array barriers{valid, out_of_range};
    CommandList      list(EQueueType::Graphics);
    ExpectRejectedWithoutMutation(
        list, barriers, "out-of-range barrier batch was not rejected transactionally"
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

void ReleaseSentinelOverlapUsesExactBufferAndTextureRanges() {
    auto* buffer = reinterpret_cast<VulkanBuffer*>(0x7000);
    const BufferByteRangeT<VulkanBuffer> first_buffer{
        .buffer = buffer, .offset = 0, .byte_size = 256
    };
    const BufferByteRangeT<VulkanBuffer> overlapping_buffer{
        .buffer = buffer, .offset = 128, .byte_size = 256
    };
    const BufferByteRangeT<VulkanBuffer> adjacent_buffer{
        .buffer = buffer, .offset = 256, .byte_size = 256
    };
    const BufferByteRangeT<VulkanBuffer> other_buffer{
        .buffer = reinterpret_cast<VulkanBuffer*>(0x8000),
        .offset = 0,
        .byte_size = 256
    };
    Expect(
        BufferByteRangesOverlap(first_buffer, overlapping_buffer) &&
            BufferByteRangesOverlap(overlapping_buffer, first_buffer),
        "buffer release overlap must be symmetric"
    );
    Expect(
        !BufferByteRangesOverlap(first_buffer, adjacent_buffer) &&
            !BufferByteRangesOverlap(first_buffer, other_buffer),
        "buffer release overlap accepted an adjacent range or identity"
    );

    auto* texture = reinterpret_cast<VulkanTexture*>(0x9000);
    const TextureAspectSubresourceRangeT<VulkanTexture> depth{
        .subresource = {texture, 0, 1, 0, 1},
        .aspects = VK_IMAGE_ASPECT_DEPTH_BIT,
    };
    const TextureAspectSubresourceRangeT<VulkanTexture> stencil{
        .subresource = {texture, 0, 1, 0, 1},
        .aspects = VK_IMAGE_ASPECT_STENCIL_BIT,
    };
    const TextureAspectSubresourceRangeT<VulkanTexture> depth_overlap{
        .subresource = {texture, 0, 1, 0, 1},
        .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
    };
    const TextureAspectSubresourceRangeT<VulkanTexture> other_mip{
        .subresource = {texture, 1, 1, 0, 1},
        .aspects = VK_IMAGE_ASPECT_DEPTH_BIT,
    };
    Expect(
        TextureAspectSubresourceRangesOverlap(depth, depth_overlap) &&
            TextureAspectSubresourceRangesOverlap(depth_overlap, depth),
        "texture release overlap must include matching aspects"
    );
    Expect(
        !TextureAspectSubresourceRangesOverlap(depth, stencil) &&
            !TextureAspectSubresourceRangesOverlap(depth, other_mip),
        "texture release overlap accepted a disjoint aspect or subresource"
    );
}

} // namespace

int main() {
    try {
        LegacyVulkanStateMappingIsStageCompatible();
        PresentationSourceUsageSelectsGeneralPreferredLayout();
        LocalExplicitPayloadAndOwnershipArePreserved();
        ReleaseAndAcquirePayloadsPreserveEndpointAffinity();
        InvalidQueueTransferEndpointsFailWithoutMutation();
        InvalidResourceRangeFailsWithoutMutation();
        LegacyBarrierRemainsCompatible();
        EmptyExplicitBarrierBatchFailsWithoutChangingOwnership();
        ExplicitBarrierOwnsAnExclusiveReorderLayer();
        TextureRangeOverlapMatchesAtomicExplicitTracking();
        ReleaseSentinelOverlapUsesExactBufferAndTextureRanges();
    } catch (const std::exception& error) {
        std::cerr << "RHI explicit barrier contract failed: " << error.what()
                  << '\n';
        return 1;
    }
    std::cout << "RHI explicit barrier contract passed\n";
    return 0;
}
