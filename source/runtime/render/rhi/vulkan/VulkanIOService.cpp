#include "VulkanIOService.h"
#include "VulkanDevice.h"
#include "rhi/RHIExecutor.h"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <stdexcept>
#include <thread>

namespace Moer::Render {
namespace {

struct FileSource {
    std::string path;
    uint64      offset;
};
struct BufferTarget {
    BufferRef buffer;
    uint64    offset;
};
struct TextureTarget {
    TextureRef  texture;
    TextureView view;
};
struct IORequest {
    std::variant<FileSource, Array<byte>>                      source;
    std::variant<std::span<byte>, BufferTarget, TextureTarget> target;
    uint64                                                     size;
};
struct IOSignal {
    FenceRef fence;
    uint64   value;
};

void LogIOError(const char* _message) noexcept {
    try {
        LOG_ERROR("[VulkanIO] {}", _message);
    } catch (...) {
    }
}

struct FileListCleanup {
    Array<FileHandle> files;
    ~FileListCleanup() {
        for (const auto& file : files) {
            Memory::Free(file.file);
        }
    }
};

struct IOBatch {
    Array<IORequest>  requests;
    Array<IOSignal>   signals;
    Array<IOCallBack> callbacks;
    FileListCleanup   files;
    uint64            bytes     = 0;
    std::atomic_bool  completed = false;

