#ifndef MOER_RHI_IMPL_H
#define MOER_RHI_IMPL_H
#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include <variant>
namespace Moer::Render {

struct ComponentShuffleShader : public ComputePipeline {
    struct Arg {
        uint stride;
        uint component_cnt;
    };

    DEFINE_COMPUTE_PIPELINE_CLASS(ComponentShuffleShader);

    DEFINE_SHADER_CONSTANT_STRUCT(Arg, args);
    DEFINE_SHADER_BUFFER(indices);
    DEFINE_SHADER_BUFFER(src);
    DEFINE_SHADER_BUFFER(dst);

    DEFINE_SHADER_ARGS(args, indices, src, dst);
};

struct DeviceInternalShaders {
    ComponentShuffleShader sd_component_shuffle{};
};

struct UploadBufferCmd : public Command {
private:
    uint64                                           handle{};
    uint64                                           offset{};
    uint64                                           byte_size{};
    std::variant<Array<byte>, std::span<const byte>> storage;
    UploadBufferCmd() : Command(EType::UploadBuffer) {}

public:
    UploadBufferCmd(
        uint64           _handle,
        uint64           _offset,
        uint64           _byte_size,
        const void*      _data,
        StringView _name = typenames[uint(EType::UploadBuffer)]
    ) :
        Command(EType::UploadBuffer, _name),
        handle(_handle),
        offset(_offset),
        byte_size(_byte_size),
        storage(std::span<const byte>(reinterpret_cast<const byte*>(_data), _byte_size)) {}

    UploadBufferCmd(
        uint64           _handle,
        uint64           _offset,
        uint64           _byte_size,
        Array<byte>&&    _data,
        StringView _name = typenames[uint(EType::UploadBuffer)]
    ) :
        Command(EType::UploadBuffer, _name),
        handle(_handle),
        offset(_offset),
        byte_size(_byte_size),
        storage(std::move(_data)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }
    auto Handle() const {
        return handle;
    }
    auto Offset() const {
        return offset;
    }
    auto ByteSize() const {
        return byte_size;
    }
    auto Data() const {
        return std::visit(
            [](auto&& _data) -> std::span<const byte> {
                if constexpr (std::is_same_v<std::decay_t<decltype(_data)>, Array<byte>>) {
                    return std::span<const byte>(_data.data(), _data.size());
                } else {
                    return _data;
                }
            },
            storage
        );
    }

    mutable BufferView staging_buffer;
};

struct CopyBackBufferCmd : public Command {
private:
    uint64      handle{};
    uint64      offset{};
    uint64      byte_size{};
    void* const data{};
    GraphEventRef completion_event{nullptr};
    CopyBackBufferCmd() : Command(EType::CopyBackBuffer) {}

public:
    CopyBackBufferCmd(
        uint64           _handle,
        uint64           _offset,
        uint64           _byte_size,
        void* const      _data,
        GraphEventRef    _completion_event = nullptr,
        StringView _name = typenames[uint(EType::CopyBackBuffer)]
    ) :
        Command(EType::CopyBackBuffer, _name),
        handle(_handle),
        offset(_offset),
        byte_size(_byte_size),
        data(_data),
        completion_event(std::move(_completion_event)) {}

    //generate getters
    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }
    auto Handle() const {
        return handle;
    }
    auto Offset() const {
        return offset;
    }
    auto ByteSize() const {
        return byte_size;
    }
    auto Data() const {
        return data;
    }
    const GraphEventRef& CompletionEvent() const {
        return completion_event;
    }

    mutable BufferView staging_buffer;
};

