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
            std::string_view _name = typenames[uint(EType::UploadBuffer)]) : Command(EType::UploadBuffer, _name),
                                                                             handle(_handle),
                                                                             offset(_offset),
                                                                             byte_size(_byte_size),
                                                                             storage(std::span<const byte>(reinterpret_cast<const byte*>(_data), _byte_size)) {}

        UploadBufferCmd(
            uint64           _handle,
            uint64           _offset,
            uint64           _byte_size,
            Array<byte>&&    _data,
            std::string_view _name = typenames[uint(EType::UploadBuffer)]) : Command(EType::UploadBuffer, _name),
                                                                             handle(_handle),
                                                                             offset(_offset),
                                                                             byte_size(_byte_size),
                                                                             storage(std::move(_data)) {}

        EQueueType GetQueueType() const override { return EQueueType::Copy; }
        auto       Handle() const { return handle; }
        auto       Offset() const { return offset; }
        auto       ByteSize() const { return byte_size; }
        auto       Data() const {
            return std::visit([](auto&& _data) -> std::span<const byte> {
                if constexpr (std::is_same_v<std::decay_t<decltype(_data)>, Array<byte>>) {
                    return std::span<const byte>(_data.data(), _data.size());
                } else {
                    return _data;
                }
            },
                              storage);
        }

        mutable BufferView staging_buffer;
    };

    struct CopyBackBufferCmd : public Command {
    private:
        uint64      handle{};
        uint64      offset{};
        uint64      byte_size{};
        void* const data{};
        CopyBackBufferCmd() : Command(EType::CopyBackBuffer) {}

    public:
        CopyBackBufferCmd(
            uint64           _handle,
            uint64           _offset,
            uint64           _byte_size,
            void* const      _data,
            std::string_view _name = typenames[uint(EType::CopyBackBuffer)]) : Command(EType::CopyBackBuffer, _name), handle(_handle), offset(_offset), byte_size(_byte_size), data(_data) {}

        //generate getters
        EQueueType GetQueueType() const override { return EQueueType::Copy; }
        auto       Handle() const { return handle; }
        auto       Offset() const { return offset; }
        auto       ByteSize() const { return byte_size; }
        auto       Data() const { return data; }

        mutable BufferView staging_buffer;
    };

    struct CopyBackTextureCmd : public Command {
    private:
        uint64          handle{};
        uint            mip_level{};
        uint3           offset{};
        uint3           size{};
        std::span<byte> data;
        CopyBackTextureCmd() : Command(EType::CopyBackTexture) {}

    public:
        CopyBackTextureCmd(
            uint64           _handle,
            uint             _mip_level,
            uint3            _offset,
            uint3            _size,
            std::span<byte>  _data,
            std::string_view _name = typenames[uint(EType::CopyBackTexture)]) : Command(EType::CopyBackTexture, _name),
                                                                                handle(_handle),
                                                                                mip_level(_mip_level),
                                                                                offset{_offset.x, _offset.y, _offset.z},
                                                                                size{_size.x, _size.y, _size.z},
                                                                                data(_data) {}

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Handle() const { return handle; }
        auto MipLevel() const { return mip_level; }
        auto Offset() const { return offset; }
        auto Size() const { return size; }
        auto Data() const { return data; }

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
            std::string_view _name = typenames[uint(EType::CopyBackBuffer)]) : Command(EType::BufferToBuffer, _name), src_handle(_src_handle), dst_handle(_dst_handle), src_offset(_src_offset), dst_offset(_dst_offset), byte_size(_byte_size) {}

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto SrcHandle() const { return src_handle; }
        auto DstHandle() const { return dst_handle; }
        auto SrcOffset() const { return src_offset; }
        auto DstOffset() const { return dst_offset; }
        auto ByteSize() const { return byte_size; }
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
            std::string_view _name = typenames[uint(EType::TextureToTexture)]) : Command(EType::TextureToTexture, _name),
                                                                                 format(_format),
                                                                                 src_handle(_src_handle),
                                                                                 dst_handle(_dst_handle),
                                                                                 src_mip_level(_src_mip_level),
                                                                                 dst_mip_level(_dst_mip_level),
                                                                                 src_offset{_src_offset.x, _src_offset.y, _src_offset.z},
                                                                                 dst_offset{_dst_offset.x, _dst_offset.y, _dst_offset.z},
                                                                                 size{_size.x, _size.y, _size.z} {
        }

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Format() const { return format; }
        auto SrcHandle() const { return src_handle; }
        auto DstHandle() const { return dst_handle; }
        auto SrcMipLevel() const { return src_mip_level; }
        auto DstMipLevel() const { return dst_mip_level; }
        auto SrcOffset() const { return src_offset; }
        auto DstOffset() const { return dst_offset; }
        auto Size() const { return size; }
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
            std::string_view _name = typenames[uint(EType::BufferToTexture)]) : Command(EType::BufferToTexture, _name),
                                                                                format(_format),
                                                                                src_handle(_src_handle),
                                                                                dst_handle(_dst_handle),
                                                                                src_offset(_src_offset),
                                                                                dst_offset{_dst_offset.x, _dst_offset.y, _dst_offset.z},
                                                                                size{_size.x, _size.y, _size.z},
                                                                                mip_level{_mip_level} {
        }

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Format() const { return format; }
        auto SrcHandle() const { return src_handle; }
        auto DstHandle() const { return dst_handle; }
        auto SrcOffset() const { return src_offset; }
        auto DstOffset() const { return dst_offset; }
        auto Size() const { return size; }
        auto ByteSize() const { return size[0] * size[1] * size[2]; }
        auto MipLevel() const { return mip_level; }
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
            std::string_view _name = typenames[uint(EType::TextureToBuffer)]) : Command(EType::TextureToBuffer, _name),
                                                                                format(_format),
                                                                                src_handle(_src_handle),
                                                                                dst_handle(_dst_handle),
                                                                                src_offset{_src_offset.x, _src_offset.y, _src_offset.z},
                                                                                dst_offset(_dst_offset),
                                                                                size{_size.x, _size.y, _size.z},
                                                                                mip_level(_mip_level) {
        }

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Format() const { return format; }
        auto SrcHandle() const { return src_handle; }
        auto DstHandle() const { return dst_handle; }
        auto SrcOffset() const { return src_offset; }
        auto DstOffset() const { return dst_offset; }
        auto Size() const { return size; }
        auto ByteSize() const { return size[0] * size[1] * size[2]; }
        auto MipLevel() const { return mip_level; }
    };

    struct UploadTextureCmd : public Command {
    public:
        EPixelFormat format{};
        uint64       handle{};
        uint         mip_level{};
        uint3        offset{};
        uint3        size{};
        // const void*  data{};
        std::variant<std::span<const byte>, Array<byte>> storage;

    private:
        UploadTextureCmd() : Command(EType::UploadTexture) {}

    public:
        UploadTextureCmd(
            EPixelFormat     _format,
            uint64           _handle,
            uint             _mip_level,
            uint3            _offset,
            uint3            _size,
            const void*      _data,
            std::string_view _name = typenames[uint(EType::UploadTexture)]) : Command(EType::UploadTexture, _name),
                                                                              format(_format),
                                                                              handle(_handle),
                                                                              mip_level(_mip_level),
                                                                              offset{_offset.x, _offset.y, _offset.z},
                                                                              size{_size.x, _size.y, _size.z},
                                                                              storage(std::span<const byte>(reinterpret_cast<const byte*>(_data),
                                                                                                            GetSizeFromImageFormat(_format, _size))) {
        }

        UploadTextureCmd(
            EPixelFormat     _format,
            uint64           _handle,
            uint             _mip_level,
            uint3            _offset,
            uint3            _size,
            Array<byte>&&    _data,
            std::string_view _name = typenames[uint(EType::UploadTexture)]) : Command(EType::UploadTexture, _name),
                                                                              format(_format),
                                                                              handle(_handle),
                                                                              mip_level(_mip_level),
                                                                              offset{_offset.x, _offset.y, _offset.z},
                                                                              size{_size.x, _size.y, _size.z},
                                                                              storage(std::move(_data)) {
        }

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Format() const { return format; }
        auto Handle() const { return handle; }
        auto MipLevel() const { return mip_level; }
        auto Offset() const { return offset; }
        auto Size() const { return size; }
        // auto Data() const { return data; }
        auto Data() const {
            return std::visit([](const auto& _data) -> std::span<const byte> {
                using TData = std::decay_t<decltype(_data)>;
                if constexpr (std::is_same_v<TData, std::span<const byte>>) {
                    return _data;
                } else if constexpr (std::is_same_v<TData, Array<byte>>) {
                    return std::span<const byte>(_data.data(), _data.size());
                }
                return std::span<const byte>();
            },
                              storage);
        }

        mutable BufferView staging_buffer;
    };

    using ResourceState = std::variant<EBufferRuntimeUsageFlags, ETextureStateFlags>;

    struct TextureBarrier {
        uint64        handle;
        ETextureState state;
        EPassType     pass_type;
        uint          mip_level : 8;
        uint          mip_cnt : 8;
    };

    struct BufferBarrier {
        uint64       handle;
        EBufferState state;
        EPassType    pass_type;
        uint64       offset{};
        uint64       byte_size{};
    };
    struct BarrierCmd : public Command {
    private:
        BarrierCmd() : Command(EType::Barrier) {}
        Array<TextureBarrier> read_textures;
        Array<TextureBarrier> write_textures;
        Array<BufferBarrier>  read_buffers;
        Array<BufferBarrier>  write_buffers;
        EQueueType            src_queue;
        EQueueType            dst_queue;
        bool                  b_queue_transition = false;

    public:
        BarrierCmd& ReadTexture(const TextureView& _view, ETextureState _dst_state, EPassType _pass_type) {
            read_textures.emplace_back(TextureBarrier{reinterpret_cast<uint64>(_view.texture), _dst_state, _pass_type, _view.mip_level, _view.num_mips});
            return *this;
        }
        BarrierCmd& WriteTexture(const TextureView& _view, ETextureState _dst_state, EPassType _pass_type) {
            write_textures.emplace_back(TextureBarrier{reinterpret_cast<uint64>(_view.texture), _dst_state, _pass_type, _view.mip_level, _view.num_mips});
            return *this;
        }
        BarrierCmd& ReadBuffer(const BufferView& _view, EBufferState _dst_state, EPassType _pass_type) {
            read_buffers.emplace_back(BufferBarrier{reinterpret_cast<uint64>(_view.GetBuffer()), _dst_state, _pass_type, _view.GetByteOffset(), _view.GetByteSize()});
            return *this;
        }
        BarrierCmd& WriteBuffer(const BufferView& _view, EBufferState _dst_state, EPassType _pass_type) {
            write_buffers.emplace_back(BufferBarrier{reinterpret_cast<uint64>(_view.GetBuffer()), _dst_state, _pass_type, _view.GetByteOffset(), _view.GetByteSize()});
            return *this;
        }

        BarrierCmd(uint _read_tex_cnt, uint _write_tex_cnt, uint _read_buf_cnt, uint _write_buf_cnt, EQueueType _src_queue, EQueueType _dst_queue) : Command(EType::Barrier), src_queue(_src_queue), dst_queue(_dst_queue), b_queue_transition(_src_queue != _dst_queue) {
            read_textures.reserve(_read_tex_cnt);
            write_textures.reserve(_write_tex_cnt);
            read_buffers.reserve(_read_buf_cnt);
            write_buffers.reserve(_write_buf_cnt);
        }

        EQueueType       GetQueueType() const override { return EQueueType::Graphics; }
        const auto&      ReadTextures() const { return read_textures; }
        const auto&      WriteTextures() const { return write_textures; }
        const auto&      ReadBuffers() const { return read_buffers; }
        const auto&      WriteBuffers() const { return write_buffers; }
        const bool       IsQueueTransition() const { return b_queue_transition; }
        const EQueueType GetSrcQueue() const { return src_queue; }
        const EQueueType GetDstQueue() const { return dst_queue; }
    };

    struct QueueTransferCmd : public Command {
    private:
        QueueTransferCmd() : Command(EType::QueueTransfer) {}

    public:
        QueueTransferCmd(EQueueType             _src_queue,
                         Array<ImportTexture>&& _textures_to_import,
                         Array<ImportBuffer>&&  _buffer_to_import) : Command(EType::QueueTransfer),
                                                                    src_queue(_src_queue),
                                                                    import_textures(std::move(_textures_to_import)),
                                                                    import_buffers(std::move(_buffer_to_import)),
                                                                    b_is_import(true) {}
        QueueTransferCmd(EQueueType             _dst_queue,
                         Array<ExportTexture>&& _textures_to_export,
                         Array<ExportBuffer>&&  _buffer_to_export) : Command(EType::QueueTransfer),
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
        EQueueType         GetQueueType() const override { return EQueueType::Ignore; }
        mutable EQueueType src_queue;
        mutable EQueueType dst_queue;
        const auto&        ImportTextures() const { return import_textures; }
        const auto&        ExportTextures() const { return export_textures; }
        const auto&        ImportBuffers() const { return import_buffers; }
        const auto&        ExportBuffers() const { return export_buffers; }
        const bool         IsImport() const { return b_is_import; }
    };

    struct UpdateBindlessArrayCmd : public Command {
    private:
        UpdateBindlessArrayCmd() : Command(EType::UpdateBindlessArray) {}
        mutable BindlessArrayRef        array;
        Array<BindlessArray::UpdateCmd> update_cmds;
        Array<byte>                     array_data;
        Array<std::pair<uint, uint>>    array_indices_dat;
        Array<byte>                     buffer_data;
        Array<std::pair<uint, uint>>    buffer_indices_dat;
        Array<byte>                     texture_data;
        Array<std::pair<uint, uint>>    texture_indices_dat;

    public:
        UpdateBindlessArrayCmd(BindlessArrayRef                  _array,
                               Array<BindlessArray::UpdateCmd>&& _update_cmds,
                               Array<byte>&&                     _array_data,
                               Array<std::pair<uint, uint>>&&    _array_indices_dat,
                               Array<byte>&&                     _buffer_data,
                               Array<std::pair<uint, uint>>&&    _buffer_indices_dat,
                               Array<byte>&&                     _texture_data,
                               Array<std::pair<uint, uint>>&&    _texture_indices_dat,
                               std::string_view                  _name = typenames[uint(EType::UpdateBindlessArray)]) : Command(EType::UpdateBindlessArray, _name), array(_array),
                                                                                                       update_cmds(_update_cmds),
                                                                                                       array_data(std::move(_array_data)),
                                                                                                       array_indices_dat(std::move(_array_indices_dat)),
                                                                                                       buffer_data(std::move(_buffer_data)),
                                                                                                       buffer_indices_dat(std::move(_buffer_indices_dat)),
                                                                                                       texture_data(std::move(_texture_data)),
                                                                                                       texture_indices_dat(std::move(_texture_indices_dat)) {

            // assert(texture_updates.size() < 20 && "too many textures");
        }
        auto*       Handle() const { return array.Get(); }
        EQueueType  GetQueueType() const override { return EQueueType::Graphics; }
        const auto& UpdateCommands() const { return update_cmds; }

        auto StealArrayData() const { return std::move(array_data); }
        auto StealArrayIndicesData() const { return std::move(array_indices_dat); }
        auto StealBufferIndicesData() const { return std::move(buffer_indices_dat); }
        auto StealTextureIndicesData() const { return std::move(texture_indices_dat); }
        auto StealBufferData() const { return std::move(buffer_data); }
        auto StealTextureData() const { return std::move(texture_data); }

        bool HasUpdates() const { return !array_indices_dat.empty(); }
        bool HasBufferUpdates() const { return !buffer_indices_dat.empty(); }
        bool HasTextureUpdates() const { return !texture_indices_dat.empty(); }
    };

    using TClearResource = std::variant<BufferView, TextureView>;
    using TClearVar      = std::variant<uint, float4>;
    struct ClearResourceCmd : public Command {
    private:
        ClearResourceCmd() : Command(EType::ClearResource) {}

    public:
        ClearResourceCmd(BufferView _buffer, uint _value, std::string_view _name = typenames[uint(EType::ClearResource)]) : Command(EType::ClearResource, _name), resource(_buffer), clear_value(_value) {}
        ClearResourceCmd(TextureView _texture, float4 _value, std::string_view _name = typenames[uint(EType::ClearResource)]) : Command(EType::ClearResource, _name), resource(_texture), clear_value(_value) {}
        ClearResourceCmd(TextureView _texture, uint _value, std::string_view _name = typenames[uint(EType::ClearResource)]) : Command(EType::ClearResource, _name), resource(_texture), clear_value(_value) {}

        EQueueType  GetQueueType() const override { return EQueueType::Graphics; }
        const auto& Resource() const { return resource; }
        const auto& ClearValue() const { return clear_value; }

        bool IsBuffer() const { return std::holds_alternative<BufferView>(resource); }
        bool IsTexture() const { return std::holds_alternative<TextureView>(resource); }

        const auto& Buffer() const { return std::get<BufferView>(resource); }
        const auto& Texture() const { return std::get<TextureView>(resource); }

        const auto& UIntValue() const { return std::get<uint>(clear_value); }
        const auto& Float4Value() const { return std::get<float4>(clear_value); }

        bool IsUInt() const { return std::holds_alternative<uint>(clear_value); }
        bool IsFloat4() const { return std::holds_alternative<float4>(clear_value); }

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
        mutable PipelineHandle*            pipeline{};
        RenderPassInfo                     render_pass_info;
        Array<MeshDrawData>                mesh_data;
        ArrayArguments                     args;
        GraphEventRef                      evaluate_mesh_task = nullptr;
        UnorderedMap<Buffer*, BufferRange> vertex_buffers;
        UnorderedMap<Buffer*, BufferRange> index_buffers;
        UnorderedMap<Buffer*, BufferRange> indirect_buffers;
        UnorderedMap<Buffer*, BufferRange> draw_count_buffers;

        SetDrawStateCmd(ArrayArguments&& _args) : Command(EType::SetDrawState), args(std::move(_args)) {
        }

    public:
        SetDrawStateCmd(PipelineHandle&       _pipeline,
                        ArrayArguments&&      _args,
                        RenderPassInfo&&      _info,
                        Array<MeshDrawData>&& _draw_data,
                        std::string_view      _name = typenames[uint(EType::SetDrawState)]) : Command(EType::SetDrawState, _name),
                                                                                         args(std::move(_args)),
                                                                                         pipeline(&_pipeline),
                                                                                         render_pass_info(std::move(_info)),
                                                                                         mesh_data(std::move(_draw_data)) {
            evaluate_mesh_task = LambdaTask::Create([this]() {
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

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto&       Pipeline() const { return *pipeline; }
        const auto& RenderPassInfo() const { return render_pass_info; }
        const auto& DrawData() const { return mesh_data; }
        const auto& Args() const { return args; }

        // Note: This function is never used, so I comment it out.
        // void IterateArgs(std::function<void(const TArg&, ParamInfoFlags _flag)> _func) const {
        //     for (int i = 0; i < args.args.size(); i++) {
        //         if (pipeline->valid_bits & (1 << i))
        //             _func(args.args[i], pipeline->binding_infos[i]);
        //     }
        // }

        void IterateArgs(std::function<void(const TArg&, uint _idx)> _func, std::function<void(const TArg&, uint _idx)> _bdls_post_func) const {
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
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return vertex_buffers;
        }
        const auto& IndexBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return index_buffers;
        }
        const auto& IndirectBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return indirect_buffers;
        }

        const auto& DrawCountBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return draw_count_buffers;
        }
    };

    //Between BeginRendering() And EndRendering()
    struct MultiDrawCmd : public Command {
    public:
        MultiDrawCmd(DrawBatch&& _batch, RenderPassInfo&& _info, std::string_view _name) : Command(EType::MultiDraw, _name),
                                                                                           draw_batch(std::move(_batch)), render_pass_info(std::move(_info)) {

            evaluate_mesh_task = LambdaTask::Create([this]() {
                                     //collect vertex buffer and index buffer
                                     for (const auto& mesh : draw_batch.draw_cmds) {
                                         if (std::holds_alternative<Array<MeshDrawData>>(mesh.mesh_dispatch_data)) {
                                             const auto& mesh_data_array = std::get<Array<MeshDrawData>>(mesh.mesh_dispatch_data);
                                             for (const auto& mesh_data : mesh_data_array) {
                                                 if (mesh_data.indirect_draw_param.has_value()) {
                                                     if (mesh_data.indirect_draw_param->count_buffer.has_value()) {
                                                         const BufferView& count_view = mesh_data.indirect_draw_param->count_buffer.value();
                                                         if (indirect_buffers.find(count_view.GetBuffer()) == indirect_buffers.end()) {
                                                             indirect_buffers[count_view.GetBuffer()] = BufferRange(count_view.GetByteOffset(), count_view.GetByteSize());
                                                         } else {
                                                             auto [offset, size] = indirect_buffers[count_view.GetBuffer()];
                                                             indirect_buffers[count_view.GetBuffer()].Merge(BufferRange(count_view.GetByteOffset(), count_view.GetByteSize()));
                                                         }
                                                     }

                                                     const BufferView& indirect_view = mesh_data.indirect_draw_param->buffer;
                                                     BufferRange       range(indirect_view.GetByteOffset(), indirect_view.GetByteSize());
                                                     if (indirect_buffers.find(indirect_view.GetBuffer()) == indirect_buffers.end()) {
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
                                             const auto& mesh_data_array = std::get<Array<DispatchMeshData>>(mesh.mesh_dispatch_data);
                                             for (const auto& mesh_data : mesh_data_array) {
                                                 if (std::holds_alternative<IndirectDrawParam>(mesh_data.draw_param)) {
                                                     const auto& indirect_view = std::get<IndirectDrawParam>(mesh_data.draw_param);
                                                     if (indirect_view.count_buffer.has_value()) {

                                                         const BufferView& count_view = indirect_view.count_buffer.value();
                                                         if (indirect_buffers.find(count_view.GetBuffer()) == indirect_buffers.end()) {
                                                             indirect_buffers[count_view.GetBuffer()] = BufferRange(count_view.GetByteOffset(), count_view.GetByteSize());
                                                         } else {
                                                             auto [offset, size] = indirect_buffers[count_view.GetBuffer()];
                                                             indirect_buffers[count_view.GetBuffer()].Merge(BufferRange(count_view.GetByteOffset(), count_view.GetByteSize()));
                                                         }
                                                     }

                                                     const BufferView& indirect_buffer = std::get<IndirectDrawParam>(mesh_data.draw_param).buffer;
                                                     BufferRange       range(indirect_buffer.GetByteOffset(), indirect_buffer.GetByteSize());
                                                 }
                                             }
                                         }
                                     }
                                 }).Dispatch();
        }
        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

    public:
        DrawBatch      draw_batch;
        RenderPassInfo render_pass_info;

    public:
        const auto& VertexBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return vertex_buffers;
        }

        const auto& IndexBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return index_buffers;
        }

        const auto& IndirectBuffers() const {
            if (evaluate_mesh_task && !evaluate_mesh_task->IsComplete()) { evaluate_mesh_task->Wait(); }
            return indirect_buffers;
        }

        const auto& RenderPassInfo() const { return render_pass_info; }

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
    //         std::string_view                                             _name
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

    static void IterateArgs(const ArrayArguments& _args, std::function<void(const TArg&, uint _idx)> _func, std::function<void(const TArg&, uint _idx)> _bdls_post_func) {
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
        mutable PipelineHandle* pipeline = nullptr;
        DispatchParam           param;
        // ArrayArguments          args;
        TShaderArgArray args;
        DispatchCmd(ArrayArguments&& _args) : Command(EType::ShaderDispatch), args(std::move(_args)) {
        }

    public:
        DispatchCmd(TShaderArgArray&& _args, PipelineHandle& _handle, uint3 _param, std::string_view _name = typenames[uint(EType::ShaderDispatch)]) : Command(EType::ShaderDispatch, _name), param(_param), pipeline(&_handle), args(std::move(_args)) {}
        DispatchCmd(TShaderArgArray&& _args, PipelineHandle& _handle, BufferView _indirect, std::string_view _name = typenames[uint(EType::ShaderDispatch)]) : Command(EType::ShaderDispatch, _name), pipeline(&_handle), param(DispatchIndirectParam{_indirect}), args(std::move(_args)) {}

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& Args(const TCachedArgArray& _args_cache) const {
            if (std::holds_alternative<ArrayArguments>(args)) {
                return std::get<ArrayArguments>(args);
            }
            return _args_cache[std::get<ArrayArgReference>(args).handle];
        }
        auto& Pipeline() const { return *pipeline; }
        auto  Param() const { return param; }
    };

    struct TraceRayCmd : public Command {
    public:
        using TraceRayParam = std::variant<uint3, BufferView>;

    private:
        PipelineHandle pipeline{};
        TraceRayParam  param;
        ArrayArguments args;
        TraceRayCmd(ArrayArguments&& _args) : Command(EType::TraceRay), args(std::move(_args)) {
        }

    public:
        TraceRayCmd(ArrayArguments&& _args, PipelineHandle _handle, uint3 _param, std::string_view _name = typenames[uint(EType::TraceRay)]) : Command(EType::TraceRay, _name), param(_param), pipeline(_handle), args(std::move(_args)) {}
        TraceRayCmd(ArrayArguments&& _args, PipelineHandle _handle, BufferView _param, std::string_view _name = typenames[uint(EType::TraceRay)]) : Command(EType::TraceRay, _name), pipeline(_handle), param(_param), args(std::move(_args)) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        const auto& Args() const { return args; }
        const auto& Pipeline() const { return pipeline; }
        auto        Param() const { return param; }
        void        IterateArgs(const std::function<void(const TArg&, ParamInfoFlags _flag)>& _func) const {
            for (int i = 0; i < args.args.size(); i++) {
                if (pipeline.valid_bits & (1 << i))
                    std::visit([&_func, i, this](const auto& _arg) { _func(_arg, pipeline.binding_infos[i]); }, args.args[i]);
            }
        }
    };

    struct BuildAccelerationStructuresCmd : public Command {
    private:
        BuildAccelerationStructuresCmd() : Command(EType::BuildAccel) {}

    public:
        BuildAccelerationStructuresCmd(const Array<AccelerationStructureBuildParam>& _params, std::string_view _name = typenames[uint(EType::BuildAccel)]) : Command(EType::BuildAccel, _name), params(_params) {
            AsyncPreprocess();
        }
        BuildAccelerationStructuresCmd(Array<AccelerationStructureBuildParam>&& _params, std::string_view _name = typenames[uint(EType::BuildAccel)]) : Command(EType::BuildAccel, _name), params(std::move(_params)) {
            AsyncPreprocess();
        }

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& Params() const { return params; }

        auto& Scratch() const { return scratch_buffer; }

        auto& VtxBuffers() const {
            if (evaluate_task && !evaluate_task->IsComplete()) { evaluate_task->Wait(); }
            return vtx_buffers;
        }

        auto& IdxBuffers() const {
            if (evaluate_task && !evaluate_task->IsComplete()) { evaluate_task->Wait(); }
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
            std::string_view               _name = typenames[uint(EType::BuildTLAS)]) : related_geometries(std::move(_related_geoms)),
                                                                          scene_handle(_scene_handle),
                                                                          instance_buffer_handle(_instance_buffer_handle),
                                                                          scratch_buffer_handle(_scratch_buffer_handle),
                                                                          tlas_handle(_tlas_handle),

                                                                          instance_to_update_ids(std::move(_instances_to_update)),
                                                                          instance_data(std::move(_instance_data)),
                                                                          instance_cnt(_instance_cnt),
                                                                          b_full_refit(_full_refit),
                                                                          Command(EType::BuildTLAS, _name) {
        }

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& InstancesToUpdate() const { return instance_to_update_ids; }
        const auto& InstanceData() const { return instance_data; }

        auto StealInstancesToUpdate() const { return std::move(instance_to_update_ids); }
        auto StealInstanceData() const { return std::move(instance_data); }

        auto SceneHandle() const { return scene_handle; }

        uint64 InstanceBufferHandle() const { return instance_buffer_handle; }
        uint64 ScratchBufferHandle() const { return scratch_buffer_handle; }
        uint64 TlasHandle() const { return tlas_handle; }

        uint32 InstanceCount() const { return instance_cnt; }

        bool HasGeometry(uint64 _handle) const { return related_geometries.find(_handle) != related_geometries.end(); }

        bool ForceUpdate() const { return b_full_refit; }

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

    //command for push/pop debug scope
    struct ScopeCmd : public Command {
    public:
        ScopeCmd(std::string_view _name, bool _push, bool _query_timestamp) : Command(EType::Scope, _name), b_push(_push), scope_name(_name), b_query_timestamp(_query_timestamp) {}

    public:
        EQueueType GetQueueType() const override { return EQueueType::Graphics; }
        bool       IsPush() const { return b_push; }
        bool       IsPop() const { return !b_push; }
        auto       ScopeName() const { return scope_name; }
        bool       QueryTimestamp() const { return b_query_timestamp; }

    private:
        bool             b_push            = false;
        bool             b_query_timestamp = false;
        std::string_view scope_name;
    };

    struct CustomCmd : public Command {
    public:
        enum class CustomCmdId : uint8 {
            CUSTOM_CMD_NONE = 0u,
            CUSTOM_RASTER,
            CUSTOM_DISPATCH,
            // ...
            CUSTOM_CMD_END = 0xffu,
        };

        static constexpr std::string_view custom_cmd_names[] = {
            "CUSTOM_CMD_NONE",
            "CUSTOM_RASTER",
            "CUSTOM_DISPATCH",
        };

    private:
        CustomCmd() : custom_id(CustomCmdId::CUSTOM_CMD_NONE), Command(EType::Custom) {}

        CustomCmdId custom_id;

    public:
        explicit CustomCmd(CustomCmdId _id) : custom_id(_id), Command(EType::Custom, custom_cmd_names[uint(_id)]) {}
        explicit CustomCmd(CustomCmdId _id, std::string_view _name) : custom_id(_id), Command(EType::Custom, _name) {}
        virtual ~CustomCmd() = default;
        CustomCmdId CustomId() const { return custom_id; }
    };

    struct CustomDispatchCmd : public CustomCmd {
    public:
        struct ResourceUsage {
            TArg           resource;
            ParamInfoFlags state_flags;
            template<typename Arg>
                requires(std::is_constructible_v<TArg, Arg &&>)
            ResourceUsage(
                Arg&&          _resource,
                ParamInfoFlags _state_flags)
                : resource{std::forward<Arg>(_resource)},
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

    class RenderDevice::Impl {
    public:
        Impl() {}

        virtual ~Impl() = default;
        virtual void PostInit() {}

    public:
        virtual FenceRef  CreateFence()                                                                                                              = 0;
        virtual BufferRef CreateBuffer(std::string_view _name, uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage, EPixelFormat _format) = 0;

        virtual TextureRef CreateTexture(std::string_view _name, ETextureDimension _dimension, Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint _array_size = 1) = 0;

        DepthBufferRef CreateDepthBuffer(std::string_view _name, Extent2D _size, EPixelFormat _format, uint32_t _array_size = 1, ETextureUsageFlags _usage = ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT) {
            return DepthBufferRef(MoerNew(DepthBuffer)(CreateTexture(_name, ETextureDimension::TEX_2D, _size, _format, _usage, 1, _array_size)));
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

        virtual PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) = 0;//gfx
        virtual PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders)                               = 0;//compute

        virtual DeviceExtension* LoadExtension(std::string_view _name) { return nullptr; }

        virtual IOInterfaceRef CreateIOInterface(CopyQueue&) { return nullptr; };
    };

}// namespace Moer::Render
#endif