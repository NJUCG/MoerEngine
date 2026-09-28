#pragma once

#include "rhi/metal/MetalFormat.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>

namespace Moer::Render {
// The bootstrap queues wait for each command buffer to finish. This host
// timeline still distinguishes submitted, completed, and rejected values so
// callers cannot mistake a rejected dependency for completed GPU work.
class MetalFence final : public Fence {
public:
    uint64 GetValue() const override;
    void Wait(uint64 value) override;
    void MarkSubmitted(uint64 value) override;
    bool WaitSubmitted(uint64 value, const std::atomic_bool* continue_waiting,
        EQueueType, uint32) override;
    void Reject(uint64 value) noexcept override;
    bool IsRejected(uint64 value) const override;
    void Complete(uint64 value);

private:
    bool IsRejectedLocked(uint64 value) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64 submitted_{0};
    uint64 completed_{0};
    std::unordered_set<uint64> rejected_;
    bool reject_all_{false};
};

class MetalBuffer final : public Buffer {
public:
    MetalBuffer(const BufferInfo& info, id<MTLBuffer> buffer,
                id<MTLTexture> texel_texture = nil);

    id<MTLBuffer> Native() const noexcept;
    id<MTLTexture> NativeTexelTexture() const noexcept;
    void SetName(const std::string_view name) override;

private:
    id<MTLBuffer> buffer_;
    id<MTLTexture> texel_texture_{nil};
};

class MetalTexture final : public Texture {
public:
    MetalTexture(const TextureInfo& info, id<MTLTexture> texture);

    id<MTLTexture> Native() const noexcept;
    uint GetMipByteSize(uint mip) const override;
    void SetName(const std::string_view name) override;

private:
    id<MTLTexture> texture_;
};

void ValidateTextureUpload(const UploadTextureCmd& upload);
void ValidateBufferUpload(const UploadBufferCmd& upload);
void EncodeTextureUpload(id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadTextureCmd& upload);
void EncodeBufferUpload(id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadBufferCmd& upload);
} // namespace Moer::Render