struct CopyBackTextureCmd : public Command {
private:
    uint64          handle{};
    uint            mip_level{};
    uint3           offset{};
    uint3           size{};
    std::span<byte> data;
    GraphEventRef   completion_event{nullptr};
    CopyBackTextureCmd() : Command(EType::CopyBackTexture) {}

public:
    CopyBackTextureCmd(
        uint64           _handle,
        uint             _mip_level,
        uint3            _offset,
        uint3            _size,
        std::span<byte>  _data,
        GraphEventRef    _completion_event = nullptr,
        StringView _name = typenames[uint(EType::CopyBackTexture)]
    ) :
        Command(EType::CopyBackTexture, _name),
        handle(_handle),
        mip_level(_mip_level),
        offset{_offset.x, _offset.y, _offset.z},
        size{_size.x, _size.y, _size.z},
        data(_data),
        completion_event(std::move(_completion_event)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto Handle() const {
        return handle;
    }
    auto MipLevel() const {
        return mip_level;
    }
    auto Offset() const {
        return offset;
    }
    auto Size() const {
        return size;
    }
    auto Data() const {
        return data;
    }
    const GraphEventRef& CompletionEvent() const {
        return completion_event;
    }

    mutable BufferView staging_buffer;
};

struct CopyBufferCmd : public Command {
public:
    uint64 src_handle{};
    uint64 dst_handle{};
    uint64 src_offset{};
    uint64 dst_offset{};
    uint64 byte_size{};

private:
    CopyBufferCmd() : Command(EType::BufferToBuffer) {}

public:
    CopyBufferCmd(
        uint64           _src_handle,
        uint64           _dst_handle,
        uint64           _src_offset,
        uint64           _dst_offset,
        uint64           _byte_size,
        StringView _name = typenames[uint(EType::CopyBackBuffer)]
    ) :
        Command(EType::BufferToBuffer, _name),
        src_handle(_src_handle),
        dst_handle(_dst_handle),
        src_offset(_src_offset),
        dst_offset(_dst_offset),
        byte_size(_byte_size) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto SrcHandle() const {
        return src_handle;
    }
    auto DstHandle() const {
        return dst_handle;
    }
    auto SrcOffset() const {
        return src_offset;
    }
    auto DstOffset() const {
        return dst_offset;
    }
    auto ByteSize() const {
        return byte_size;
    }
};

struct CopyTextureCmd : public Command {
public:
    EPixelFormat format{};
    uint64       src_handle{};
    uint64       dst_handle{};
    uint         src_mip_level{};
    uint         dst_mip_level{};
    uint3        src_offset{};
    uint3        dst_offset{};
    uint3        size{};

private:
    CopyTextureCmd() : Command(EType::TextureToTexture) {}

public:
    CopyTextureCmd(
        EPixelFormat     _format,
        uint64           _src_handle,
        uint64           _dst_handle,
        uint             _src_mip_level,
        uint             _dst_mip_level,
        uint3            _src_offset,
        uint3            _dst_offset,
        uint3            _size,
        StringView _name = typenames[uint(EType::TextureToTexture)]
    ) :
        Command(EType::TextureToTexture, _name),
        format(_format),
        src_handle(_src_handle),
        dst_handle(_dst_handle),
        src_mip_level(_src_mip_level),
        dst_mip_level(_dst_mip_level),
        src_offset{_src_offset.x, _src_offset.y, _src_offset.z},
        dst_offset{_dst_offset.x, _dst_offset.y, _dst_offset.z},
        size{_size.x, _size.y, _size.z} {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto Format() const {
        return format;
    }
    auto SrcHandle() const {
        return src_handle;
    }
    auto DstHandle() const {
        return dst_handle;
    }
    auto SrcMipLevel() const {
        return src_mip_level;
    }
    auto DstMipLevel() const {
        return dst_mip_level;
    }
    auto SrcOffset() const {
        return src_offset;
    }
    auto DstOffset() const {
        return dst_offset;
    }
    auto Size() const {
        return size;
    }
};

struct CopyBufferToTextureCmd : public Command {
public:
    EPixelFormat format{};
    uint64       src_handle{};
    uint64       dst_handle{};
    uint         src_offset{};
    uint3        dst_offset{};
    uint3        size{};
    uint         mip_level{};
    uint32       array_layer{};

private:
    CopyBufferToTextureCmd() : Command(EType::BufferToTexture) {}

public:
    CopyBufferToTextureCmd(
        EPixelFormat     _format,
        uint64           _src_handle,
        uint64           _dst_handle,
        uint             _src_offset,
        uint3            _dst_offset,
        uint3            _size,
        uint             _mip_level,
        uint32           _array_layer,
        StringView _name = typenames[uint(EType::BufferToTexture)]
    ) :
        Command(EType::BufferToTexture, _name),
        format(_format),
        src_handle(_src_handle),
        dst_handle(_dst_handle),
        src_offset(_src_offset),
        dst_offset{_dst_offset.x, _dst_offset.y, _dst_offset.z},
        size{_size.x, _size.y, _size.z},
        mip_level{_mip_level},
        array_layer{_array_layer} {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto Format() const {
        return format;
    }
    auto SrcHandle() const {
        return src_handle;
    }
    auto DstHandle() const {
        return dst_handle;
    }
    auto SrcOffset() const {
        return src_offset;
    }
    auto DstOffset() const {
        return dst_offset;
    }
    auto Size() const {
        return size;
    }
    auto ByteSize() const {
        return GetSizeFromImageFormat(format, size);
    }
    auto MipLevel() const {
        return mip_level;
    }
    auto ArrayLayer() const {
        return array_layer;
    }
};

struct CopyTextureToBufferCmd : public Command {
public:
    EPixelFormat format{};
    uint64       src_handle{};
    uint64       dst_handle{};
    uint3        src_offset{};
    uint         dst_offset{};
    uint3        size{};
    uint         mip_level{};
    uint         array_layer{};

private:
    CopyTextureToBufferCmd() : Command(EType::TextureToBuffer) {}

public:
    CopyTextureToBufferCmd(
        EPixelFormat     _format,
        uint64           _src_handle,
        uint64           _dst_handle,
        uint3            _src_offset,
        uint             _dst_offset,
        uint3            _size,
        uint             _mip_level,
        uint             _array_layer,
        StringView _name = typenames[uint(EType::TextureToBuffer)]
    ) :
        Command(EType::TextureToBuffer, _name),
        format(_format),
        src_handle(_src_handle),
        dst_handle(_dst_handle),
        src_offset{_src_offset.x, _src_offset.y, _src_offset.z},
        dst_offset(_dst_offset),
        size{_size.x, _size.y, _size.z},
        mip_level(_mip_level),
        array_layer(_array_layer) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto Format() const {
        return format;
    }
    auto SrcHandle() const {
        return src_handle;
    }
    auto DstHandle() const {
        return dst_handle;
    }
    auto SrcOffset() const {
        return src_offset;
    }
    auto DstOffset() const {
        return dst_offset;
    }
    auto Size() const {
        return size;
    }
    auto ByteSize() const {
        return GetSizeFromImageFormat(format, size);
    }
    auto MipLevel() const {
        return mip_level;
    }
    auto ArrayLayer() const {
        return array_layer;
    }
};

struct UploadTextureCmd : public Command {
public:
    EPixelFormat                                     format{};
    uint64                                           handle{};
    uint32                                           mip_level{};
    uint32                                           array_layer{};
    uint3                                            offset{};
    uint3                                            size{};
    std::variant<std::span<const byte>, Array<byte>> storage;

private:
    UploadTextureCmd() : Command(EType::UploadTexture) {}

public:
    UploadTextureCmd(
        EPixelFormat     _format,
        uint64           _handle,
        uint             _mip_level,
        uint32           _array_layer,
        uint3            _offset,
        uint3            _size,
        const void*      _data,
        StringView _name = typenames[uint(EType::UploadTexture)]
    ) :
        Command(EType::UploadTexture, _name),
        format(_format),
        handle(_handle),
        mip_level(_mip_level),
        array_layer(_array_layer),
        offset{_offset.x, _offset.y, _offset.z},
        size{_size.x, _size.y, _size.z},
        storage(
            std::span<const byte>(
                reinterpret_cast<const byte*>(_data),
                GetSizeFromImageFormat(_format, _size)
            )
        ) {}

    UploadTextureCmd(
        EPixelFormat     _format,
        uint64           _handle,
        uint             _mip_level,
        uint32           _array_layer,
        uint3            _offset,
        uint3            _size,
        Array<byte>&&    _data,
        StringView _name = typenames[uint(EType::UploadTexture)]
    ) :
        Command(EType::UploadTexture, _name),
        format(_format),
        handle(_handle),
        mip_level(_mip_level),
        array_layer(_array_layer),
        offset{_offset.x, _offset.y, _offset.z},
        size{_size.x, _size.y, _size.z},
        storage(std::move(_data)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Copy;
    }

    auto Format() const {
        return format;
    }
    auto Handle() const {
        return handle;
    }
    auto MipLevel() const {
        return mip_level;
    }
    auto ArrayLayer() const {
        return array_layer;
    }
    auto Offset() const {
        return offset;
    }
    auto Size() const {
        return size;
    }
    // auto Data() const { return data; }
    auto Data() const {
        return std::visit(
            [](const auto& _data) -> std::span<const byte> {
                using TData = std::decay_t<decltype(_data)>;
                if constexpr (std::is_same_v<TData, std::span<const byte>>) {
                    return _data;
                } else if constexpr (std::is_same_v<TData, Array<byte>>) {
                    return std::span<const byte>(_data.data(), _data.size());
                }
                return std::span<const byte>();
            },
            storage
        );
    }

