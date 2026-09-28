#include "rhi/metal/MetalResource.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Moer::Render {
uint64 MetalFence::GetValue() const {
    std::lock_guard lock(mutex_);
    return completed_;
}

void MetalFence::Wait(uint64 value) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return completed_ >= value || IsRejectedLocked(value); });
}

void MetalFence::MarkSubmitted(uint64 value) {
    {
        std::lock_guard lock(mutex_);
        submitted_ = std::max(submitted_, value);
    }
    cv_.notify_all();
}

bool MetalFence::WaitSubmitted(uint64 value, const std::atomic_bool* continue_waiting,
    EQueueType, uint32) {
    std::unique_lock lock(mutex_);
    while (submitted_ < value && !IsRejectedLocked(value) &&
           (continue_waiting == nullptr || continue_waiting->load(std::memory_order_acquire))) {
        cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return submitted_ >= value && !IsRejectedLocked(value);
}

void MetalFence::Reject(uint64 value) noexcept {
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

bool MetalFence::IsRejected(uint64 value) const {
    std::lock_guard lock(mutex_);
    return IsRejectedLocked(value);
}

void MetalFence::Complete(uint64 value) {
    {
        std::lock_guard lock(mutex_);
        completed_ = std::max(completed_, value);
    }
    cv_.notify_all();
}

bool MetalFence::IsRejectedLocked(uint64 value) const {
    return reject_all_ || rejected_.contains(value);
}

MetalBuffer::MetalBuffer(const BufferInfo& info, id<MTLBuffer> buffer,
    id<MTLTexture> texel_texture) :
    Buffer(info), buffer_(buffer), texel_texture_(texel_texture) {}

id<MTLBuffer> MetalBuffer::Native() const noexcept { return buffer_; }
id<MTLTexture> MetalBuffer::NativeTexelTexture() const noexcept { return texel_texture_; }

void MetalBuffer::SetName(const std::string_view name) {
    debug_name = std::string(name);
    buffer_.label = [NSString stringWithUTF8String:debug_name->c_str()];
}

MetalTexture::MetalTexture(const TextureInfo& info, id<MTLTexture> texture) :
    Texture(info), texture_(texture) {}

id<MTLTexture> MetalTexture::Native() const noexcept { return texture_; }

uint MetalTexture::GetMipByteSize(uint mip) const {
    if (mip >= GetNumMips()) Unsupported("texture mip sizes");
    return std::max(1u, GetWidth() >> mip) *
           std::max(1u, GetHeight() >> mip) * PixelStride(GetFormat());
}

void MetalTexture::SetName(const std::string_view name) {
    debug_name = std::string(name);
    texture_.label = [NSString stringWithUTF8String:debug_name->c_str()];
}

void ValidateTextureUpload(const UploadTextureCmd& upload) {
    auto* texture = dynamic_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
    const uint3 size = upload.Size();
    const uint mip = upload.MipLevel();
    if (texture == nullptr || upload.Format() != texture->GetFormat() ||
        mip >= texture->GetNumMips() || upload.ArrayLayer() >= texture->GetNumArray() ||
        upload.Offset() != uint3{0, 0, 0} ||
        size != uint3{std::max(1u, texture->GetWidth() >> mip),
                      std::max(1u, texture->GetHeight() >> mip), 1} ||
        upload.Data().size_bytes() != size_t(size.x) * size.y * PixelStride(upload.Format())) {
        Unsupported("this texture upload layout");
    }
}

void ValidateBufferUpload(const UploadBufferCmd& upload) {
    auto* buffer = dynamic_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
    if (buffer == nullptr || upload.ByteSize() == 0 ||
        upload.Offset() > buffer->GetByteSize() ||
        upload.ByteSize() > buffer->GetByteSize() - upload.Offset() ||
        upload.Data().size_bytes() != upload.ByteSize()) {
        Unsupported("this buffer upload layout");
    }
}

void EncodeTextureUpload(
    id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadTextureCmd& upload
) {
    auto* texture = static_cast<MetalTexture*>(reinterpret_cast<Texture*>(upload.Handle()));
    const NSUInteger width = upload.Size().x;
    const NSUInteger height = upload.Size().y;
    const NSUInteger source_row_bytes = width * PixelStride(upload.Format());
    const NSUInteger staging_row_bytes = (source_row_bytes + 255) & ~NSUInteger(255);
    id<MTLBuffer> staging = [device newBufferWithLength:staging_row_bytes * height
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
              toTexture:texture->Native() destinationSlice:upload.ArrayLayer()
       destinationLevel:upload.MipLevel()
     destinationOrigin:MTLOriginMake(0, 0, 0)];
}

void EncodeBufferUpload(
    id<MTLDevice> device, id<MTLBlitCommandEncoder> blit,
    NSMutableArray<id<MTLBuffer>>* staging_buffers, const UploadBufferCmd& upload
) {
    auto* buffer = static_cast<MetalBuffer*>(reinterpret_cast<Buffer*>(upload.Handle()));
    id<MTLBuffer> staging = [device newBufferWithBytes:upload.Data().data()
                                              length:upload.ByteSize()
                                             options:MTLResourceStorageModeShared];
    if (staging == nil) throw std::runtime_error("Cannot allocate Metal buffer staging");
    [staging_buffers addObject:staging];
    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:buffer->Native()
      destinationOffset:upload.Offset() size:upload.ByteSize()];
}

void* GetMetalNativeTexture(Texture* texture) noexcept {
    auto* metal_texture = dynamic_cast<MetalTexture*>(texture);
    return metal_texture == nullptr ? nullptr : (__bridge void*)metal_texture->Native();
}

void* GetMetalNativeBuffer(Buffer* buffer) noexcept {
    auto* metal_buffer = dynamic_cast<MetalBuffer*>(buffer);
    return metal_buffer == nullptr ? nullptr : (__bridge void*)metal_buffer->Native();
}

} // namespace Moer::Render