    // Also used for failures before the generic executor accepts the batch.
    // Once accepted, its ordinary Completion path owns this operation.
    bool Complete(bool _reject = false) noexcept {
        if (completed.exchange(true)) {
            return false;
        }
        if (_reject) {
            for (const auto& signal : signals) {
                signal.fence->Reject(signal.value);
            }
        }
        for (auto& callback : callbacks) {
            try {
                callback();
            } catch (...) {
                LogIOError("completion callback threw");
            }
        }
        callbacks.clear();
        requests.clear();
        signals.clear();
        return true;
    }
};

uint64 CheckedAdd(uint64 _a, uint64 _b) {
    if (_b > std::numeric_limits<uint64>::max() - _a) {
        throw std::overflow_error("IO byte range overflows");
    }
    return _a + _b;
}

IORequest OwnRequest(const IOCmd& _command) {
    if (_command.flags != 0) {
        throw std::invalid_argument("IO flags are not supported");
    }
    IORequest request;
    request.size   = _command.SizeByte();
    request.target = std::visit(
        Overload{
            [](const RawDataDesc& _dst) -> decltype(request.target) {
                if (!_dst.data.data() && !_dst.data.empty()) {
                    throw std::invalid_argument("null IO CPU destination");
                }
                return std::span<byte>(reinterpret_cast<byte*>(_dst.data.data()), _dst.data.size());
            },
            [&request](const BufferViewDesc& _dst) -> decltype(request.target) {
                auto* buffer = reinterpret_cast<Buffer*>(_dst.handle);
                if (!buffer || _dst.offset > buffer->GetByteSize() ||
                    request.size > buffer->GetByteSize() - _dst.offset || _dst.offset % 4 != 0 ||
                    request.size % 4 != 0) {
                    throw std::invalid_argument("invalid IO buffer copy range");
                }
                return BufferTarget{BufferRef(buffer), _dst.offset};
            },
            [](const TextureViewDesc& _dst) -> decltype(request.target) {
                auto* texture = reinterpret_cast<Texture*>(_dst.handle);
                if (!texture || texture->GetFormat() != _dst.pixel_fmt || _dst.mip_cnt > 1 ||
                    _dst.array_cnt > 1 || _dst.mip_offset >= texture->GetNumMips() || _dst.mip_offset >= 32 ||
                    _dst.array_offset >= texture->GetNumArray() || texture->GetNumSamples() != 1) {
                    throw std::invalid_argument("IO texture copy requires one matching subresource");
                }
                const auto  format = GetPixelFormatInfo(_dst.pixel_fmt);
                const auto  base   = texture->GetExtent();
                const uint3 extent(
                    std::max(uint(base.x) >> _dst.mip_offset, 1u),
                    std::max(uint(base.y) >> _dst.mip_offset, 1u),
                    std::max(uint(base.z) >> _dst.mip_offset, 1u)
                );
                const auto valid = [](uint _offset, uint _size, uint _extent, uint _block) {
                    return _block && _size && _offset < _extent && _size <= _extent - _offset &&
                           _offset % _block == 0 && (_size % _block == 0 || _offset + _size == _extent);
                };
                if (!valid(_dst.offset.x, _dst.size.x, extent.x, format.block_width) ||
                    !valid(_dst.offset.y, _dst.size.y, extent.y, format.block_height) ||
                    !valid(_dst.offset.z, _dst.size.z, extent.z, format.block_depth)) {
                    throw std::invalid_argument("invalid IO texture copy region");
                }
                TextureView view(texture, _dst.pixel_fmt, _dst.mip_offset, 1);
                view.array_layer = _dst.array_offset;
                view.num_array   = 1;
                view.offset      = _dst.offset;
                view.extent      = _dst.size;
                return TextureTarget{TextureRef(texture), view};
            }
        },
        _command.dst
    );
    request.source = std::visit(
        Overload{
            [&](const FileDesc& _src) -> decltype(request.source) {
                if (!_src.handle.file || _command.file_offset > _src.size ||
                    request.size > _src.size - _command.file_offset) {
                    throw std::invalid_argument("invalid IO file source range");
                }
                const uint64 offset = CheckedAdd(_src.offset, _command.file_offset);
                if (CheckedAdd(offset, request.size) > uint64(std::numeric_limits<int64_t>::max())) {
                    throw std::invalid_argument("IO file range exceeds signed 64-bit offsets");
                }
                return FileSource{static_cast<const char*>(_src.handle.file), offset};
            },
            [&](const RawDataDesc& _src) -> decltype(request.source) {
                if ((!_src.data.data() && !_src.data.empty()) || _command.file_offset > _src.data.size() ||
                    request.size > _src.data.size() - _command.file_offset) {
                    throw std::invalid_argument("invalid IO memory source range");
                }
                Array<byte> data(request.size);
                if (!data.empty()) {
                    std::memcpy(data.data(), _src.data.data() + _command.file_offset, data.size());
                }
                return data;
            }
        },
        _command.src
    );
    return request;
}

void AddRequest(IOBatch& _batch, const IOCmd& _command) {
    IORequest    request = OwnRequest(_command);
    const uint64 bytes   = CheckedAdd(_batch.bytes, request.size);
    _batch.requests.push_back(std::move(request));
    _batch.bytes = bytes;
}

void ReadFile(const FileSource& _source, std::span<byte> _destination) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(_source.path.c_str(), "rb"), &std::fclose);
    if (!file) {
        throw std::runtime_error("cannot open IO source: " + _source.path);
    }
#ifdef _WIN32
    const int seek_result = _fseeki64(file.get(), static_cast<int64_t>(_source.offset), SEEK_SET);
#else
    const int seek_result = fseeko(file.get(), static_cast<off_t>(_source.offset), SEEK_SET);
#endif
    if (seek_result != 0 ||
        std::fread(_destination.data(), 1, _destination.size(), file.get()) != _destination.size()) {
        throw std::runtime_error("seek or short read in IO source: " + _source.path);
    }
}

RawDataDesc MemorySource(const void* _memory, size_t _offset, size_t _size) {
    if (!_memory && _size != 0) {
        throw std::invalid_argument("null IO memory source");
    }
    auto* data = const_cast<uint8_t*>(static_cast<const uint8_t*>(_memory));
    return {std::span<uint8_t>(data ? data + _offset : nullptr, _size)};
}

TextureViewDesc
TextureDestination(void* _texture, EPixelFormat _format, uint3 _offset, uint3 _size, uint _mip) {
    if (_mip > std::numeric_limits<uint8>::max()) {
        throw std::invalid_argument("invalid IO mip level");
    }
    return {_offset, _size, uint64(_texture), _format, _mip, 1, 0, 1};
}

} // namespace