    mutable BufferView staging_buffer;
};

using ResourceState = std::variant<EBufferRuntimeUsageFlags, ETextureStateFlags>;

struct TextureBarrier {
    uint64                    handle;
    BarrierState              src_state;
    BarrierState              dst_state;
    std::optional<ETextureState> tracked_state;
    bool                      access_write{false};
    uint                      mip_level : 8;
    uint                      mip_cnt : 8;
    uint                      array_layer : 8;
    uint                      array_count : 8;
};

struct BufferBarrier {
    uint64                   handle;
    BarrierState             src_state;
    BarrierState             dst_state;
    std::optional<EBufferState> tracked_state;
    bool                     access_write{false};
    uint64                   offset{};
    uint64                   byte_size{};
};

struct BindlessArrayBarrier {
    uint64       handle;
    EBufferState state;
    EPassType    pass_type;
};

struct AccelerationStructureBarrier {
    uint64       handle;
    EBufferState state;
    EPassType    pass_type;
};

struct BarrierCmd : public Command {
private:
    BarrierCmd() : Command(EType::Barrier) {}
    Array<TextureBarrier> textures;
    Array<BufferBarrier>  buffers;
    Array<BindlessArrayBarrier> read_bindless_arrays;
    Array<BindlessArrayBarrier> write_bindless_arrays;
    Array<AccelerationStructureBarrier> read_acceleration_structures;
    Array<AccelerationStructureBarrier> write_acceleration_structures;
    EQueueType            src_queue;
    EQueueType            dst_queue;
    bool                  b_queue_transition = false;
    ETrackedStateUpdateMode tracked_state{ETrackedStateUpdateMode::Update};

public:
    BarrierCmd& AddBarrier(const BarrierCreateInfo& barrier) {
        std::visit(
            [this, &barrier](auto&& resource) {
                using TResource = std::decay_t<decltype(resource)>;
                if constexpr (std::is_same_v<TResource, TextureView>) {
                    const std::optional<ETextureState> tracked_texture_state =
                        TryGetTrackedTextureState(barrier.dst_state);
                    if (tracked_state == ETrackedStateUpdateMode::Update) {
                        assert(resource.IsWholeResource() && "partial texture barriers must use ETrackedStateUpdateMode::Skip");
                        assert(tracked_texture_state.has_value() && "texture barrier dst_state must map to tracked state");
                    }
                    textures.emplace_back(TextureBarrier{
                        reinterpret_cast<uint64>(resource.texture),
                        barrier.src_state,
                        barrier.dst_state,
                        tracked_texture_state,
                        BarrierStateWrites(barrier.dst_state),
                        resource.mip_level,
                        resource.num_mips,
                        resource.array_layer,
                        resource.num_array,
                    });
                } else if constexpr (std::is_same_v<TResource, BufferView>) {
                    const std::optional<EBufferState> tracked_buffer_state =
                        TryGetTrackedBufferState(barrier.dst_state);
                    if (tracked_state == ETrackedStateUpdateMode::Update) {
                        assert(resource.IsWholeResource() && "partial buffer barriers must use ETrackedStateUpdateMode::Skip");
                        assert(tracked_buffer_state.has_value() && "buffer barrier dst_state must map to tracked state");
                    }
                    buffers.emplace_back(BufferBarrier{
                        reinterpret_cast<uint64>(resource.GetBuffer()),
                        barrier.src_state,
                        barrier.dst_state,
                        tracked_buffer_state,
                        BarrierStateWrites(barrier.dst_state),
                        resource.GetByteOffset(),
                        resource.GetByteSize(),
                    });
                }
            },
            barrier.resource
        );
        return *this;
    }

    BarrierCmd& ReadBindlessArray(BindlessArrayRef _array, EBufferState _dst_state, EPassType _pass_type) {
        read_bindless_arrays.emplace_back(
            BindlessArrayBarrier{reinterpret_cast<uint64>(_array.Get()), _dst_state, _pass_type}
        );
        return *this;
    }

    BarrierCmd& WriteBindlessArray(BindlessArrayRef _array, EBufferState _dst_state, EPassType _pass_type) {
        write_bindless_arrays.emplace_back(
            BindlessArrayBarrier{reinterpret_cast<uint64>(_array.Get()), _dst_state, _pass_type}
        );
        return *this;
    }

    BarrierCmd& ReadAccelerationStructure(uint64 _handle, EBufferState _dst_state, EPassType _pass_type) {
        read_acceleration_structures.emplace_back(
            AccelerationStructureBarrier{_handle, _dst_state, _pass_type}
        );
        return *this;
    }

    BarrierCmd& WriteAccelerationStructure(uint64 _handle, EBufferState _dst_state, EPassType _pass_type) {
        write_acceleration_structures.emplace_back(
            AccelerationStructureBarrier{_handle, _dst_state, _pass_type}
        );
        return *this;
    }

    BarrierCmd(
        uint                    _barrier_cnt,
        EQueueType              _src_queue,
        EQueueType              _dst_queue,
        ETrackedStateUpdateMode _tracked_state = ETrackedStateUpdateMode::Update
    ) :
        Command(EType::Barrier),
        src_queue(_src_queue),
        dst_queue(_dst_queue),
        b_queue_transition(_src_queue != _dst_queue),
        tracked_state(_tracked_state) {
        textures.reserve(_barrier_cnt);
        buffers.reserve(_barrier_cnt);
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }
    const auto& Textures() const {
        return textures;
    }
    const auto& Buffers() const {
        return buffers;
    }
    const auto& ReadBindlessArrays() const {
        return read_bindless_arrays;
    }
    const auto& WriteBindlessArrays() const {
        return write_bindless_arrays;
    }
    const auto& ReadAccelerationStructures() const {
        return read_acceleration_structures;
    }
    const auto& WriteAccelerationStructures() const {
        return write_acceleration_structures;
    }
    const bool IsQueueTransition() const {
        return b_queue_transition;
    }
    const EQueueType GetSrcQueue() const {
        return src_queue;
    }
    const EQueueType GetDstQueue() const {
        return dst_queue;
    }
    bool ShouldUpdateTrackedState() const {
        return tracked_state == ETrackedStateUpdateMode::Update;
    }
};

struct SetTrackedStateCmd : public Command {
private:
    Array<TrackedTextureState> textures;
    Array<TrackedBufferState>  buffers;

public:
    SetTrackedStateCmd(
        Array<TrackedTextureState>&& _textures,
        Array<TrackedBufferState>&&  _buffers
    ) :
        Command(EType::SetTrackedState),
        textures(std::move(_textures)),
        buffers(std::move(_buffers)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }
    const Array<TrackedTextureState>& Textures() const {
        return textures;
    }
    const Array<TrackedBufferState>& Buffers() const {
        return buffers;
    }
};

struct QueueTransferCmd : public Command {
private:
    QueueTransferCmd() : Command(EType::QueueTransfer) {}

public:
    QueueTransferCmd(
        EQueueType             _src_queue,
        Array<ImportTexture>&& _textures_to_import,
        Array<ImportBuffer>&&  _buffer_to_import
    ) :
        Command(EType::QueueTransfer),
        src_queue(_src_queue),
        import_textures(std::move(_textures_to_import)),
        import_buffers(std::move(_buffer_to_import)),
        b_is_import(true) {}
    QueueTransferCmd(
        EQueueType             _dst_queue,
        Array<ExportTexture>&& _textures_to_export,
        Array<ExportBuffer>&&  _buffer_to_export
    ) :
        Command(EType::QueueTransfer),
        dst_queue(_dst_queue),
        export_textures(std::move(_textures_to_export)),
        export_buffers(std::move(_buffer_to_export)) {}

private:
    Array<ImportTexture> import_textures;
    Array<ExportTexture> export_textures;
    Array<ImportBuffer>  import_buffers;
    Array<ExportBuffer>  export_buffers;
    bool                 b_is_import = false;

public:
    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }
    mutable EQueueType src_queue{EQueueType::Ignore};
    mutable EQueueType dst_queue{EQueueType::Ignore};
    const auto&        ImportTextures() const {
        return import_textures;
    }
    const auto& ExportTextures() const {
        return export_textures;
    }
    const auto& ImportBuffers() const {
        return import_buffers;
    }
    const auto& ExportBuffers() const {
        return export_buffers;
    }
    const bool IsImport() const {
        return b_is_import;
    }
};

struct UpdateBindlessArrayCmd : public Command {
private:
    UpdateBindlessArrayCmd() : Command(EType::UpdateBindlessArray) {}
    mutable BindlessArrayRef        array;
    Array<BindlessArray::UpdateCmd> update_cmds;
    Array<uint>                     freed_array_slots;
    Array<uint>                     freed_texture_slots;
    Array<uint>                     freed_buffer_slots;
    Array<byte>                     array_data;
    Array<std::pair<uint, uint>>    array_indices_dat;
    Array<byte>                     buffer_data;
    Array<std::pair<uint, uint>>    buffer_indices_dat;
    Array<byte>                     texture_data;
    Array<std::pair<uint, uint>>    texture_indices_dat;

public:
    UpdateBindlessArrayCmd(
        BindlessArrayRef                  _array,
        Array<BindlessArray::UpdateCmd>&& _update_cmds,
        Array<uint>&&                     _freed_array_slots,
        Array<uint>&&                     _freed_texture_slots,
        Array<uint>&&                     _freed_buffer_slots,
        Array<byte>&&                     _array_data,
        Array<std::pair<uint, uint>>&&    _array_indices_dat,
        Array<byte>&&                     _buffer_data,
        Array<std::pair<uint, uint>>&&    _buffer_indices_dat,
        Array<byte>&&                     _texture_data,
        Array<std::pair<uint, uint>>&&    _texture_indices_dat,
        StringView                        _name = typenames[uint(EType::UpdateBindlessArray)]
    ) :
        Command(EType::UpdateBindlessArray, _name),
        array(_array),
        update_cmds(_update_cmds),
        freed_array_slots(std::move(_freed_array_slots)),
        freed_texture_slots(std::move(_freed_texture_slots)),
        freed_buffer_slots(std::move(_freed_buffer_slots)),
        array_data(std::move(_array_data)),
        array_indices_dat(std::move(_array_indices_dat)),
        buffer_data(std::move(_buffer_data)),
        buffer_indices_dat(std::move(_buffer_indices_dat)),
        texture_data(std::move(_texture_data)),
        texture_indices_dat(std::move(_texture_indices_dat)) {

        // assert(texture_updates.size() < 20 && "too many textures");
    }
    auto* Handle() const {
        return array.Get();
    }
    BindlessArrayRef ArrayRef() const {
        return array;
    }
    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }
    const auto& UpdateCommands() const {
        return update_cmds;
    }

