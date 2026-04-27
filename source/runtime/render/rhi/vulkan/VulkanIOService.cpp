#include "VulkanIOService.h"
#include "PixelFormat.h"
#include "VulkanCommand.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "taskgraph/ThreadManager.h"
#include "vulkan/vulkan_core.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
namespace Moer::Render {

struct VulkanIOTask {
    VulkanCommitSession*  session;
    std::function<void()> callback;
    Array<IOCmd>          file_cmds;
    Array<IOCmd>          mem_cmds;
    Array<IOSignalEvt>    signal_evts;
};

void VulkanCommitSession::Commit() {
    if (signal_events.empty()) {
        CommitWithoutSignal();
    } else {
        CommitSignaled();
    }
}

bool VulkanCommitSession::IsComplete() const {
    // Check if all tasks are completed
    return pending_tasks.load(std::memory_order_acquire) == task_count;
}

void VulkanCommitSession::Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
    // Enqueue file to memory copy
    file_cmds.push_back(
        {FileDesc{_handle, uint(_file_offset), uint(_len)},
         RawDataDesc{std::span<ubyte>((ubyte*)_ptr, _len)},
         0}
    );
}

void VulkanCommitSession::Enqueue(
    FileHandle _handle,
    size_t     _file_offset,
    void*      _buffer_ptr,
    size_t     _offset,
    size_t     _len
) {
    // Enqueue file to buffer copy
    file_cmds.push_back(
        {FileDesc{_handle, uint(_file_offset), uint(_len)},
         BufferViewDesc{uint64(_buffer_ptr), uint(_offset), uint(_len)},
         0}
    );
}

void VulkanCommitSession::Enqueue(
    FileHandle   _handle,
    size_t       _file_offset,
    void*        _tex_ptr,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip_offset
) {
    // Enqueue file to texture copy
    file_cmds.push_back(
        {FileDesc{_handle, uint(_file_offset), uint(GetSizeFromImageFormat(_format, _size))},
         TextureViewDesc{uint3(_offset), uint3(_size), uint64(_tex_ptr), _format, uint(_mip_offset), 0, 0, 0},
         0}
    );
}

void VulkanCommitSession::Enqueue(
    void const* _mem,
    size_t      _file_offset,
    void*       _buffer_ptr,
    size_t      _offset,
    size_t      _len
) {
    // Enqueue memory to buffer copy
    mem_cmds.push_back(
        {RawDataDesc{std::span<ubyte>((ubyte*)_mem, _len)},
         BufferViewDesc{uint64(_buffer_ptr), uint(_offset), uint(_len)},
         0}
    );
}

void VulkanCommitSession::Enqueue(
    void const*  _mem,
    size_t       _file_offset,
    void*        _tex_ptr,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip_offset
) {
    // Enqueue memory to texture copy
    mem_cmds.push_back(
        {RawDataDesc{std::span<ubyte>((ubyte*)_mem, GetSizeFromImageFormat(_format, _size))},
         TextureViewDesc{uint3(_offset), uint3(_size), uint64(_tex_ptr), _format, uint(_mip_offset), 0, 0, 0},
         0}
    );
}

void VulkanCommitSession::EnqueueSignal(FenceRef _fence, uint64_t _timeline) {
    // Enqueue signal command
    signal_events.push_back({uint64(_fence.Get()), _timeline});
}

void VulkanCommitSession::CommitWithoutSignal() {
    VulkanIOTask* tsk = MoerNew(VulkanIOTask)();
    tsk->session      = this;
    tsk->file_cmds    = std::move(file_cmds);
    tsk->mem_cmds     = std::move(mem_cmds);
    tsk->callback     = [this]() {
        ++pending_tasks;
    };
    task_count++;
    std::atomic_thread_fence(std::memory_order_acq_rel);
    CommitIOTask(tsk);
}

void VulkanCommitSession::CommitSignaled() {

    VulkanIOTask* tsk = MoerNew(VulkanIOTask)();
    tsk->session      = this;
    tsk->file_cmds    = std::move(file_cmds);
    tsk->mem_cmds     = std::move(mem_cmds);
    tsk->signal_evts  = std::move(signal_events);
    tsk->callback     = [this]() {
        ++pending_tasks;
    };
    CommitIOTask(tsk);
}

void VulkanCommitSession::CommitIOTask(VulkanIOTask* _tsk) {
    io_interface.CommitIOTask(_tsk);
}

