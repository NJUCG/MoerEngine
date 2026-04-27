#include "VulkanAllocator.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "taskgraph/ThreadManager.h"
#include <atomic>
#include <thread>
namespace Moer::Render {

struct VulkanCommitSession {
    friend class VulkanStorage;

public:
    VulkanCommitSession(VulkanStorage& _io_interface) : io_interface(_io_interface) {}
    VulkanStorage& io_interface;

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
    void Enqueue(void const* _mem, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        void const*  _mem,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void EnqueueSignal(FenceRef _fence, uint64_t _timeline);
    void Commit();
    bool IsComplete() const;

private:
    Array<IOCmd>       file_cmds;
    Array<IOCmd>       mem_cmds;
    Array<IOSignalEvt> signal_events;
    std::atomic_uint   pending_tasks = 0;
    bool               signaled      = false;
    uint               task_count    = 1;

    void CommitWithoutSignal();
    void CommitSignaled();
    void FlushAllCommit();
    void CommitIOTask(class VulkanIOTask*);
};

struct VkExecCallback {
    Array<std::function<void()>> callback;
    Array<FileHandle>            files;
    uint64                       timeline;

    void Clear() {
        for (auto& file : files) {
            Memory::Free(file.file);
        }
    }
};
class VulkanIOInterface : public IOInterface {
public:
    friend VulkanCommitSession;
    VulkanIOInterface(VulkanDevice& _device, VkCopyQueue& _queue);
    ~VulkanIOInterface();
    void      Execute(IOCommandList&& _cmd_list, uint64 _timeline);
    uint64    Execute(IOCommandList&& _cmd_list) override;
    WaitEvent GetWaitEvent(uint64 _time_stamp) override;
    void      Sync(uint64_t _time_stamp) override;

    VulkanCommitSession* NewSession();

    VkCopyQueue& GetQueue() {
        return m_queue;
    }

private:
    UniquePtr<VulkanAllocator> GetAllocator() {
        if (allocators.size() >= max_allocators) {
        }
        auto allocator = MakeUnique<VulkanAllocator>(&m_device, EQueueType::Copy);
        return std::move(allocator);
    }
    void Complete(uint64 _timeline) {
        while (executed_frame < _timeline) {
            std::this_thread::yield();
        }
    }

    void Tick();

private:
    VulkanDevice&                       m_device;
    VkCopyQueue&                        m_queue;
    DEQueue<UniquePtr<VulkanAllocator>> allocators;
    static constexpr uint32_t           max_allocators = 2;
    std::atomic_uint64_t                executed_frame = 0;
    std::atomic_uint64_t                last_frame     = 0;

    class VulkanStorage*        storage;
    Queue<VulkanCommitSession*> commit_sessions;

    Queue<std::pair<IOCommandList&&, uint64>> io_cmdlists;
    std::mutex                                mut;

    Queue<VkExecCallback> callbacks;
    FenceRef              event;
    uint64                timeline = 0;
};

class VulkanStorage {
public:
    friend VulkanCommitSession;
    friend class VulkanIOTaskThread;
    VulkanStorage(VulkanDevice& _device, VkCopyQueue& _copy_queue);
    ~VulkanStorage();
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
    void Enqueue(void const* _mem, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        void const*  _mem,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void EnqueueSignal(FenceRef _fence, uint64_t _timeline);
    void Commit();

private:
    VulkanCommitSession* GetCurrentSession();

    VkCopyQueue& GetQueue();
    void         CommitIOTask(VulkanIOTask* _tsk);

private:
    VkCopyQueue&               copy_queue;
    VulkanCommitSession*       current_session;
    struct VulkanIOTaskThread* task_thread;
};
} // namespace Moer::Render