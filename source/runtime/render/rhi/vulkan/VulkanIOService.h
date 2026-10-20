#pragma once

#include "RenderAPI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include <memory>
#include <mutex>

namespace Moer::Render {

class VulkanDevice;
class VkCopyQueue;

// Enqueue snapshots memory sources and file paths and retains GPU destinations.
// CPU destinations remain borrowed until completion. Enqueue/Commit form a
// single-producer batch; committed batches drain before destruction.
// Destroy before the device/executor. Completion callbacks must not destroy
// this service or wait for work whose completion depends on that callback.
class RENDER_API VulkanStorage {
public:
    VulkanStorage(VulkanDevice& _device, VkCopyQueue& _copy_queue);
    ~VulkanStorage();
    VulkanStorage(const VulkanStorage&)            = delete;
    VulkanStorage& operator=(const VulkanStorage&) = delete;

    void Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len);
    void Enqueue(FileHandle _handle, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        FileHandle   _handle,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void Enqueue(const void* _mem, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        const void*  _mem,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void EnqueueSignal(FenceRef _fence, uint64 _timeline);
    // Queues CPU preparation. Before publishing a GPU consumer of a signal,
    // use Fence::WaitSubmitted to establish producer submission (or failure).
    void Commit();

private:
    friend class VulkanIOInterface;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class VulkanIOInterface final : public IOInterface {
public:
    VulkanIOInterface(VulkanDevice& _device, VkCopyQueue& _queue);
    ~VulkanIOInterface();
    // Consumes the list and snapshots sources before returning. Callbacks are
    // terminal cleanup (including failure), not success notifications. A CPU
    // read failure rejects the batch's signal; earlier CPU writes may remain.
    uint64    Execute(IOCommandList&& _commands) override;
    // May block for CPU preparation/submission so a consumer cannot overtake
    // the IO producer in the unified submission stream.
    // The returned event borrows this interface's fence; keep the interface
    // alive until consumers have finished waiting on it.
    WaitEvent GetWaitEvent(uint64 _time_stamp) override;
    // Waits for the signal or rejection, not for user callbacks to return.
    void Sync(uint64 _time_stamp) override;

private:
    std::mutex    mutex;
    FenceRef      event;
    uint64        timeline = 0;
    VulkanStorage storage;
};

} // namespace Moer::Render