struct VulkanIOTaskThread : public Runnable {
    RunnableThread*                 thread{nullptr};
    std::atomic_bool                enabled{true};
    LockFreeQueueBase<VulkanIOTask> queue;
    VulkanIOTaskThread() {
        thread = RunnableThread::Create(
            this,
            ThreadAttributes{.affinity = Affinity{}, .name = MOER_ASCII_TEXT("VulkanIOTaskThread")}
        );
    }

    ~VulkanIOTaskThread() {
        Stop();
        if (thread != nullptr) {
            MoerDelete(thread);
            thread = nullptr;
        }
    }

    uint32_t Run() override {
        InnerWorkLoop();
        return 0;
    }
    void Init() override {}
    void Stop() override {
        enabled.store(false, std::memory_order_release);
    }
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }

    void InnerWorkLoop() {
        while (enabled.load(std::memory_order_acquire)) {
            VulkanIOTask* task;
            if (task = queue.Pop(); task) {
                // Process the submission
                // ...
                auto copy_file_to_mem =
                    [&](const FileDesc& _src, size_t _file_offset, std::span<ubyte> _dst) {
                        FILE* result_handle = nullptr;
                        fopen_s(&result_handle, (const char*)_src.handle.file, "r");
                        if (!result_handle) {
                            LOG_ERROR(MOER_TEXT("Failed to open file {}"), (const char*)_src.handle.file);
                            assert(false && "Failed to open file");
                        }
                        std::fseek(result_handle, _file_offset, SEEK_SET);
                        std::fread(_dst.data(), sizeof(ubyte), _dst.size_bytes(), result_handle);
                        std::fclose(result_handle);
                    };
                uint64 temp_size = 0;
                for (auto& cmd : task->file_cmds) {
                    // Process each command
                    // ...
                    std::visit(
                        Overload{
                            [&](RawDataDesc& _dst) {
                                copy_file_to_mem(std::get<FileDesc>(cmd.src), cmd.file_offset, _dst.data);
                            },
                            [&](BufferViewDesc& _dst) {
                                temp_size += cmd.SizeByte();
                            },
                            [&](TextureViewDesc& _dst) {
                                temp_size += cmd.SizeByte();
                            }
                        },
                        cmd.dst
                    );
                }
                Array<ubyte> temp_buffer;
                temp_buffer.resize(temp_size);
                temp_size = 0;
                CommandList cmd_list{};
                for (auto& cmd : task->file_cmds) {
                    std::visit(
                        Overload{
                            [&](RawDataDesc& _dst) {},
                            [&](BufferViewDesc& _dst) {
                                copy_file_to_mem(
                                    std::get<FileDesc>(cmd.src),
                                    cmd.file_offset,
                                    std::span<ubyte>(temp_buffer.data() + temp_size, _dst.size)
                                );
                                VulkanBuffer* buffer = (VulkanBuffer*)_dst.handle;
                                cmd_list.CopyFrom(
                                    std::span<byte>((byte*)temp_buffer.data() + temp_size, _dst.size),
                                    buffer->GetView(_dst.offset, _dst.size)
                                );
                                temp_size += cmd.SizeByte();
                            },
                            [&](TextureViewDesc& _dst) {
                                copy_file_to_mem(
                                    std::get<FileDesc>(cmd.src),
                                    cmd.file_offset,
                                    std::span<ubyte>(temp_buffer.data() + temp_size, _dst.GetByteSize())
                                );
                                VulkanTexture* texture = (VulkanTexture*)_dst.handle;
                                TextureView    view(texture, _dst.pixel_fmt, _dst.mip_offset, _dst.mip_cnt);
                                view.offset = _dst.offset;
                                view.extent = _dst.size;
                                cmd_list.CopyFrom(
                                    std::span<byte>(
                                        (byte*)temp_buffer.data() + temp_size, _dst.GetByteSize()
                                    ),
                                    view
                                );
                                temp_size += cmd.SizeByte();
                            }
                        },
                        cmd.dst
                    );
                }
                for (auto& cmd : task->mem_cmds) {
                    std::visit(
                        Overload{
                            [&](RawDataDesc& _src) {},
                            [&](BufferViewDesc& _dst) {
                                const RawDataDesc& src    = std::get<RawDataDesc>(cmd.src);
                                VulkanBuffer*      buffer = (VulkanBuffer*)_dst.handle;
                                cmd_list.CopyFrom(
                                    std::span<byte>((byte*)src.data.data(), src.data.size_bytes()),
                                    buffer->GetView(_dst.offset, _dst.size)
                                );
                            },
                            [&](TextureViewDesc& _dst) {
                                const RawDataDesc& src     = std::get<RawDataDesc>(cmd.src);
                                VulkanTexture*     texture = (VulkanTexture*)_dst.handle;
                                TextureView view(texture, _dst.pixel_fmt, _dst.mip_offset, _dst.mip_cnt);
                                view.offset = _dst.offset;
                                view.extent = _dst.size;
                                cmd_list.CopyFrom(
                                    std::span<byte>((byte*)src.data.data(), src.data.size_bytes()), view
                                );
                            }
                        },
                        cmd.dst
                    );
                }

                // Execute the command list
                auto& copy_queue = task->session->io_interface.GetQueue();
                auto  evt        = copy_queue.Execute(std::move(cmd_list.Submit()));
                copy_queue.Sync(evt.timeline);
                task->callback();

            } else {
                std::this_thread::yield();
            }
        }
    }
};