struct VulkanStorage::Impl {
    // This bounds concurrent prepared/upload payloads, not the caller's queued
    // memory snapshots or the generic allocator's retained pool capacity.
    // An oversized batch runs alone; it is never split into partial commits.
    static constexpr uint64              byte_budget = 128ull * 1024 * 1024;
    static constexpr size_t              batch_limit = 32;
    std::shared_ptr<IOBatch>             pending     = std::make_shared<IOBatch>();
    std::mutex                           mutex;
    std::condition_variable              changed;
    std::deque<std::shared_ptr<IOBatch>> queue;
    uint64                               in_flight_bytes   = 0;
    size_t                               in_flight_batches = 0;
    bool                                 stopping          = false;
    std::jthread                         thread;

    Impl() :
        thread([this] {
            Run();
        }) {}
    ~Impl() {
        pending->Complete(true);
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        thread.join();
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] {
            return in_flight_batches == 0;
        });
    }

    void Publish(const std::shared_ptr<IOBatch>& _batch) {
        {
            std::lock_guard lock(mutex);
            queue.push_back(_batch);
        }
        changed.notify_all();
    }

    void Complete(const std::shared_ptr<IOBatch>& _batch, bool _reject = false) noexcept {
        if (_batch->Complete(_reject)) {
            std::lock_guard lock(mutex);
            in_flight_bytes -= _batch->bytes;
            --in_flight_batches;
            changed.notify_all();
        }
    }

    void Run() noexcept {
        for (;;) {
            std::shared_ptr<IOBatch> batch;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return stopping || !queue.empty();
                });
                if (queue.empty()) {
                    return;
                }
                const uint64 bytes = queue.front()->bytes;
                changed.wait(lock, [this, bytes] {
                    return in_flight_batches == 0 ||
                           (in_flight_batches < batch_limit && bytes <= byte_budget &&
                            in_flight_bytes <= byte_budget - bytes);
                });
                batch = std::move(queue.front());
                queue.pop_front();
                in_flight_bytes += bytes;
                ++in_flight_batches;
            }
            try {
                CommandList commands(EQueueType::Copy);
                for (auto& request : batch->requests) {
                    if (request.size == 0) {
                        continue;
                    }
                    if (auto* destination = std::get_if<std::span<byte>>(&request.target)) {
                        if (auto* file = std::get_if<FileSource>(&request.source)) {
                            ReadFile(*file, *destination);
                        } else {
                            const auto& data = std::get<Array<byte>>(request.source);
                            std::memcpy(destination->data(), data.data(), data.size());
                        }
                        continue;
                    }
                    Array<byte> data;
                    if (auto* file = std::get_if<FileSource>(&request.source)) {
                        data.resize(request.size);
                        ReadFile(*file, data);
                    } else {
                        data = std::move(std::get<Array<byte>>(request.source));
                    }
                    if (auto* buffer = std::get_if<BufferTarget>(&request.target)) {
                        commands.CopyFrom(
                            std::move(data), BufferView(buffer->buffer.Get(), buffer->offset, request.size, 1)
                        );
                    } else {
                        commands.CopyFrom(std::move(data), std::get<TextureTarget>(request.target).view);
                    }
                }
                for (const auto& signal : batch->signals) {
                    commands.Signal(signal.fence, signal.value);
                }
                commands.AddCallback([this, batch] {
                    Complete(batch);
                });
                RHIExecutor::Get().Submit(EQueueType::Copy, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
            } catch (const std::exception& error) {
                LogIOError(error.what());
                Complete(batch, true);
            } catch (...) {
                LogIOError("failed to prepare or publish IO batch");
                Complete(batch, true);
            }
        }
    }
};

VulkanStorage::VulkanStorage(VulkanDevice&, VkCopyQueue&) : impl(std::make_unique<Impl>()) {}
VulkanStorage::~VulkanStorage() = default;

