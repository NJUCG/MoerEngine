//
// Created by 17152 on 2023/9/21.
//
#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "RHIImpl.h"
RHICommandListBase::RHICommandListBase() {
}
RHICommandListBase::~RHICommandListBase() {
}
namespace Moer::Render {

    void CommandQueue::Test() {
        RenderDevice* device;
        ShaderManager manager(*device);

        GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                                  VertexStream(),
                                  {},
                                  RHIDepthStencilStateInfo::Preset(),
                                  PF_D32_SFLOAT_S8_UINT);

        GBufferLayout layout = manager.Raster()
                                   .Vertex("")
                                   .Pixel("")
                                   .Build<GBufferLayout>(std::move(pso_info));
        CommandList         cmd_list;
        Array<MeshDrawData> draw_data;
        auto&&              draw_dispatcher = cmd_list.Gfx(layout);
        draw_dispatcher.Draw(Rect2D{}, std::move(draw_data), ColorAttachment{nullptr});
    }

    CommandList::CommandList() {
    }
    // void CommandList::ArgSetter::SetBuffer(uint64 _index, BufferView _buffer) {
    //     auto idx          = handle.GetBindingIdx(_index);
    //     temp_args[_index] = _buffer;
    // }

    // void CommandList::ArgSetter::SetTexture(uint64 _index, TextureView _texture) {
    //     auto idx          = handle.GetBindingIdx(_index);
    //     temp_args[_index] = _texture;
    // }

    // void CommandList::ArgSetter::SetConstant(void* _data, uint _size) {
    //     temp_constant.resize(_size);
    //     std::memcpy(temp_constant.data(), _data, _size);
    // }

    // void CommandList::DrawDispatcher::SubmitArgsIfPossible() {
    //     if (HasParams()) {
    //         cmd_list.SubmitArgs(pso, arg_setter.StealArgs());
    //     }
    //     if (b_set_consts) {
    //         cmd_list.SubmitConstants(pso, arg_setter.StealConstants());
    //     }
    //     b_set_params = false;
    //     b_set_consts = false;
    // }

    CommandList::ComputeDispatcher::ComputeDispatcher(
        ComputePipeline& _pso,
        CommandList&     _cmd_list,
        ArrayArguments&& _args)
        : cmd_list(_cmd_list), pso(_pso), args(std::move(_args)) {
        // cmd_list.commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
    }
    CommandList::ComputeDispatcher::ComputeDispatcher(
        ComputePipeline& _pso,
        CommandList&     _cmd_list)
        : cmd_list(_cmd_list), pso(_pso), args({}) {
    }

    CommandList::DrawDispatcher::DrawDispatcher(
        RasterPipeline&  _pso,
        CommandList&     _cmd_list,
        ArrayArguments&& _args)
        : cmd_list(_cmd_list), args(std::move(_args)), pso(_pso) {
    }

    CommandList::DrawDispatcher::DrawDispatcher(
        RasterPipeline& _pso,
        CommandList&    _cmd_list)
        : cmd_list(_cmd_list), pso(_pso), args({}) {
    }

    void CommandList::ComputeDispatcher::Dispatch(uint3 _group_count, std::string_view _name, ProfileSection _section) {
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _group_count));
        cmd_list.commands.back()->name = _name;
    }

    void CommandList::ComputeDispatcher::DispatchIndirect(BufferView _indirect, std::string_view _name, ProfileSection _section) {
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _indirect));
        cmd_list.commands.back()->name = _name;
    }

    CmdSubmit CommandList::Submit() {
        CmdSubmit submit(std::move(commands), std::move(callbacks), bindless_array);
        return std::move(submit);
    }

    void
    CommandList::CopyFrom(BufferView _src, BufferView _dst, std::string_view _name) {
        commands.push_back(MakeUnique<CopyBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.byte_offset,
            _dst.byte_offset,
            _src.GetByteSize(),
            _name));
    }
    void CommandList::CopyFrom(TextureView _src, TextureView _dst, std::string_view _name) {
        commands.push_back(MakeUnique<CopyTextureCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture),//reinterpret_cast<uint64
            reinterpret_cast<uint64>(_dst.texture),
            _src.mip_level,
            _dst.mip_level,
            _src.offset,
            _dst.offset,
            _src.extent,
            _name));
    }
    void CommandList::CopyFrom(TextureView _src, BufferView _dst, std::string_view _name) {

        commands.push_back(MakeUnique<CopyTextureToBufferCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.offset,
            _dst.byte_offset,
            _src.extent,
            _src.mip_level,
            _name));
    }
    void CommandList::CopyFrom(BufferView _src, TextureView _dst, std::string_view _name) {
        commands.push_back(MakeUnique<CopyBufferToTextureCmd>(
            _dst.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.texture),
            _src.byte_offset,
            _dst.offset,
            _dst.extent,
            _dst.mip_level,
            _name));
    }

    void CommandList::CopyFrom(std::span<byte> _data, TextureView _texture, std::string_view _name) {
        //
        uint3 extent = uint3(
            std::max(uint(_texture.extent.x) >> _texture.mip_level, 1u),
            std::max(uint(_texture.extent.y) >> _texture.mip_level, 1u),
            std::max(uint(_texture.extent.z) >> _texture.mip_level, 1u));
        commands.push_back(MakeUnique<UploadTextureCmd>(
            _texture.texture->GetFormat(),
            reinterpret_cast<uint64>(_texture.texture),
            _texture.mip_level,
            _texture.offset,
            extent,
            _data.data(),
            _name));
    }

    void CommandList::CopyFrom(std::span<byte> _data, BufferView _buffer, std::string_view _name) {
        //
        if (_data.size() == 0) {
            return;
        }
        commands.push_back(MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_buffer.GetBuffer()),
            _buffer.GetByteOffset(),
            _data.size_bytes(),
            _data.data(),
            _name));
    }

    void CommandList::CopyFrom(BufferView _src, std::span<byte> _data, std::string_view _name) {
        //
        commands.push_back(MakeUnique<CopyBackBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            _src.GetByteOffset(),
            _src.GetByteSize(),
            _data.data(),
            _name));
    }

    void CommandList::CopyFrom(TextureView _src, std::span<byte> _data, std::string_view _name) {
        //check if size is 0
        bool b_valid_size = _data.size() > 0 && _src.GetTexture();
        if (!b_valid_size) {
            return;
        }
        uint mip_size = _src.GetTexture()->GetMipByteSize(_src.mip_level);
        b_valid_size &= mip_size <= _data.size();

        assert(b_valid_size && "Fatal: Copy back data size is less than texture mip size");
        commands.push_back(MakeUnique<CopyBackTextureCmd>(
            reinterpret_cast<uint64>(_src.texture),
            _src.mip_level,
            _src.offset,
            _src.extent,
            std::span<byte>(_data.data(), mip_size),
            _name));
    }

    void CommandList::CopyFrom(Array<byte>&& _data, BufferView _dst, std::string_view _name) {
        //
        if (_data.size() == 0) {
            return;
        }
        commands.push_back(MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _dst.GetByteOffset(),
            _data.size(),
            std::move(_data),
            _name));
    }

    void CommandList::CopyFrom(Array<byte>&& _data, TextureView _dst, std::string_view _name) {
        //
        if (_data.size() == 0) {
            return;
        }
        commands.push_back(MakeUnique<UploadTextureCmd>(
            _dst.texture->GetFormat(),
            reinterpret_cast<uint64>(_dst.texture),
            _dst.mip_level,
            _dst.offset,
            _dst.extent,
            std::move(_data),
            _name));
    }

    void CommandList::SetRenderCmds(PipelineHandle& _handle, ArrayArguments&& _args, RenderPassInfo&& _info, Array<MeshDrawData>&& _mesh_data, std::optional<std::string_view> _name) {
        commands.push_back(MakeUnique<SetDrawStateCmd>(_handle, std::move(_args), std::move(_info), std::move(_mesh_data)));
        if (_name.has_value()) {
            commands.back()->name = *_name;
        }
    }

    void CommandList::UpdateBindlessArray(BindlessArrayRef _array) {
        assert(_array && "Bindless array is null");

        commands.push_back(_array->CreateUpdateCommand());
    }

    void CommandList::ClearResource(BufferView _buffer, uint _value) {
        commands.emplace_back(MakeUnique<ClearResourceCmd>(_buffer, _value));
    }

    void CommandList::ClearResource(TextureView _texture, float4 _color) {
        commands.emplace_back(MakeUnique<ClearResourceCmd>(_texture, _color));
    }

    void CommandList::ClearResource(TextureView _texture, uint _value) {
        commands.emplace_back(MakeUnique<ClearResourceCmd>(_texture, _value));
    }