    auto StealFreedArraySlots() const {
        return std::move(freed_array_slots);
    }
    auto StealFreedTextureSlots() const {
        return std::move(freed_texture_slots);
    }
    auto StealFreedBufferSlots() const {
        return std::move(freed_buffer_slots);
    }

    auto StealArrayData() const {
        return std::move(array_data);
    }
    auto StealArrayIndicesData() const {
        return std::move(array_indices_dat);
    }
    auto StealBufferIndicesData() const {
        return std::move(buffer_indices_dat);
    }
    auto StealTextureIndicesData() const {
        return std::move(texture_indices_dat);
    }
    auto StealBufferData() const {
        return std::move(buffer_data);
    }
    auto StealTextureData() const {
        return std::move(texture_data);
    }

    bool HasUpdates() const {
        return !array_indices_dat.empty() || !buffer_indices_dat.empty() || !texture_indices_dat.empty();
    }
    bool HasBufferUpdates() const {
        return !buffer_indices_dat.empty();
    }
    bool HasTextureUpdates() const {
        return !texture_indices_dat.empty();
    }
};

using TClearResource = std::variant<BufferView, TextureView>;
using TClearVar      = std::variant<uint, float4>;
struct ClearResourceCmd : public Command {
private:
    ClearResourceCmd() : Command(EType::ClearResource) {}

public:
    ClearResourceCmd(
        BufferView       _buffer,
        uint             _value,
        StringView _name = typenames[uint(EType::ClearResource)]
    ) :
        Command(EType::ClearResource, _name),
        resource(_buffer),
        clear_value(_value) {}
    ClearResourceCmd(
        TextureView      _texture,
        float4           _value,
        StringView _name = typenames[uint(EType::ClearResource)]
    ) :
        Command(EType::ClearResource, _name),
        resource(_texture),
        clear_value(_value) {}
    ClearResourceCmd(
        TextureView      _texture,
        uint             _value,
        StringView _name = typenames[uint(EType::ClearResource)]
    ) :
        Command(EType::ClearResource, _name),
        resource(_texture),
        clear_value(_value) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }
    const auto& Resource() const {
        return resource;
    }
    const auto& ClearValue() const {
        return clear_value;
    }

    bool IsBuffer() const {
        return std::holds_alternative<BufferView>(resource);
    }
    bool IsTexture() const {
        return std::holds_alternative<TextureView>(resource);
    }

    const auto& Buffer() const {
        return std::get<BufferView>(resource);
    }
    const auto& Texture() const {
        return std::get<TextureView>(resource);
    }

    const auto& UIntValue() const {
        return std::get<uint>(clear_value);
    }
    const auto& Float4Value() const {
        return std::get<float4>(clear_value);
    }

    bool IsUInt() const {
        return std::holds_alternative<uint>(clear_value);
    }
    bool IsFloat4() const {
        return std::holds_alternative<float4>(clear_value);
    }

    uint64 UnderlyingHandle() const {
        if (IsBuffer()) {
            return uint64(Buffer().GetBuffer());
        }
        return uint64(Texture().texture);
    }

private:
    TClearResource resource;
    TClearVar      clear_value;
};

struct BufferRange {
    uint64 min;
    uint64 max;
    BufferRange(uint64 _offset, uint64 _size) : min(_offset), max(_offset + _size) {}
    BufferRange() : min(0), max(0) {}
    void Merge(const BufferRange& _other) {
        min = std::min(min, _other.min);
        max = std::max(max, _other.max);
    }
};
struct SetDrawStateCmd : public Command {
public:
private:
    PipelineHandle                     pipeline{};
    RenderPassInfo                     render_pass_info;
    Array<MeshDrawData>                mesh_data;
    ArrayArguments                     args;
    GraphEventRef                      evaluate_mesh_task = nullptr;
    UnorderedMap<Buffer*, BufferRange> vertex_buffers;
    UnorderedMap<Buffer*, BufferRange> index_buffers;
    UnorderedMap<Buffer*, BufferRange> indirect_buffers;
    UnorderedMap<Buffer*, BufferRange> draw_count_buffers;

