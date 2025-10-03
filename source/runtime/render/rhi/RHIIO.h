#ifndef MOER_RHI_IO_H
#define MOER_RHI_IO_H
#include "PixelFormat.h"
#include "rhi/RHIResource.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <functional>
#include <span>
#include <thread>
#include <variant>
namespace Moer::Render {

struct FileHandle {
    void*  file   = nullptr;
    size_t length = 0;
};
struct FileDesc {
    FileHandle handle;
    uint       offset;
    uint       size;
};

struct RawDataDesc {
    std::span<uint8_t> data;
};

struct BufferViewDesc {
    uint64_t handle;
    uint32_t offset;
    uint32_t size;
};

struct TextureViewDesc {
    uint3        offset;
    uint3        size;
    uint64       handle;
    EPixelFormat pixel_fmt;
    uint         mip_offset : 8;
    uint         mip_cnt : 8;
    uint         array_offset : 8;
    uint         array_cnt : 8;

    uint64 GetByteSize() const;
};
// using CmdTarget = std::variant<FileDesc, RawDataDesc, BufferViewDesc, TextureViewDesc>;
using IOSrc = std::variant<FileDesc, RawDataDesc>;
using IODst = std::variant<RawDataDesc, BufferViewDesc, TextureViewDesc>;
struct IOCmd {
    IOSrc    src;
    IODst    dst;
    uint64   file_offset;
    uint32_t flags;

    uint64 SizeByte() const {
        return std::visit(
            Overload{
                [](const RawDataDesc& _src) {
                    return _src.data.size_bytes();
                },
                [](const BufferViewDesc& _src) {
                    return uint64(_src.size);
                },
                [](const TextureViewDesc& _src) {
                    return _src.GetByteSize();
                }
            },
            dst
        );
    }
};
using IOCallBack = std::function<void(void)>;

struct IOWaitEvt {
    uint64 handle;
    uint64 timeline;
};

struct IOSignalEvt {
    uint64 handle;
    uint64 timeline;
};

struct IOSubmission {
    Array<IOCmd>      cmds;
    Array<IOCallBack> callbacks;
    Array<FileHandle> files;

    Array<IOWaitEvt>   wait_events;
    Array<IOSignalEvt> signal_events;

    IOSubmission() = default;
    IOSubmission(IOSubmission&& _other) noexcept :
        cmds(std::move(_other.cmds)),
        callbacks(std::move(_other.callbacks)),
        files(std::move(_other.files)),
        wait_events(std::move(_other.wait_events)),
        signal_events(std::move(_other.signal_events)) {}

    IOSubmission& operator=(IOSubmission&& _other) {
        if (this != &_other) {
            cmds          = std::move(_other.cmds);
            callbacks     = std::move(_other.callbacks);
            files         = std::move(_other.files);
            wait_events   = std::move(_other.wait_events);
            signal_events = std::move(_other.signal_events);
        }
        return *this;
    }

    IOSubmission(const IOSubmission&)            = delete;
    IOSubmission& operator=(const IOSubmission&) = delete;

    IOSubmission Wait(IOWaitEvt _evt) {
        wait_events.push_back(_evt);
        return std::move(*this);
    }

    IOSubmission Signal(IOSignalEvt _evt) {
        signal_events.push_back(_evt);
        return std::move(*this);
    }
};
static constexpr uint64 s_io_signal_non = 0xFFFFFFFFFFFFFFFF;

struct IOQueueCommandList {
    //todo: add file to mem
    void Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
        // file_to_mem.emplace_back(_handle, _file_offset, _ptr, _len);
    }
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

    Array<IOCmd> file_to_mem;
    Array<IOCmd> file_to_buffer;
    Array<IOCmd> file_to_texture;
    Array<IOCmd> mem_to_mem;
    Array<IOCmd> mem_to_buffer;
    Array<IOCmd> mem_to_texture;
};
struct IOQueueSubmission {
    IOQueueCommandList cmds;
    uint64             timeline = s_io_signal_non;
};

class IOCommandList {
    friend struct IOHandler;

public:
    Array<UniquePtr<class QueueTransferCmd>> import_cmds;
    Array<UniquePtr<QueueTransferCmd>>       export_cmds;
    Array<IOCmd>                             cmds;
    Array<IOCallBack>                        callbacks;
    Array<FileHandle>                        files;

public:
    void CopyFrom(const FileDesc& _src, const RawDataDesc& _dst) {
        cmds.push_back({_src, _dst, 0});
    }
    void ImportResources(
        EQueueType                  _src_queue,
        const Array<ImportTexture>& _textures,
        const Array<ImportBuffer>&  _buffers
    );
    void AddCallback(IOCallBack&& _callback) {
        callbacks.push_back(std::move(_callback));
    }

    FileHandle ResolveFileHandle(const std::filesystem::path& _path) {
        assert(std::filesystem::exists(_path) && "File does not exist");
        FileHandle handle;
        char*      path_str = (char*)Memory::Malloc(_path.string().size() + 1);
        strcpy_s(path_str, _path.string().size(), _path.string().c_str());
        path_str[_path.string().size()] = '\0';
        handle.file                     = path_str;
        handle.length                   = _path.string().size();
        files.push_back(handle);
        return handle;
    }

    IOSubmission Submit() {
        IOSubmission submission;
        submission.cmds          = std::move(cmds);
        submission.callbacks     = std::move(callbacks);
        submission.files         = std::move(files);
        submission.wait_events   = {};
        submission.signal_events = {};
        return submission;
    }
};

struct Event {
    std::atomic_int64_t timeline;
    void                Wait(uint64_t _timeline) {
        while (this->timeline.load(std::memory_order_relaxed) < _timeline) {
            std::this_thread::yield();
        }
    }
    void Signal(uint64_t _timeline) {
        if (_timeline > this->timeline.load(std::memory_order_relaxed))
            this->timeline.store(_timeline, std::memory_order_relaxed);
    }
    bool IsSignaled(uint64_t _timeline) {
        return this->timeline.load(std::memory_order_relaxed) >= _timeline;
    }
};

} // namespace Moer::Render
#endif