static constexpr uint64_t max_chunk_size = 1024 * 1024 * 64; // 64MB
VulkanIOInterface::VulkanIOInterface(VulkanDevice& _device, VkCopyQueue& _queue) :
    m_device(_device),
    m_queue(_queue) {
    storage = MoerNew(VulkanStorage)(_device, _queue);
}

VulkanIOInterface::~VulkanIOInterface() {
    MoerDelete(storage);
}

void VulkanIOInterface::Execute(IOCommandList&& _cmdlist, uint64 _timeline) {
    auto split_commands = [&](Array<IOCmd>& _cmds, Array<IOCmd>& _splited_cmds) {
        auto cnt = _cmds.size();
        for (auto i = 0; i < cnt; i++) {
            auto& cmd  = _cmds[i];
            auto  size = cmd.SizeByte();
            if (size < max_chunk_size)
                continue;

            auto cur_cmd = std::move(cmd);
            if (i < cnt - 1) {
                cmd = std::move(_cmds.back());
            }
            _cmds.pop_back();
            i--;
            cnt--;
            if (size == 0)
                continue;

            std::visit(
                Overload{
                    [&](RawDataDesc& _dst) {
                        do {

                            auto sub_buffer_size = std::min(_dst.data.size_bytes(), max_chunk_size);
                            auto sub_buffer      = _dst.data.subspan(0, sub_buffer_size);
                            _splited_cmds.emplace_back(
                                cur_cmd.src, cur_cmd.dst, cur_cmd.file_offset, cur_cmd.flags
                            );

                            if (_dst.data.size_bytes() - sub_buffer_size > 0) {
                                cur_cmd.file_offset += sub_buffer_size;
                                _dst.data = _dst.data.subspan(sub_buffer_size);
                            } else {
                                break;
                            }
                        } while (true);
                    },
                    [&](BufferViewDesc& _dst) {
                        // Process buffer to buffer copy
                        do {
                            auto sub_buffer_size = std::min(uint64(_dst.size), max_chunk_size);
                            auto buffer_view     = _dst;
                            buffer_view.size     = sub_buffer_size;
                            _splited_cmds.emplace_back(
                                cur_cmd.src, buffer_view, cur_cmd.file_offset, cur_cmd.flags
                            );
                            if (_dst.size - sub_buffer_size > 0) {
                                cur_cmd.file_offset += sub_buffer_size;
                                _dst.offset += sub_buffer_size;
                                _dst.size -= sub_buffer_size;
                            } else {
                                break;
                            }
                        } while (true);
                    },
                    [&](TextureViewDesc& _dst) {
                        auto size = GetSizeFromImageFormat(_dst.pixel_fmt, uint3(_dst.size.xy, 1));
                        if (size <= max_chunk_size) {
                            for (auto i = 0; i < _dst.size[2]; i++) {
                                _splited_cmds.emplace_back(
                                    cur_cmd.src,
                                    TextureViewDesc{
                                        uint3(_dst.offset.xy, _dst.offset.z + i),
                                        uint3(_dst.size.xy, 1),
                                        _dst.handle,
                                        _dst.pixel_fmt,
                                        _dst.mip_offset,
                                        _dst.mip_cnt,
                                        _dst.array_offset,
                                        _dst.array_cnt
                                    },
                                    cur_cmd.file_offset,
                                    cur_cmd.flags
                                );
                                cur_cmd.file_offset += size;
                            }
                        } else {
                            //for each row
                            uint block_height = _dst.size.y;
                            bool is_bc        = IsPixelFormatBC(_dst.pixel_fmt);
                            if (is_bc) {
                                block_height >>= 2;
                            }
                            auto row_size = size / block_height;
                            auto col_size = max_chunk_size / row_size;
                            if (is_bc)
                                col_size <<= 2;
                            for (auto i = 0; i < _dst.size[2]; i++) {
                                uint col_offset = 0;
                                do {
                                    auto dst_col = std::min(uint(col_size), _dst.size.y - col_offset);
                                    _splited_cmds.emplace_back(
                                        cur_cmd.src,
                                        TextureViewDesc{
                                            uint3(
                                                _dst.offset.x, _dst.offset.y + col_offset, _dst.offset.z + i
                                            ),
                                            uint3(_dst.size.x, dst_col, 1),
                                            _dst.handle,
                                            _dst.pixel_fmt,
                                            _dst.mip_offset,
                                            _dst.mip_cnt,
                                            _dst.array_offset,
                                            _dst.array_cnt
                                        },
                                        cur_cmd.file_offset,
                                        cur_cmd.flags
                                    );

                                    col_offset += dst_col;
                                    if (col_offset < _dst.size.y) {
                                        cur_cmd.file_offset += GetSizeFromImageFormat(
                                            _dst.pixel_fmt, uint3(_dst.size.x, dst_col, 1)
                                        );
                                    } else {
                                        break;
                                    }
                                } while (true);
                            }
                        }
                    }
                },
                cur_cmd.dst
            );
        }
    };
    Array<IOCmd>      splited_cmds;
    Array<IOCmd>      src_cmds     = std::move(_cmdlist.cmds);
    Array<FileHandle> files        = std::move(_cmdlist.files);
    Array<IOCallBack> callbacks    = std::move(_cmdlist.callbacks);
    bool              need_execute = false;

    auto&& scope_exit = OnScopeExit([&]() {
        if (need_execute) {
            storage->EnqueueSignal(event, _timeline);
            storage->Commit();
        }
        if (files.empty() && callbacks.empty())
            return;
        this->callbacks.emplace(std::move(callbacks), std::move(files), _timeline);
    });

    if (src_cmds.empty()) {
        return;
    }
    need_execute = true;
    split_commands(src_cmds, splited_cmds);
    uint64 cmd_size = src_cmds.size();

    Array<std::pair<IOCmd, uint64>> frag_cmds;
    Array<std::pair<IOCmd, uint64>> staging_cmds;
    auto                            divide_cmds = [&](Array<IOCmd>& _cmds) {
        for (auto& cmd : _cmds) {
            if (cmd.SizeByte() < max_chunk_size) {
                frag_cmds.push_back({cmd, cmd_size});
            } else {
                staging_cmds.push_back({cmd, cmd_size});
            }
        }
    };

    divide_cmds(src_cmds);
    divide_cmds(splited_cmds);
    std::sort(frag_cmds.begin(), frag_cmds.end(), [](auto& _lhs, auto& _rhs) {
        return _lhs.second < _rhs.second;
    });
    auto commit = [&]() {
        if (!need_execute)
            return;
        storage->Commit();
        need_execute = false;
    };

    if (!frag_cmds.empty()) {
        uint64 byte_offset = 0;
        for (auto& cmd : frag_cmds) {
            auto& src = cmd.first.src;
            auto& dst = cmd.first.dst;
            if (cmd.second > max_chunk_size - byte_offset) {
                commit();
                byte_offset = 0;
            }
            need_execute = true;
            std::visit(
                Overload{
                    [&](RawDataDesc& _src, BufferViewDesc& _dst) {

                    },
                    [&](RawDataDesc& _src, TextureViewDesc& _dst) {
                        storage->Enqueue(
                            _src.data.data(),
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, BufferViewDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, TextureViewDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, RawDataDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.data.data(),
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](auto& _src, auto& _dst) {
                        assert(false && "Invalid command");
                    }
                },
                cmd.first.src,
                cmd.first.dst
            );

            byte_offset += cmd.second;
        }
    }

    if (!staging_cmds.empty()) {
        for (auto& cmd : staging_cmds) {
            auto& src = cmd.first.src;
            auto& dst = cmd.first.dst;
            commit();
            need_execute = true;
            std::visit(
                Overload{
                    [&](RawDataDesc& _src, BufferViewDesc& _dst) {
                        storage->Enqueue(
                            _src.data.data(),
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](RawDataDesc& _src, TextureViewDesc& _dst) {
                        storage->Enqueue(
                            _src.data.data(),
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, BufferViewDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, TextureViewDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.handle,
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](FileDesc& _src, RawDataDesc& _dst) {
                        storage->Enqueue(
                            _src.handle,
                            cmd.first.file_offset,
                            (void*)_dst.data.data(),
                            cmd.first.file_offset,
                            cmd.first.SizeByte()
                        );
                    },
                    [&](auto& _src, auto& _dst) {
                        assert(false && "Invalid command");
                    }
                },
                src,
                dst
            );
        }
    }
}

uint64 VulkanIOInterface::Execute(IOCommandList&& _cmdlist) {
    uint64 timline;
    if (_cmdlist.callbacks.empty() && _cmdlist.files.empty() && _cmdlist.cmds.empty() &&
        _cmdlist.import_cmds.empty() && _cmdlist.export_cmds.empty()) {
        timline = this->timeline;
        return timline;
    }
    timline = ++this->timeline;
    {
        std::unique_lock<std::mutex> lock(mut);
        io_cmdlists.emplace(std::move(_cmdlist), timline);
    }
    return timline;
}

WaitEvent VulkanIOInterface::GetWaitEvent(uint64 _timeline) {
    return {uint64(event.Get()), timeline};
}

void VulkanIOInterface::Sync(uint64 _timeline) {
    event->Wait(_timeline);
}

void VulkanIOInterface::Tick() {
    {
        std::unique_lock<std::mutex> lock(mut);
        if (!io_cmdlists.empty()) {
            auto& cmd_list_pair = io_cmdlists.front();
            Execute(std::move(cmd_list_pair.first), cmd_list_pair.second);
            io_cmdlists.pop();
        }
    }
    if (!callbacks.empty()) {
        if (event->GetValue() >= callbacks.front().timeline) {
            auto& callback = callbacks.front();
            for (auto& cb : callback.callback) {
                cb();
            }
            callback.Clear();
            callbacks.pop();
        }
    } else {
        //yield if no callback
        std::this_thread::yield();
    }
}

VulkanStorage::VulkanStorage(VulkanDevice& _device, VkCopyQueue& _queue) : copy_queue(_queue) {
    task_thread = MoerNew(VulkanIOTaskThread)();
}

VulkanStorage::~VulkanStorage() {
    MoerDelete(task_thread);
    task_thread = nullptr;
}

VulkanCommitSession* VulkanStorage::GetCurrentSession() {
    if (current_session && !current_session->IsComplete()) {
        return current_session;
    }
    assert(!current_session && "current session not signaled!");
    current_session = MoerNew(VulkanCommitSession)(*this);
    return current_session;
}

VkCopyQueue& VulkanStorage::GetQueue() {
    return copy_queue;
}

void VulkanStorage::CommitIOTask(VulkanIOTask* _tsk) {
    task_thread->queue.Push(_tsk);
}

void VulkanStorage::Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
    GetCurrentSession()->Enqueue(_handle, _file_offset, _ptr, _len);
}
void VulkanStorage::Enqueue(
    FileHandle _handle,
    size_t     _file_offset,
    void*      _buffer_ptr,
    size_t     _offset,
    size_t     _len
) {
    GetCurrentSession()->Enqueue(_handle, _file_offset, _buffer_ptr, _offset, _len);
}
void VulkanStorage::Enqueue(
    FileHandle   _handle,
    size_t       _file_offset,
    void*        _tex_ptr,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip_offset
) {
    GetCurrentSession()->Enqueue(_handle, _file_offset, _tex_ptr, _format, _offset, _size, _mip_offset);
}
void VulkanStorage::Enqueue(
    void const* _mem,
    size_t      _file_offset,
    void*       _buffer_ptr,
    size_t      _offset,
    size_t      _len
) {
    GetCurrentSession()->Enqueue(_mem, _file_offset, _buffer_ptr, _offset, _len);
}
void VulkanStorage::Enqueue(
    void const*  _mem,
    size_t       _file_offset,
    void*        _tex_ptr,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip_offset
) {
    GetCurrentSession()->Enqueue(_mem, _file_offset, _tex_ptr, _format, _offset, _size, _mip_offset);
}
void VulkanStorage::EnqueueSignal(FenceRef _fence, uint64_t _timeline) {
    GetCurrentSession()->EnqueueSignal(_fence, _timeline);
}
void VulkanStorage::Commit() {
    GetCurrentSession()->Commit();
}

} // namespace Moer::Render