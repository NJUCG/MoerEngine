#include "rhi/RHIIO.h"
#include "rhi/RHI.h"
namespace Moer::Render {
class DeviceIOService : public IOInterface {
public:
    DeviceIOService(Render::RenderDevice& _device) : m_copy_queue(_device.GetCopyQueue()) {}

    uint64 Execute(IOCommandList&& _cmdlist) override {
        // TODO: unfinished code here
        LOG_WARNING(MOER_TEXT("unfinished code here!"));
        return 114514;
    }
    Render::WaitEvent GetWaitEvent(uint64 _time_stamp) override {
        return Render::WaitEvent{uint64(m_copy_queue.GetFenceHandle().Get()), _time_stamp};
    }
    void Sync(uint64_t _time_stamp) override {}

private:
    Render::CopyQueue& m_copy_queue;
};

uint64 TextureViewDesc::GetByteSize() const {
    return GetSizeFromImageFormat(pixel_fmt, size);
}
class IOLooper {
    std::jthread                     thread;
    std::mutex                       mutex;
    Array<std::function<void(void)>> requests;
    bool                             enabled = true;

public:
    IOLooper() {
        thread = std::jthread([this]() {
            InnerWorkLoop();
        });
    }
    ~IOLooper() {}

    static void Init() {
        IOLooper::Get();
    }
    static void Dispose() {
        IOLooper::Get().enabled = false;
    }
    static void EnqueueRequest(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
        IOLooper::Get().InnerEnqueueRequest(_handle, _file_offset, _ptr, _len);
    }
    static void EnqueueRequest(const void* _ptr, size_t _len, FileHandle _handle, size_t _file_offset) {
        IOLooper::Get().InnerEnqueueRequest(_ptr, _len, _handle, _file_offset);
    }
    static void EnqueueSignal(Event* _event_handle, uint64_t _timeline) {
        IOLooper::Get().InnerEnqueueSignal(_event_handle, _timeline);
    }
    static void EnqueueRequest(
        FileHandle _handle,
        size_t     _offset,
        size_t     _src_size,
        FileHandle _dst_handle,
        size_t     _dst_offset,
        size_t     _dst_size
    ) {
        IOLooper::Get().InnerEnqueueRequest(_handle, _offset, _src_size, _dst_handle, _dst_offset, _dst_size);
    }

private:
    static IOLooper& Get() {
        static IOLooper looper;
        return looper;
    }
    void InnerEnqueueRequest(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
        std::lock_guard<std::mutex> lk(mutex);
        requests.push_back([=]() {
            FILE* result_handle = nullptr;
            fopen_s(&result_handle, (const char*)_handle.file, "r");
            if (!result_handle) {
                SPDLOG_ERROR(MOER_TEXT("Failed to open file {}"), (const char*)_handle.file);
                return;
            }
            std::fseek(result_handle, _file_offset, SEEK_SET);
            std::fread(_ptr, sizeof(std::byte), _len, result_handle);
            std::fclose(result_handle);
        });
    }

    void InnerEnqueueRequest(const void* ptr, size_t len, FileHandle handle, size_t file_offset) {
        std::lock_guard<std::mutex> lk(mutex);
        requests.push_back([=]() {
            FILE* result_handle = nullptr;
            fopen_s(&result_handle, (const char*)handle.file, "r");
            if (!result_handle) {
                SPDLOG_ERROR(MOER_TEXT("Failed to open file {}"), (const char*)handle.file);
                return;
            }
            std::fseek(result_handle, file_offset, SEEK_SET);
            std::fwrite(ptr, sizeof(std::byte), len, result_handle);
            std::fclose(result_handle);
        });
    }