    SetDrawStateCmd(ArrayArguments&& _args) : Command(EType::SetDrawState), args(std::move(_args)) {}

public:
    SetDrawStateCmd(
        PipelineHandle&       _pipeline,
        ArrayArguments&&      _args,
        RenderPassInfo&&      _info,
        Array<MeshDrawData>&& _draw_data,
        StringView            _name = typenames[uint(EType::SetDrawState)]
    ) :
        Command(EType::SetDrawState, _name),
        args(std::move(_args)),
        pipeline(_pipeline),
        render_pass_info(std::move(_info)),
        mesh_data(std::move(_draw_data)) {
        evaluate_mesh_task =
            LambdaTask::Create([this]() {
                for (const auto& mesh : mesh_data) {
                    for (const auto& vtx_view : mesh.vtx_views) {
                        BufferRange range(vtx_view.offset, vtx_view.buffer->GetByteSize());
                        if (vertex_buffers.find(vtx_view.buffer) == vertex_buffers.end()) {
                            vertex_buffers[vtx_view.buffer] = range;
                        } else {
                            auto [offset, size] = vertex_buffers[vtx_view.buffer];
                            vertex_buffers[vtx_view.buffer].Merge(range);
                        }
                    }
                    if (std::holds_alternative<IndexBuffer>(mesh.idx_view)) {
                        const auto&       idx_buffer = std::get<IndexBuffer>(mesh.idx_view);
                        const BufferView& idx_view   = idx_buffer.buffer;
                        BufferRange       range(idx_view.GetByteOffset(), idx_view.GetByteSize());
                        if (index_buffers.find(idx_view.GetBuffer()) == index_buffers.end()) {
                            index_buffers[idx_view.GetBuffer()] = range;
                        } else {
                            auto [offset, size] = index_buffers[idx_view.GetBuffer()];
                            index_buffers[idx_view.GetBuffer()].Merge(range);
                        }
                    }

                    if (mesh.indirect_draw_param.has_value()) {
                        if (mesh.indirect_draw_param->count_buffer.has_value()) {
                            const BufferView& count_view = mesh.indirect_draw_param->count_buffer.value();
                            BufferRange       range(count_view.GetByteOffset(), count_view.GetByteSize());
                            if (draw_count_buffers.find(count_view.GetBuffer()) == draw_count_buffers.end()) {
                                draw_count_buffers[count_view.GetBuffer()] = range;
                            } else {
                                auto [offset, size] = draw_count_buffers[count_view.GetBuffer()];
                                draw_count_buffers[count_view.GetBuffer()].Merge(range);
                            }
                        }

                        const BufferView& indirect_view = mesh.indirect_draw_param->buffer;
                        BufferRange       range(indirect_view.GetByteOffset(), indirect_view.GetByteSize());
                        if (indirect_buffers.find(indirect_view.GetBuffer()) == indirect_buffers.end()) {
                            indirect_buffers[indirect_view.GetBuffer()] = range;
                        } else {
                            auto [offset, size] = indirect_buffers[indirect_view.GetBuffer()];
                            indirect_buffers[indirect_view.GetBuffer()].Merge(range);
                        }
                    }
                }
            }).Dispatch();
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    auto& Pipeline() const {
        return pipeline;
    }
    const auto& RenderPassInfo() const {
        return render_pass_info;
    }
    const auto& DrawData() const {
        return mesh_data;
    }
    const auto& Args() const {
        return args;
    }

    // Note: This function is never used, so I comment it out.
    // void IterateArgs(std::function<void(const TArg&, ParamInfoFlags _flag)> _func) const {
    //     for (int i = 0; i < args.args.size(); i++) {
    //         if (pipeline->valid_bits & (1 << i))
    //             _func(args.args[i], pipeline->binding_infos[i]);
    //     }
    // }

    void IterateArgs(
        std::function<void(const TArg&, uint _idx)> _func,
        std::function<void(const TArg&, uint _idx)> _bdls_post_func
    ) const {
        int bdls_idx = -1;
        for (int i = 0; i < args.args.size(); i++) {
            if (std::holds_alternative<BindlessArrayRef>(args.args[i])) {
                bdls_idx = i;
                continue;
                ;
            }
            _func(args.args[i], i);
        }

        if (bdls_idx != -1) {
            _bdls_post_func(args.args[bdls_idx], bdls_idx);
        }
    }
    const auto& VertexBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return vertex_buffers;
    }
    const auto& IndexBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return index_buffers;
    }
    const auto& IndirectBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return indirect_buffers;
    }

    const auto& DrawCountBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return draw_count_buffers;
    }
};

//Between BeginRendering() And EndRendering()
struct MultiDrawCmd : public Command {
public:
    MultiDrawCmd(DrawBatch&& _batch, RenderPassInfo&& _info, StringView _name) :
        Command(EType::MultiDraw, _name),
        draw_batch(std::move(_batch)),
        render_pass_info(std::move(_info)) {

        evaluate_mesh_task =
            LambdaTask::Create([this]() {
                //collect vertex buffer and index buffer
                for (const auto& mesh : draw_batch.draw_cmds) {
                    if (std::holds_alternative<Array<MeshDrawData>>(mesh.mesh_dispatch_data)) {
                        const auto& mesh_data_array = std::get<Array<MeshDrawData>>(mesh.mesh_dispatch_data);
                        for (const auto& mesh_data : mesh_data_array) {
                            if (mesh_data.indirect_draw_param.has_value()) {
                                if (mesh_data.indirect_draw_param->count_buffer.has_value()) {
                                    const BufferView& count_view =
                                        mesh_data.indirect_draw_param->count_buffer.value();
                                    if (indirect_buffers.find(count_view.GetBuffer()) ==
                                        indirect_buffers.end()) {
                                        indirect_buffers[count_view.GetBuffer()] =
                                            BufferRange(count_view.GetByteOffset(), count_view.GetByteSize());
                                    } else {
                                        auto [offset, size] = indirect_buffers[count_view.GetBuffer()];
                                        indirect_buffers[count_view.GetBuffer()].Merge(
                                            BufferRange(count_view.GetByteOffset(), count_view.GetByteSize())
                                        );
                                    }
                                }

                                const BufferView& indirect_view = mesh_data.indirect_draw_param->buffer;
                                BufferRange range(indirect_view.GetByteOffset(), indirect_view.GetByteSize());
                                if (indirect_buffers.find(indirect_view.GetBuffer()) ==
                                    indirect_buffers.end()) {
                                    indirect_buffers[indirect_view.GetBuffer()] = range;
                                } else {
                                    auto [offset, size] = indirect_buffers[indirect_view.GetBuffer()];
                                    indirect_buffers[indirect_view.GetBuffer()].Merge(range);
                                }
                            }
                            for (const auto& vtx_view : mesh_data.vtx_views) {
                                BufferRange range(vtx_view.offset, vtx_view.buffer->GetByteSize());
                                if (vertex_buffers.find(vtx_view.buffer) == vertex_buffers.end()) {
                                    vertex_buffers[vtx_view.buffer] = range;
                                } else {
                                    auto [offset, size] = vertex_buffers[vtx_view.buffer];
                                    vertex_buffers[vtx_view.buffer].Merge(range);
                                }
                            }
                            if (std::holds_alternative<IndexBuffer>(mesh_data.idx_view)) {
                                const auto&       idx_buffer = std::get<IndexBuffer>(mesh_data.idx_view);
                                const BufferView& idx_view   = idx_buffer.buffer;
                                BufferRange       range(idx_view.GetByteOffset(), idx_view.GetByteSize());
                                if (index_buffers.find(idx_view.GetBuffer()) == index_buffers.end()) {
                                    index_buffers[idx_view.GetBuffer()] = range;
                                } else {
                                    auto [offset, size] = index_buffers[idx_view.GetBuffer()];
                                    index_buffers[idx_view.GetBuffer()].Merge(range);
                                }
                            }
                        }
                    } else if (std::holds_alternative<Array<DispatchMeshData>>(mesh.mesh_dispatch_data)) {
                        const auto& mesh_data_array =
                            std::get<Array<DispatchMeshData>>(mesh.mesh_dispatch_data);
                        for (const auto& mesh_data : mesh_data_array) {
                            if (std::holds_alternative<IndirectDrawParam>(mesh_data.draw_param)) {
                                const auto& indirect_view = std::get<IndirectDrawParam>(mesh_data.draw_param);
                                if (indirect_view.count_buffer.has_value()) {

                                    const BufferView& count_view = indirect_view.count_buffer.value();
                                    if (indirect_buffers.find(count_view.GetBuffer()) ==
                                        indirect_buffers.end()) {
                                        indirect_buffers[count_view.GetBuffer()] =
                                            BufferRange(count_view.GetByteOffset(), count_view.GetByteSize());
                                    } else {
                                        auto [offset, size] = indirect_buffers[count_view.GetBuffer()];
                                        indirect_buffers[count_view.GetBuffer()].Merge(
                                            BufferRange(count_view.GetByteOffset(), count_view.GetByteSize())
                                        );
                                    }
                                }

                                const BufferView& indirect_buffer =
                                    std::get<IndirectDrawParam>(mesh_data.draw_param).buffer;
                                BufferRange range(
                                    indirect_buffer.GetByteOffset(), indirect_buffer.GetByteSize()
                                );
                            }
                        }
                    }
                }
            }).Dispatch();
    }
    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

public:
    DrawBatch      draw_batch;
    RenderPassInfo render_pass_info;

public:
    const auto& VertexBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return vertex_buffers;
    }

    const auto& IndexBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return index_buffers;
    }

    const auto& IndirectBuffers() const {
        if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) {
            evaluate_mesh_task->Wait();
        }
        return indirect_buffers;
    }

    const auto& RenderPassInfo() const {
        return render_pass_info;
    }

