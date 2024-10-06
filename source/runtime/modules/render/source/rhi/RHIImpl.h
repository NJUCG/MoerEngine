#ifndef MOER_RHI_IMPL_H
#define MOER_RHI_IMPL_H
#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include <variant>
namespace Moer::Render {
    static uint64 GetSizeFromImageFormat(EPixelFormat _format, const uint3 _size) {
        switch (_format) {
            case PF_R8G8B8A8_SRGB:
            case PF_R8G8B8A8_UNORM:
            case PF_R8G8B8A8_UINT:
            case PF_R8G8B8A8_SNORM:
            case PF_R8G8B8A8_SINT:
                return _size.x * _size.y * _size.z * 4;
                break;
            case PF_R32G32B32A32_SFLOAT:
            case PF_R32G32B32A32_UINT:
            case PF_R32G32B32A32_SINT:
                return _size.x * _size.y * _size.z * 16;
                break;
            case PF_R32G32_SFLOAT:
            case PF_R32G32_UINT:
            case PF_R32G32_SINT:
                return _size.x * _size.y * _size.z * 8;
                break;
            case PF_R32_SFLOAT:
            case PF_R32_UINT:
            case PF_R32_SINT:
                return _size.x * _size.y * _size.z * 4;
                break;
            case PF_R16G16B16A16_SFLOAT:
            case PF_R16G16B16A16_UNORM:
            case PF_R16G16B16A16_UINT:
            case PF_R16G16B16A16_SNORM:
            case PF_R16G16B16A16_SINT:
                return _size.x * _size.y * _size.z * 8;
                break;
            case PF_R16G16_SFLOAT:
            case PF_R16G16_UNORM:
            case PF_R16G16_UINT:
            case PF_R16G16_SNORM:
            case PF_R16G16_SINT:
                return _size.x * _size.y * _size.z * 4;
                break;
            case PF_R16_SFLOAT:
            case PF_R16_UNORM:
            case PF_R16_UINT:
            case PF_R16_SNORM:
            case PF_R16_SINT:
                return _size.x * _size.y * _size.z * 2;
                break;
            case PF_R8G8B8_SRGB:
            case PF_R8G8B8_UNORM:
            case PF_R8G8B8_UINT:
            case PF_R8G8B8_SNORM:
            case PF_R8G8B8_SINT:
                return _size.x * _size.y * _size.z * 3;
                break;
            case PF_R8G8_SRGB:
            case PF_R8G8_UNORM:
            case PF_R8G8_UINT:
            case PF_R8G8_SNORM:
            case PF_R8G8_SINT:
                return _size.x * _size.y * _size.z * 2;
                break;
            case PF_R8_SRGB:
            case PF_R8_UNORM:
            case PF_R8_UINT:
            case PF_R8_SNORM:
            case PF_R8_SINT:
                return _size.x * _size.y * _size.z;
                break;
            default:
                assert(false && "not support format");
        }
        return 0;
    }
    struct UploadBufferCmd : public Command {
    private:
        uint64                                           handle{};
        uint64                                           offset{};
        uint64                                           byte_size{};
        std::variant<Array<byte>, std::span<const byte>> storage;
        UploadBufferCmd() : Command(EType::UploadBuffer) {}

    public:
        UploadBufferCmd(
            uint64      _handle,
            uint64      _offset,
            uint64      _byte_size,
            const void* _data) : Command(EType::UploadBuffer),
                                 handle(_handle),
                                 offset(_offset),
                                 byte_size(_byte_size),
                                 storage(std::span<const byte>(reinterpret_cast<const byte*>(_data), _byte_size)) {}

        UploadBufferCmd(
            uint64        _handle,
            uint64        _offset,
            uint64        _byte_size,
            Array<byte>&& _data) : Command(EType::UploadBuffer),
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
            uint64      _handle,
            uint64      _offset,
            uint64      _byte_size,
            void* const _data) : Command(EType::CopyBackBuffer), handle(_handle), offset(_offset), byte_size(_byte_size), data(_data) {}