void VulkanStorage::Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len) {
    AddRequest(
        *impl->pending,
        {FileDesc{_handle, _file_offset, _len},
         RawDataDesc{std::span<uint8_t>(static_cast<uint8_t*>(_ptr), _len)}}
    );
}
void VulkanStorage::Enqueue(
    FileHandle _handle,
    size_t     _file_offset,
    void*      _buffer,
    size_t     _offset,
    size_t     _len
) {
    AddRequest(
        *impl->pending,
        {FileDesc{_handle, _file_offset, _len}, BufferViewDesc{uint64(_buffer), _offset, _len}}
    );
}
void VulkanStorage::Enqueue(
    FileHandle   _handle,
    size_t       _file_offset,
    void*        _texture,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip
) {
    const auto destination = TextureDestination(_texture, _format, _offset, _size, _mip);
    AddRequest(*impl->pending, {FileDesc{_handle, _file_offset, destination.GetByteSize()}, destination});
}
void VulkanStorage::Enqueue(
    const void* _memory,
    size_t      _source_offset,
    void*       _buffer,
    size_t      _offset,
    size_t      _len
) {
    AddRequest(
        *impl->pending,
        {MemorySource(_memory, _source_offset, _len), BufferViewDesc{uint64(_buffer), _offset, _len}}
    );
}
void VulkanStorage::Enqueue(
    const void*  _memory,
    size_t       _source_offset,
    void*        _texture,
    EPixelFormat _format,
    uint3        _offset,
    uint3        _size,
    uint         _mip
) {
    const auto destination = TextureDestination(_texture, _format, _offset, _size, _mip);
    AddRequest(
        *impl->pending, {MemorySource(_memory, _source_offset, destination.GetByteSize()), destination}
    );
}
void VulkanStorage::EnqueueSignal(FenceRef _fence, uint64 _timeline) {
    if (!_fence.IsValid() || _timeline == 0) {
        throw std::invalid_argument("IO signal requires a fence and non-zero value");
    }
    impl->pending->signals.push_back({std::move(_fence), _timeline});
}
void VulkanStorage::Commit() {
    if (impl->pending->requests.empty() && impl->pending->signals.empty()) {
        return;
    }
    auto next = std::make_shared<IOBatch>();
    impl->Publish(impl->pending);
    impl->pending = std::move(next);
}

VulkanIOInterface::VulkanIOInterface(VulkanDevice& _device, VkCopyQueue& _queue) :
    event(_device.CreateFence()),
    storage(_device, _queue) {}
VulkanIOInterface::~VulkanIOInterface() = default;

uint64 VulkanIOInterface::Execute(IOCommandList&& _commands) {
    FileListCleanup files{std::move(_commands.files)};
    auto            batch = std::make_shared<IOBatch>();
    batch->callbacks      = std::move(_commands.callbacks);
    batch->files.files    = std::move(files.files);
    try {
        if (!_commands.import_cmds.empty() || !_commands.export_cmds.empty()) {
            throw std::invalid_argument("IO queue ownership transfers are not supported");
        }
        for (const auto& command : _commands.cmds) {
            AddRequest(*batch, command);
        }
        _commands.cmds.clear();
        std::lock_guard lock(mutex);
        if (batch->requests.empty() && batch->callbacks.empty() && batch->files.files.empty()) {
            return timeline;
        }
        const uint64 value = CheckedAdd(timeline, 1);
        batch->signals.push_back({event, value});
        // Admission failure rejects this value. Never reuse it on a retry.
        timeline = value;
        storage.impl->Publish(batch);
        return value;
    } catch (...) {
        batch->Complete(true);
        throw;
    }
}
WaitEvent VulkanIOInterface::GetWaitEvent(uint64 _time_stamp) {
    // The unified Submission owner cannot wait for a producer queued behind
    // its consumer. Establish native acceptance (or rejection) before exposing
    // a GPU dependency; this does not wait for the copy to finish on the GPU.
    (void)event->WaitSubmitted(_time_stamp);
    return {uint64(event.Get()), _time_stamp};
}
void VulkanIOInterface::Sync(uint64 _time_stamp) {
    event->Wait(_time_stamp);
}

} // namespace Moer::Render