private:
    UnorderedMap<Buffer*, BufferRange> vertex_buffers;
    UnorderedMap<Buffer*, BufferRange> index_buffers;
    UnorderedMap<Buffer*, BufferRange> indirect_buffers;

    GraphEventRef evaluate_mesh_task = nullptr;
};

// Specialized version of SetDrawStateCmd for geometry pass
// struct SetGeometryPassDrawStateCmd : public Command {
// private:
//     // Origin parameters
//     ArrayArguments                                             args;
//     RenderPassInfo                                             render_pass_info;
//     UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>> mesh_data_array_map;
//     // Derived parameters
//     GraphEventRef                                          evaluate_mesh_task = nullptr;
//     UnorderedMap<Buffer*, BufferRange>                     vertex_buffers;
//     UnorderedMap<Buffer*, BufferRange>                     index_buffers;
//     UnorderedMap<VertexAttributesBitmask, PipelineHandle&> pipeline_map;

// public:
//     // clang-format off
//     SetGeometryPassDrawStateCmd(
//         ArrayArguments&&                                             _args,
//         RenderPassInfo&&                                             _info,
//         UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>>&& _mesh_data_array_map,
//         StringView                                                   _name
//     ) : Command(EType::SetGeometryPassDrawState, _name),
//         args(std::move(_args)),
//         render_pass_info(std::move(_info)),
//         mesh_data_array_map(std::move(_mesh_data_array_map))
//     {
//         evaluate_mesh_task = LambdaTask::Create([this]() {
//             // TODO: 检查一下是否所有MeshData可以直接全部塞进一个vertex buffer和一个index buffer里
//             for (const auto& [bitmask, mesh_data_array] : mesh_data_array_map) {

//                 pipeline_map.emplace(bitmask, GeometryPassPsoManager::Get().GetPso(bitmask).handle);

//                 for (const auto& mesh : mesh_data_array) {
//                     for (const auto& vtx_view : mesh.vtx_views) {
//                         BufferRange range(vtx_view.offset, vtx_view.buffer->GetByteSize());
//                         if (vertex_buffers.find(vtx_view.buffer) == vertex_buffers.end()) {
//                             vertex_buffers[vtx_view.buffer] = range;
//                         } else {
//                             auto [offset, size] = vertex_buffers[vtx_view.buffer];
//                             vertex_buffers[vtx_view.buffer].Merge(range);
//                         }
//                     }
//                     if (std::holds_alternative<IndexBuffer>(mesh.idx_view)) {
//                         const auto&       idx_buffer = std::get<IndexBuffer>(mesh.idx_view);
//                         const BufferView& idx_view   = idx_buffer.buffer;
//                         BufferRange       range(idx_view.GetByteOffset(), idx_view.GetByteSize());
//                         if (index_buffers.find(idx_view.GetBuffer()) == index_buffers.end()) {
//                             index_buffers[idx_view.GetBuffer()] = range;
//                         } else {
//                             auto [offset, size] = index_buffers[idx_view.GetBuffer()];
//                             index_buffers[idx_view.GetBuffer()].Merge(range);
//                         }
//                     }
//                 }
//             }
//         }).Dispatch();
//     }
//     // clang-format on

//     EQueueType GetQueueType() const override { return EQueueType::Graphics; }

//     const auto& Args() const { return args; }
//     const auto& RenderPassInfo() const { return render_pass_info; }
//     const auto& DrawDataArrayMap() const { return mesh_data_array_map; }

//     const auto& VertexBuffers() const {
//         if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
//         return vertex_buffers;
//     }
//     const auto& IndexBuffers() const {
//         if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
//         return index_buffers;
//     }
//     const auto& PipelineMap() const {
//         if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
//         return pipeline_map;
//     }

//     void IterateArgs(std::function<void(const TArg&, uint _idx)> _func, std::function<void(const TArg&, uint _idx)> _bdls_post_func) const {
//         int bdls_idx = -1;
//         for (int i = 0; i < args.args.size(); i++) {
//             if (std::holds_alternative<BindlessArrayRef>(args.args[i])) {
//                 bdls_idx = i;
//                 continue;
//                 ;
//             }
//             _func(args.args[i], i);
//         }

//         if (bdls_idx != -1) {
//             _bdls_post_func(args.args[bdls_idx], bdls_idx);
//         }
//     }
// };
struct DispatchIndirectParam {
    BufferView indirect;
};

static void IterateArgs(
    const ArrayArguments&                       _args,
    std::function<void(const TArg&, uint _idx)> _func,
    std::function<void(const TArg&, uint _idx)> _bdls_post_func
) {
    int bdls_idx = -1;
    for (int i = 0; i < _args.args.size(); i++) {
        if (std::holds_alternative<BindlessArrayRef>(_args.args[i])) {
            bdls_idx = i;
            continue;
        }
        _func(_args.args[i], i);
    }

    if (bdls_idx != -1) {
        _bdls_post_func(_args.args[bdls_idx], bdls_idx);
    }
}
struct DispatchCmd : public Command {
public:
    using DispatchParam = std::variant<uint3, DispatchIndirectParam>;

private:
    // const PipelineHandle& pipeline;
    PipelineHandle pipeline{};
    DispatchParam           param;
    // ArrayArguments          args;
    TShaderArgArray args;
    DispatchCmd(ArrayArguments&& _args) : Command(EType::ShaderDispatch), args(std::move(_args)) {}

public:
    DispatchCmd(
        TShaderArgArray&& _args,
        PipelineHandle&   _handle,
        uint3             _param,
        StringView        _name = typenames[uint(EType::ShaderDispatch)]
    ) :
        Command(EType::ShaderDispatch, _name),
        param(_param),
        pipeline(_handle),
        args(std::move(_args)) {}
    DispatchCmd(
        TShaderArgArray&& _args,
        PipelineHandle&   _handle,
        BufferView        _indirect,
        StringView        _name = typenames[uint(EType::ShaderDispatch)]
    ) :
        Command(EType::ShaderDispatch, _name),
        pipeline(_handle),
        param(DispatchIndirectParam{_indirect}),
        args(std::move(_args)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Compute;
    }

    const auto& Args(const TCachedArgArray& _args_cache) const {
        if (std::holds_alternative<ArrayArguments>(args)) {
            return std::get<ArrayArguments>(args);
        }
        return _args_cache[std::get<ArrayArgReference>(args).handle];
    }
    auto& Pipeline() const {
        return pipeline;
    }
    auto Param() const {
        return param;
    }
};