    void InnerEnqueueRequest(
        FileHandle _handle,
        size_t     _offset,
        size_t     _src_size,
        FileHandle _in_dst_handle,
        size_t     _dst_offset,
        size_t     _dst_size
    ) {
        std::lock_guard<std::mutex> lk(mutex);
        requests.push_back([=]() {
            FILE* src_handle = nullptr;
            fopen_s(&src_handle, (const char*)_handle.file, "r");
            FILE* dst_handle = nullptr;
            fopen_s(&dst_handle, (const char*)_in_dst_handle.file, "r+");
            if (!src_handle || !dst_handle) {
                SPDLOG_ERROR(MOER_TEXT("Failed to open file {}"), (const char*)_handle.file);
                return;
            }
            std::fseek(src_handle, _offset, SEEK_SET);
            std::fseek(dst_handle, _dst_offset, SEEK_SET);
            // use fixed size buffer for now
            char   buffer[4096];
            size_t read_size = 0;
            while (read_size < _src_size) {
                size_t to_read = std::min(sizeof(buffer), _src_size - read_size);
                size_t read    = std::fread(buffer, 1, to_read, src_handle);
                if (read == 0) {
                    break;
                }
                std::fwrite(buffer, 1, read, dst_handle);
                read_size += read;
            }
            std::fclose(src_handle);
            std::fclose(dst_handle);
        });
    }
    void InnerEnqueueSignal(Event* _event_handle, uint64_t _timeline) {
        std::lock_guard<std::mutex> lk(mutex);
        requests.push_back([=, this]() {
            _event_handle->Signal(_timeline);
        });
    }
    void InnerWorkLoop() {
        SPDLOG_INFO(MOER_TEXT("IOLooper started"));
        while (enabled) {
            Array<std::function<void(void)>> requests_copy;
            {
                std::lock_guard<std::mutex> lk(mutex);
                requests_copy = std::move(requests);
            }
            if (requests_copy.empty()) {
                std::this_thread::yield();
            }
            for (auto& request : requests_copy) {
                request();
            }
        }
        SPDLOG_INFO(MOER_TEXT("IOLooper exited"));
    }
};

struct IOCommandListHolder {
    Array<IOCmd>      cmds;
    Array<IOCallBack> callbacks;
    Array<FileHandle> files;
    uint64_t          time_stamp;
};

struct IOHandler {
    struct CallBacks {
        Array<IOCallBack> callbacks;
        Array<FileHandle> files;
        uint64_t          time_stamp;
    };
    uint64_t                   time_stamp;
    Queue<IOCommandListHolder> cmd_batches;
    std::mutex                 mutex;
    Event                      event;
    uint64_t                   EnqueueCmds(IOCommandList& cmd_list) {
        if (cmd_list.cmds.empty()) {
            return time_stamp;
        }
        {
            std::unique_lock<std::mutex> lk(mutex);
            cmd_batches.emplace(
                std::move(cmd_list.cmds),
                std::move(cmd_list.callbacks),
                std::move(cmd_list.files),
                ++time_stamp
            );
        }
        return time_stamp;
    }
    Queue<CallBacks> m_callbacks;
    void             Tick() {
        IOCommandListHolder cmds_batch;
        bool                has_cmds = false;
        {
            std::unique_lock<std::mutex> lk(mutex);
            if (!cmd_batches.empty()) {
                has_cmds   = true;
                cmds_batch = std::move(cmd_batches.front());
                cmd_batches.pop();
            }
        }
        if (has_cmds) {
            AsyncExecuteCmds(cmds_batch);
        }
        if (!m_callbacks.empty()) {
            auto& first = m_callbacks.front();
            if (event.IsSignaled(first.time_stamp)) {
                for (auto& callback : first.callbacks) {
                    callback();
                }
                for (auto& file : first.files) {
                    Memory::Free(file.file);
                }
                m_callbacks.pop();
                ;
            }
        } else {
            std::this_thread::yield();
        }
    }

    void Join() {
        Tick();
        while (!m_callbacks.empty()) {
            auto& first = m_callbacks.front();
            event.Wait(first.time_stamp);
            for (auto& callback : first.callbacks) {
                callback();
            }
            for (auto& file : first.files) {
                Memory::Free(file.file);
            }
            m_callbacks.pop();
        }
    }

private:
    void AsyncExecuteCmds(IOCommandListHolder& _cmd_holder) {
        auto&& cmds         = std::move(_cmd_holder.cmds);
        auto&& callbacks    = std::move(_cmd_holder.callbacks);
        auto&& files        = std::move(_cmd_holder.files);
        bool   has_commands = false;

        if (cmds.empty()) {
            return;
        }
        auto exit_func = OnScopeExit([&]() {
            m_callbacks.push({std::move(callbacks), std::move(files), _cmd_holder.time_stamp});
        });
        // iterate over commands
        for (auto& cmd : cmds) {
            has_commands = true;
            std::visit(
                [&](auto&& _src, auto&& _dst) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(_src)>, FileDesc>) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(_dst)>, FileDesc>) {
                            IOLooper::EnqueueRequest(
                                _src.handle, _src.offset, _src.size, _dst.handle, _dst.offset, _dst.size
                            );
                        } else {
                            // IOLooper::EnqueueRequest(_src.handle, _src.offset, _dst.data.data(), _dst.data.size());
                        }
                    } else {
                        if constexpr (std::is_same_v<std::decay_t<decltype(_dst)>, FileDesc>) {
                            IOLooper::EnqueueRequest(
                                _src.data.data(), _src.data.size(), _dst.handle, _dst.offset
                            );
                        } else {
                            SPDLOG_ERROR(MOER_TEXT("Invalid command"));
                        }
                    }
                },
                cmd.src,
                cmd.dst
            );
        }

        IOLooper::EnqueueSignal(&event, _cmd_holder.time_stamp);
    }
};
struct IOService::Impl {
    std::jthread*           thread;
    IOHandler               handler;
    bool                    requested_exit = false;
    static IOService::Impl& Get() {
        static IOService::Impl impl;
        return impl;
    }

    using time_stamp = uint32_t;
    void WorkLoop() {
        while (!requested_exit) {
            handler.Tick();
        }
        handler.Join();
    }
    Impl() {
        IOLooper::Init();
        thread = MoerNew(std::jthread)([this]() {
            WorkLoop();
        });
    }
    void Dispose() {
        requested_exit = true;
        MoerDelete(thread);
        IOLooper::Dispose();
    }
    void Sync(uint64_t _time_stamp) {
        handler.event.Wait(_time_stamp);
    }
    IOService* CreateGPUService(Render::CopyQueue* _copy_queue) {
        return nullptr;
    }
};

void IOService::Init() {
    IOService::Impl::Get();
}
void IOService::Dispose() {
    IOService::Impl::Get().Dispose();
}
void IOService::Sync(uint64_t _time_stamp) {
    IOService::Impl::Get().Sync(_time_stamp);
}
uint64_t IOService::Execute(IOCommandList& _cmd_list) {
    return IOService::Impl::Get().handler.EnqueueCmds(_cmd_list);
}

IOInterface* IOService::CreateGPUService(Render::CopyQueue* _copy_queue) {
    return MoerNew(DeviceIOService)(Render::RenderDevice::Get());
}
} // namespace Moer::Render