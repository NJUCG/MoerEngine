#pragma once
#include "misc/Traits.h"
#include <span>
#include <variant>
#include <functional>
#include <misc/STL.h>
#include <filesystem>
namespace Moer {

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
        uint3 offset;
        uint3 size;
        uint  pixel_fmt;
        uint  mip_level;
    };
    using CmdTarget = std::variant<FileDesc, RawDataDesc>;
    struct IOCmd {
        CmdTarget src;
        CmdTarget dst;
        uint32_t  flags;
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
        IOSubmission(IOSubmission&& _other) noexcept
            : cmds(std::move(_other.cmds)),
              callbacks(std::move(_other.callbacks)),
              files(std::move(_other.files)),
              wait_events(std::move(_other.wait_events)),
              signal_events(std::move(_other.signal_events)) {
        }

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

    class IOCommandList {
        friend struct IOHandler;
        Array<IOCmd>      cmds;
        Array<IOCallBack> callbacks;
        Array<FileHandle> files;

    public:
        void CopyFrom(const FileDesc& _src, const RawDataDesc& _dst) {
            cmds.push_back({_src, _dst, 0});
        }
        void CopyFrom(const RawDataDesc& _src, const FileDesc& _dst) {
            cmds.push_back({_src, _dst, 0});
        }
        void CopyFrom(const FileDesc& _src, const FileDesc& _dst) {
            cmds.push_back({_src, _dst, 0});
        }
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

}// namespace Moer