struct TraceRayCmd : public Command {
public:
    using TraceRayParam = std::variant<uint3, BufferView>;

private:
    PipelineHandle pipeline{};
    TraceRayParam  param;
    ArrayArguments args;
    TraceRayCmd(ArrayArguments&& _args) : Command(EType::TraceRay), args(std::move(_args)) {}

public:
    TraceRayCmd(
        ArrayArguments&& _args,
        PipelineHandle   _handle,
        uint3            _param,
        StringView _name = typenames[uint(EType::TraceRay)]
    ) :
        Command(EType::TraceRay, _name),
        param(_param),
        pipeline(_handle),
        args(std::move(_args)) {}
    TraceRayCmd(
        ArrayArguments&& _args,
        PipelineHandle   _handle,
        BufferView       _param,
        StringView _name = typenames[uint(EType::TraceRay)]
    ) :
        Command(EType::TraceRay, _name),
        pipeline(_handle),
        param(_param),
        args(std::move(_args)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    const auto& Args() const {
        return args;
    }
    const auto& Pipeline() const {
        return pipeline;
    }
    auto Param() const {
        return param;
    }
    void IterateArgs(const std::function<void(const TArg&, ParamInfoFlags _flag)>& _func) const {
        for (int i = 0; i < args.args.size(); i++) {
            if (pipeline.valid_bits & (1 << i))
                std::visit(
                    [&_func, i, this](const auto& _arg) {
                        _func(_arg, pipeline.binding_infos[i]);
                    },
                    args.args[i]
                );
        }
    }
};

struct BuildAccelerationStructuresCmd : public Command {
private:
    BuildAccelerationStructuresCmd() : Command(EType::BuildAccel) {}

public:
    BuildAccelerationStructuresCmd(
        const Array<AccelerationStructureBuildParam>& _params,
        StringView                                    _name = typenames[uint(EType::BuildAccel)]
    ) :
        Command(EType::BuildAccel, _name),
        params(_params) {
        AsyncPreprocess();
    }
    BuildAccelerationStructuresCmd(
        Array<AccelerationStructureBuildParam>&& _params,
        StringView                               _name = typenames[uint(EType::BuildAccel)]
    ) :
        Command(EType::BuildAccel, _name),
        params(std::move(_params)) {
        AsyncPreprocess();
    }

    EQueueType GetQueueType() const override {
        return EQueueType::Compute;
    }

    const auto& Params() const {
        return params;
    }

    auto& Scratch() const {
        return scratch_buffer;
    }

    auto& VtxBuffers() const {
        if (evaluate_task && !evaluate_task->IsComplete()) {
            evaluate_task->Wait();
        }
        return vtx_buffers;
    }

    auto& IdxBuffers() const {
        if (evaluate_task && !evaluate_task->IsComplete()) {
            evaluate_task->Wait();
        }
        return idx_buffers;
    }

private:
    void AsyncPreprocess() {
        evaluate_task = LambdaTask::Create([this]() {
                            for (const auto& param : params) {
                                //vtx buffers and idx buffers
                                for (const auto& segment : param.geometry->GetInfo().segments) {
                                    if (segment.vertex_buffer) {
                                        vtx_buffers.insert(segment.vertex_buffer);
                                    }
                                    if (segment.index_buffer) {
                                        idx_buffers.insert(segment.index_buffer);
                                    }
                                }
                            }
                        }).Dispatch();
    }
    Array<AccelerationStructureBuildParam> params;
    mutable BufferView                     scratch_buffer{};
    UnorderedSet<Buffer*>                  vtx_buffers;
    UnorderedSet<Buffer*>                  idx_buffers;
    GraphEventRef                          evaluate_task = nullptr;
};

struct UpdateRaytracingSceneCmd : public Command {
private:
    UpdateRaytracingSceneCmd() : Command(EType::BuildTLAS) {}

public:
    UpdateRaytracingSceneCmd(
        UnorderedMap<uint64, uint32>&& _related_geoms,
        uint64                         _scene_handle,
        uint64                         _instance_buffer_handle,
        uint64                         _scratch_buffer_handle,
        uint64                         _tlas_handle,
        Array<uint>&&                  _instances_to_update,
        Array<byte>&&                  _instance_data,
        uint                           _instance_cnt,
        bool                           _full_refit,
        StringView                     _name = typenames[uint(EType::BuildTLAS)]
    ) :
        related_geometries(std::move(_related_geoms)),
        scene_handle(_scene_handle),
        instance_buffer_handle(_instance_buffer_handle),
        scratch_buffer_handle(_scratch_buffer_handle),
        tlas_handle(_tlas_handle),

        instance_to_update_ids(std::move(_instances_to_update)),
        instance_data(std::move(_instance_data)),
        instance_cnt(_instance_cnt),
        b_full_refit(_full_refit),
        Command(EType::BuildTLAS, _name) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Compute;
    }

    const auto& InstancesToUpdate() const {
        return instance_to_update_ids;
    }
    const auto& InstanceData() const {
        return instance_data;
    }

    auto StealInstancesToUpdate() const {
        return std::move(instance_to_update_ids);
    }
    auto StealInstanceData() const {
        return std::move(instance_data);
    }

    auto SceneHandle() const {
        return scene_handle;
    }

    uint64 InstanceBufferHandle() const {
        return instance_buffer_handle;
    }
    uint64 ScratchBufferHandle() const {
        return scratch_buffer_handle;
    }
    uint64 TlasHandle() const {
        return tlas_handle;
    }

    uint32 InstanceCount() const {
        return instance_cnt;
    }

    bool HasGeometry(uint64 _handle) const {
        return related_geometries.find(_handle) != related_geometries.end();
    }

    const auto& RelatedGeometries() const {
        return related_geometries;
    }

    bool ForceUpdate() const {
        return b_full_refit;
    }

private:
    uint64      scene_handle;
    Array<uint> instance_to_update_ids;
    Array<byte> instance_data;

    uint64 instance_buffer_handle;
    uint64 scratch_buffer_handle;
    uint64 tlas_handle;

    uint                       modifiable_instance_cnt = 0;
    bool                       b_full_refit            = false;
    uint                       instance_cnt            = 0;
    UnorderedMap<uint64, uint> related_geometries;
};

struct QueryCmd : public Command {
public:
    enum class EOp : uint8_t {
        BeginTimestamp = 0,
        EndTimestamp   = 1,
        BeginOcclusion = 2,
        EndOcclusion   = 3
    };

    QueryCmd(
        QueryToken      _token,
        EOp             _op,
        StringView _name = typenames[uint(EType::Query)]
    ) :
        Command(EType::Query, _name),
        token(std::move(_token)),
        op(_op) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }

    const QueryToken& Token() const {
        return token;
    }

    EOp Op() const {
        return op;
    }

private:
    QueryToken token{};
    EOp        op{EOp::BeginTimestamp};
};

//command for push/pop debug scope
struct ScopeCmd : public Command {
public:
    ScopeCmd(StringView _name, bool _push, bool _query_timestamp) :
        Command(EType::Scope, _name),
        b_push(_push),
        scope_name(_name),
        b_query_timestamp(_query_timestamp) {}

public:
    EQueueType GetQueueType() const override {
        return EQueueType::Graphics;
    }
    bool IsPush() const {
        return b_push;
    }
    bool IsPop() const {
        return !b_push;
    }
    auto ScopeName() const {
        return scope_name;
    }
    bool QueryTimestamp() const {
        return b_query_timestamp;
    }

private:
    bool             b_push            = false;
    bool             b_query_timestamp = false;
    String scope_name;
};