#pragma region[ raytracing ]

    void CommandList::BuildAccelerationStructures(Array<AccelerationStructureBuildParam>&& _geometries) {
        commands.emplace_back(MakeUnique<BuildAccelerationStructuresCmd>(std::move(_geometries)));
    }

    void CommandList::UpdateRaytracingScene(RaytracingSceneRef _scene) {
        commands.emplace_back(_scene->UpdateScene());
    }

#pragma endregion
    // void CommandList::SubmitArgs(ShaderPipeline& _pso, Arguments&& _args) {
    //     commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
    // }

    // void CommandList::SubmitConstants(ShaderPipeline& _pso, Array<uint>&& _constants) {
    //     commands.push_back(MakeUnique<SetConstantCmd>(_pso, std::move(_constants)));
    // }

#pragma region[ custom commands ]

    void CommandList::AddCustomCommand(UniquePtr<Command>&& _cmd, std::string_view _name) {
        commands.push_back(std::move(_cmd));
        commands.back()->name = _name;
    }

#pragma endregion

    void CommandList::AddCallback(std::function<void()>&& _callback) {
        callbacks.emplace_back(std::move(_callback));
    }

    void CommandList::BeginBarriers(uint _read_tex_cnt, uint _write_tex_cnt, uint _read_buf_cnt, uint _write_buf_cnt, EQueueType _src_queue, EQueueType _dst_queue) {
        commands.push_back(MakeUnique<BarrierCmd>(_read_tex_cnt, _write_tex_cnt, _read_buf_cnt, _write_buf_cnt, _src_queue, _dst_queue));
        current_barriers = commands.back().get();
    }

    void CommandList::InnerReadBuffer(BufferView _buffer, EBufferState _state, EPassType _pass) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->ReadBuffer(_buffer.buffer, _state, _pass);
    }
    void CommandList::InnerWriteBuffer(BufferView _buffer, EBufferState _state, EPassType _pass) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->WriteBuffer(_buffer.buffer, _state, _pass);
    }

    void CommandList::InnerReadTexture(TextureView _texture, ETextureState _state, EPassType _pass) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->ReadTexture(_texture.texture, _state, _pass);
    }

    void CommandList::InnerWriteTexture(TextureView _texture, ETextureState _state, EPassType _pass) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->WriteTexture(_texture.texture, _state, _pass);
    }

    void CommandList::EndBarriers() {
        current_barriers = nullptr;
    }

    void CommandList::ImportTextureFromQueue(EQueueType _src_queue, Array<ImportTexture>&& _textures_to_import) {
        // QueueTransferCmd cmd(_src_queue, std::move(_textures_to_import));
        commands.emplace_back(MakeUnique<QueueTransferCmd>(_src_queue, std::move(_textures_to_import)));
    }

    void CommandList::ExportTextureToQueue(EQueueType _dst_queue, Array<ExportTexture>&& _textures_to_export) {
        commands.emplace_back(MakeUnique<QueueTransferCmd>(_dst_queue, std::move(_textures_to_export)));
    }

}// namespace Moer::Render