        //generate getters
        EQueueType GetQueueType() const override { return EQueueType::Copy; }
        auto       Handle() const { return handle; }
        auto       Offset() const { return offset; }
        auto       ByteSize() const { return byte_size; }
        auto       Data() const { return data; }
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
            uint64 _src_handle,
            uint64 _dst_handle,
            uint64 _src_offset,
            uint64 _dst_offset,
            uint64 _byte_size) : Command(EType::BufferToBuffer), src_handle(_src_handle), dst_handle(_dst_handle), src_offset(_src_offset), dst_offset(_dst_offset), byte_size(_byte_size) {}

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
            EPixelFormat _format,
            uint64       _src_handle,
            uint64       _dst_handle,
            uint         _src_mip_level,
            uint         _dst_mip_level,
            uint3        _src_offset,
            uint3        _dst_offset,
            uint3        _size) : Command(EType::TextureToTexture),
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
            EPixelFormat _format,
            uint64       _src_handle,
            uint64       _dst_handle,
            uint         _src_offset,
            uint3        _dst_offset,
            uint3        _size,
            uint         _mip_level) : Command(EType::BufferToTexture),
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
            EPixelFormat _format,
            uint64       _src_handle,
            uint64       _dst_handle,
            uint3        _src_offset,
            uint         _dst_offset,
            uint3        _size,
            uint         _mip_level) : Command(EType::TextureToBuffer),
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
            EPixelFormat _format,
            uint64       _handle,
            uint         _mip_level,
            uint3        _offset,
            uint3        _size,
            const void*  _data) : Command(EType::UploadTexture),
                                 format(_format),
                                 handle(_handle),
                                 mip_level(_mip_level),
                                 offset{_offset.x, _offset.y, _offset.z},
                                 size{_size.x, _size.y, _size.z},
                                 storage(std::span<const byte>(reinterpret_cast<const byte*>(_data),
                                                               GetSizeFromImageFormat(_format, _size))) {
        }

        UploadTextureCmd(
            EPixelFormat  _format,
            uint64        _handle,
            uint          _mip_level,
            uint3         _offset,
            uint3         _size,
            Array<byte>&& _data) : Command(EType::UploadTexture),
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

        BarrierCmd(uint _read_tex_cnt, uint _write_tex_cnt, uint _read_buf_cnt, uint _write_buf_cnt) : Command(EType::Barrier) {
            read_textures.reserve(_read_tex_cnt);
            write_textures.reserve(_write_tex_cnt);
            read_buffers.reserve(_read_buf_cnt);
            write_buffers.reserve(_write_buf_cnt);
        }

        EQueueType  GetQueueType() const override { return EQueueType::Graphics; }
        const auto& ReadTextures() const { return read_textures; }
        const auto& WriteTextures() const { return write_textures; }
        const auto& ReadBuffers() const { return read_buffers; }
        const auto& WriteBuffers() const { return write_buffers; }
    };

    struct UpdateBindlessArrayCmd : public Command {
    private:
        UpdateBindlessArrayCmd() : Command(EType::UpdateBindlessArray) {}
        mutable BindlessArrayRef                        array;
        mutable Array<BindlessArray::BufferUpdateInfo>  buffer_updates;
        mutable Array<BindlessArray::TextureUpdateInfo> texture_updates;

        mutable Array<uint> free_buffers;
        mutable Array<uint> free_textures;
        mutable Array<uint> free_slots;

    public:
        UpdateBindlessArrayCmd(BindlessArrayRef                          _array,
                               Array<BindlessArray::BufferUpdateInfo>&&  _update_buffers,
                               Array<BindlessArray::TextureUpdateInfo>&& _update_textures,
                               Array<uint>&&                             _free_buffers,
                               Array<uint>&&                             _free_textures,
                               Array<uint>&&                             _free_slots) : Command(EType::UpdateBindlessArray), array(_array),
                                                            buffer_updates(std::move(_update_buffers)),
                                                            texture_updates(std::move(_update_textures)),
                                                            free_buffers(std::move(_free_buffers)),
                                                            free_textures(std::move(_free_textures)),
                                                            free_slots(std::move(_free_slots)) {}
        auto*       Handle() const { return array.Get(); }
        EQueueType  GetQueueType() const override { return EQueueType::Graphics; }
        const auto& BufferUpdates() const { return buffer_updates; }
        const auto& TextureUpdates() const { return texture_updates; }
        auto        StealBufferUpdates() const { return std::move(buffer_updates); }
        auto        StealTextureUpdates() const { return std::move(texture_updates); }
        auto        StealFreeBuffers() const { return std::move(free_buffers); }
        auto        StealFreeTextures() const { return std::move(free_textures); }
        auto        StealFreeSlots() const { return std::move(free_slots); }
    };
    struct DrawIndexedCmd {
        uint index_cnt;
        uint instance_cnt;
        uint first_index;
        uint vertex_offset;
        uint first_instance;
    };
    struct DrawIndirectCmd {
        BufferView commands;
        BufferView count;
        uint       max_cnt;
        uint       stride;
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
    using DrawCmd = std::variant<DrawIndexedCmd, DrawIndirectCmd>;
    struct SetDrawStateCmd : public Command {
    public:
    private:
        PipelineHandle                     pipeline{};
        RenderPassInfo                     render_pass_info;
        Array<MeshDrawData>                mesh_data;
        uint                               vtx_cnt;
        ArrayArguments                     args;
        GraphEventRef                      evaluate_mesh_task = nullptr;
        UnorderedMap<Buffer*, BufferRange> vertex_buffers;
        UnorderedMap<Buffer*, BufferRange> index_buffers;
        BindlessArrayRef                   bindless_array = nullptr;

        SetDrawStateCmd(ArrayArguments&& _args) : Command(EType::SetDrawState), args(std::move(_args)) {
            for (const auto& arg : args.args) {
                if (arg.index() == bindless_arg_type_idx) {
                    bindless_array = std::get<BindlessArrayRef>(arg);
                }
            }
        }

    public:
        SetDrawStateCmd(PipelineHandle        _pipeline,
                        ArrayArguments&&      _args,
                        RenderPassInfo&&      _info,
                        Array<MeshDrawData>&& _draw_data) : Command(EType::SetDrawState),
                                                            args(std::move(_args)),
                                                            pipeline(_pipeline),
                                                            render_pass_info(std::move(_info)),
                                                            mesh_data(std::move(_draw_data)),
                                                            vtx_cnt(0) {
            evaluate_mesh_task = LambdaTask::Create([this]() {
                                     for (const auto& mesh : mesh_data) {
                                         for (uint vtx_idx = 0; vtx_idx < mesh.vtx_cnt; ++vtx_idx) {
                                             const auto& vtx_view = mesh.vtx_views[vtx_idx];
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
                                     }
                                 }).Dispatch();
        }

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto        Pipeline() const { return pipeline; }
        const auto& RenderPassInfo() const { return render_pass_info; }
        const auto& DrawData() const { return mesh_data; }
        const auto& Args() const { return args; }
        auto BindlessArray() const { return bindless_array; }
        void        IterateArgs(std::function<void(const TArg&)> _func) const {
            for (const auto& arg : args.args) {
                std::visit([&_func](const auto& _arg) { _func(_arg); }, arg);
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
    };
    struct DispatchIndirectParam {
        BufferView indirect;
    };
    struct DispatchCmd : public Command {
    public:
        using DispatchParam = std::variant<uint3, DispatchIndirectParam>;

    private:
        PipelineHandle pipeline{};
        DispatchParam  param;
        ArrayArguments args;
        BindlessArrayRef bindless_array;
        DispatchCmd(ArrayArguments&& _args) : Command(EType::ShaderDispatch), args(std::move(_args)) {
            for (const auto& arg : args.args) {
                if (arg.index() == bindless_arg_type_idx) {
                    bindless_array = std::get<BindlessArrayRef>(arg);
                }
            }
        }

    public:
        DispatchCmd(ArrayArguments&& _args, PipelineHandle _handle, uint3 _param) : Command(EType::ShaderDispatch), param(_param), pipeline(_handle), args(std::move(_args)) {}
        DispatchCmd(ArrayArguments&& _args, PipelineHandle _handle, BufferView _indirect) : Command(EType::ShaderDispatch), pipeline(_handle), param(DispatchIndirectParam{_indirect}), args(std::move(_args)) {}

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& Args() const { return args; }
        const auto& Pipeline() const { return pipeline; }
        auto        Param() const { return param; }
        auto BindlessArray() const { return bindless_array; }
        void        IterateArgs(std::function<void(const TArg&)> _func) const {
            for (const auto& arg : args.args) {
                _func(arg);
            }
        }
    };

    struct BuildAccelerationStructuresCmd : public Command {
    private:
        BuildAccelerationStructuresCmd() : Command(EType::BuildAccel) {}

    public:
        BuildAccelerationStructuresCmd(Array<AccelerationStructureBuildParam> _params) : Command(EType::BuildAccel) {}
        BuildAccelerationStructuresCmd(Array<AccelerationStructureBuildParam>&& _params) : Command(EType::BuildAccel) {}

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& Params() const { return params; }

        auto& Scratch() const { return scratch_buffer; }

    private:
        Array<AccelerationStructureBuildParam> params;
        mutable BufferView                     scratch_buffer{};
    };

    struct BuildRaytracingSceneCmd : public Command {
    private:
        BuildRaytracingSceneCmd() : Command(EType::BuildTLAS) {}

    public:
        BuildRaytracingSceneCmd(Array<AccelerationStructureBuildParam> _params) : Command(EType::BuildTLAS) {}
        BuildRaytracingSceneCmd(Array<AccelerationStructureBuildParam>&& _params) : Command(EType::BuildTLAS) {}

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        const auto& Params() const { return params; }

        auto& Scratch() const { return scratch_buffer; }

    private:
        Array<AccelerationStructureBuildParam> params;
        mutable BufferView                     scratch_buffer{};
    };

    class RenderDevice::Impl {
    public:
        Impl() {}

        virtual ~Impl() = default;

    public:
        virtual FenceRef  CreateFence()                                                                = 0;
        virtual BufferRef CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) = 0;

        virtual BufferRef CreateStagingBuffer(uint64_t _byte_size) = 0;

        TextureRef CreateTexture(Extent2D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1) {
            return CreateTexture(Extent3D{_size.width, _size.height, 1}, _format, _usage, _mip_cnt, _array_size);
        };

        virtual TextureRef CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1) = 0;

        DepthBufferRef CreateDepthBuffer(Extent2D _size, EPixelFormat _format, uint32_t _array_size = 1) {
            return DepthBufferRef(MoerNew(DepthBuffer)(CreateTexture(_size, _format, ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED, 1, _array_size)));
        }

        virtual BindlessArrayRef CreateBindlessArray(uint _max_size) = 0;

        /// Raytracing
        virtual RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) = 0;

        // virtual RHIViewportRef CreateViewport(const RHIViewportInitializer& _init) = 0;

        // virtual BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport) = 0;

        // virtual void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
        void FlushPendingDeletes();

        const ShaderTargetInfo& GetShaderTargetInfo() const;

        virtual CommandQueue& GetCommandQueue(EQueueType _type) = 0;

        virtual SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) = 0;

        virtual PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) = 0;//gfx
        virtual PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders)                               = 0;//compute
    };

}// namespace Moer::Render
#endif