struct CustomCmd : public Command {
public:
    enum class CustomCmdId : uint8 {
        CUSTOM_CMD_NONE = 0u,
        CUSTOM_RASTER,
        CUSTOM_DISPATCH,
        CUSTOM_TRANSLATE_FENCE,
        CUSTOM_TRANSLATE_LAMBDA,
        CUSTOM_FRAME_TICK,
        // ...
        CUSTOM_CMD_END = 0xffu,
    };

    static constexpr StringView custom_cmd_names[] = {
        MOER_TEXT("CUSTOM_CMD_NONE"),
        MOER_TEXT("CUSTOM_RASTER"),
        MOER_TEXT("CUSTOM_DISPATCH"),
        MOER_TEXT("CUSTOM_TRANSLATE_FENCE"),
        MOER_TEXT("CUSTOM_TRANSLATE_LAMBDA"),
        MOER_TEXT("CUSTOM_FRAME_TICK"),
    };

private:
    CustomCmd() : custom_id(CustomCmdId::CUSTOM_CMD_NONE), Command(EType::Custom) {}

    CustomCmdId custom_id;

public:
    explicit CustomCmd(CustomCmdId _id) :
        custom_id(_id),
        Command(EType::Custom, custom_cmd_names[uint(_id)]) {}
    explicit CustomCmd(CustomCmdId _id, StringView _name) :
        custom_id(_id),
        Command(EType::Custom, _name) {}
    virtual ~CustomCmd() = default;
    CustomCmdId CustomId() const {
        return custom_id;
    }
};

struct CustomDispatchCmd : public CustomCmd {
public:
    struct ResourceUsage {
        TArg           resource;
        ParamInfoFlags state_flags;
        template<typename Arg>
            requires(std::is_constructible_v<TArg, Arg &&>)
        ResourceUsage(Arg&& _resource, ParamInfoFlags _state_flags) :
            resource{std::forward<Arg>(_resource)},
            state_flags{_state_flags} {}
    };

private:
    virtual std::span<const ResourceUsage> GetResourceUsages() const = 0;

public:
    CustomDispatchCmd() : CustomCmd(CustomCmdId::CUSTOM_DISPATCH) {}
    ~CustomDispatchCmd() = default;

    void IterateArgs(std::function<void(const TArg&, ParamInfoFlags _usage)> _func) const {
        for (const auto& usage : GetResourceUsages()) {
            _func(usage.resource, usage.state_flags);
        }
    }
};

struct TranslateFenceCmd : public CustomCmd {
    explicit TranslateFenceCmd(RHITranslateFence _fence) :
        CustomCmd(CustomCmdId::CUSTOM_TRANSLATE_FENCE, MOER_TEXT("TranslateFence")),
        fence(std::move(_fence)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }

    const RHITranslateFence& Fence() const {
        return fence;
    }

private:
    RHITranslateFence fence{};
};

struct TranslateLambdaCmd : public CustomCmd {
    explicit TranslateLambdaCmd(
        std::function<void()>&& _callback,
        StringView              _name = MOER_TEXT("LambdaCommand")
    ) :
        CustomCmd(CustomCmdId::CUSTOM_TRANSLATE_LAMBDA, _name),
        callback(std::move(_callback)) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }

    void Execute() const {
        callback();
    }

private:
    std::function<void()> callback;
};

struct FrameTickCmd : public CustomCmd {
    FrameTickCmd() : CustomCmd(CustomCmdId::CUSTOM_FRAME_TICK, MOER_TEXT("FrameTick")) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }

    void Execute() const {}
};

struct BufferOverlapCmd : public Command {
    BufferOverlapCmd(uint64 _buffer_handle, bool _begin) :
        Command(EType::BufferOverlap, _begin ? MOER_TEXT("BeginBufferOverlap") : MOER_TEXT("EndBufferOverlap")),
        buffer_handle(_buffer_handle),
        begin(_begin) {}

    EQueueType GetQueueType() const override {
        return EQueueType::Ignore;
    }

    uint64 BufferHandle() const {
        return buffer_handle;
    }

    bool IsBegin() const {
        return begin;
    }

private:
    uint64 buffer_handle{0};
    bool   begin{false};
};

class RenderDevice::Impl {
public:
    Impl() {}

    virtual ~Impl() = default;
    virtual void PostInit() {}

public:
    virtual FenceRef  CreateFence() = 0;
    BufferRef CreateBuffer(
        StringView        _name,
        uint              _element_cnt,
        uint              _byte_stride,
        EBufferUsageFlags _usage,
        EPixelFormat      _format
    ) {
        return CreateBuffer(_name, BufferInfo{_element_cnt, _byte_stride, _usage, _format});
    }

    virtual BufferRef CreateBuffer(StringView _name, const BufferInfo& _info) = 0;

    TextureRef CreateTexture(
        StringView         _name,
        ETextureDimension  _dimension,
        Extent3D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt    = 1,
        uint               _array_size = 1
    ) {
        bool b_depth = uint(ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT & _usage) != 0;
        TextureInfo info{
            _dimension,
            _usage,
            _format,
            b_depth ? EClearAttachment::DEPTH_STENCIL : EClearAttachment::COLOR,
            _size,
            uint8(_mip_cnt),
            uint8((_dimension == ETextureDimension::TEX_CUBE ? 6 : 1) * _array_size),
            1
        };
        info.aspect_flags = b_depth ? ETextureAspectFlags::DEPTH_SLICE : ETextureAspectFlags::COLOR;
        info.debug_name   = String(_name);
        return CreateTexture(_name, info);
    }

    virtual TextureRef CreateTexture(StringView _name, const TextureInfo& _info) = 0;

    DepthBufferRef CreateDepthBuffer(
        StringView         _name,
        Extent2D           _size,
        EPixelFormat       _format,
        uint32_t           _array_size = 1,
        ETextureUsageFlags _usage      = ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
    ) {
        return DepthBufferRef(MoerNew(DepthBuffer)(
            CreateTexture(_name, ETextureDimension::TEX_2D, _size, _format, _usage, 1, _array_size)
        ));
    }

    virtual BindlessArrayRef CreateBindlessArray(uint _max_size) = 0;

    /// Raytracing
    virtual RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) = 0;

    virtual RaytracingSceneRef CreateRaytracingScene() = 0;

    // virtual RHIViewportRef CreateViewport(const RHIViewportInitializer& _init) = 0;

    // virtual BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport) = 0;

    // virtual void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
    void FlushPendingDeletes();

    const ShaderTargetInfo& GetShaderTargetInfo() const;

    virtual CommandQueue& GetCommandQueue(EQueueType _type) = 0;

    virtual CopyQueue& GetCopyQueue() = 0;
    virtual SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) = 0;

    virtual PipelineHandle
    CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) = 0; //gfx
    virtual PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders)        = 0; //compute

    virtual RuntimePlugin* LoadPlugin(StringView _name) {
        return nullptr;
    }

    virtual IOInterfaceRef CreateIOInterface(CopyQueue&) {
        return nullptr;
    };

    virtual void FlushDebugMessages() const {
        ; // do nothing by default
    }

    virtual bool IsExtensionCooperativeEnabled() const {
        return false;
    }

    virtual const CooperativeExtensionInfo& GetCooperativeExtensionInfo() const {
        static const CooperativeExtensionInfo s_empty_info{};
        return s_empty_info;
    }

    virtual bool TryConvertCooperativeVectorMatrix(
        const CooperativeVectorConversionDesc&,
        std::span<const byte>,
        std::span<byte>
    ) const {
        return false;
    }

    virtual void WaitIdle() {};
};

} // namespace Moer::Render
#endif
