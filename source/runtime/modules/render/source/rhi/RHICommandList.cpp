//
// Created by 17152 on 2023/9/21.
//
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
                                  {},
                                  RHIDepthStencilStateInfo::Preset());

        GBufferLayout layout = manager.Raster()
                                   .Vertex("")
                                   .Pixel("")
                                   .Build<GBufferLayout>(std::move(pso_info));
        CommandList cmd_list;
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
        : cmd_list(_cmd_list), pso(_pso), args({}){
    }

    CommandList::DrawDispatcher::DrawDispatcher(
        RasterPipeline&  _pso,
        CommandList&     _cmd_list,
        ArrayArguments&& _args)
        : cmd_list(_cmd_list), args(std::move(_args)), pso(_pso){
    }

    CommandList::DrawDispatcher::DrawDispatcher(
        RasterPipeline& _pso,
        CommandList&    _cmd_list)
        : cmd_list(_cmd_list), pso(_pso), args({}){
    }

    void CommandList::ComputeDispatcher::Dispatch(uint3 _group_count) {
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _group_count));
    }

    void CommandList::ComputeDispatcher::DispatchIndirect(BufferView _indirect) {
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(std::move(args), pso.handle, _indirect));
    }

    CmdSubmit CommandList::Submit() {
        CmdSubmit submit{std::move(commands), std::move(callbacks)};
        return std::move(submit);
    }

    void
    CommandList::CopyFrom(BufferView _src, BufferView _dst) {
        commands.push_back(MakeUnique<CopyBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.byte_offset,
            _dst.byte_offset,
            _src.GetByteSize()));
    }
    void CommandList::CopyFrom(TextureView _src, TextureView _dst) {
        commands.push_back(MakeUnique<CopyTextureCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture),//reinterpret_cast<uint64
            reinterpret_cast<uint64>(_dst.texture),
            _src.mip_level,
            _dst.mip_level,
            _src.offset,
            _dst.offset,
            _src.extent));
    }
    void CommandList::CopyFrom(TextureView _src, BufferView _dst) {

        commands.push_back(MakeUnique<CopyTextureToBufferCmd>(
            _src.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.texture),
            reinterpret_cast<uint64>(_dst.GetBuffer()),
            _src.offset,
            _dst.byte_offset,
            _src.extent,
            _src.mip_level));
    }
    void CommandList::CopyFrom(BufferView _src, TextureView _dst) {
        commands.push_back(MakeUnique<CopyBufferToTextureCmd>(
            _dst.texture->GetFormat(),
            reinterpret_cast<uint64>(_src.GetBuffer()),
            reinterpret_cast<uint64>(_dst.texture),
            _src.byte_offset,
            _dst.offset,
            _dst.extent,
            _dst.mip_level));
    }

    void CommandList::CopyFrom(std::span<byte> _data, TextureView _texture) {
        //
        commands.push_back(MakeUnique<UploadTextureCmd>(
            _texture.texture->GetFormat(),
            reinterpret_cast<uint64>(_texture.texture),
            _texture.mip_level,
            _texture.offset,
            _texture.extent,
            _data.data()));
    }


    void CommandList::CopyFrom(std::span<byte> _data, BufferView _buffer) {
        //
        commands.push_back(MakeUnique<UploadBufferCmd>(
            reinterpret_cast<uint64>(_buffer.GetBuffer()),
            _buffer.GetByteOffset(),
            _buffer.GetByteSize(),
            _data.data()));
    }

    void CommandList::CopyFrom(BufferView _src, std::span<byte> _data) {
        //
        commands.push_back(MakeUnique<CopyBackBufferCmd>(
            reinterpret_cast<uint64>(_src.GetBuffer()),
            _src.GetByteOffset(),
            _src.GetByteSize(),
            _data.data()));
    }

    void CommandList::SetRenderCmds(PipelineHandle& _handle, ArrayArguments&& _args, RenderPassInfo&& _info, Array<MeshDrawData>&& _mesh_data) {
        commands.push_back(MakeUnique<SetDrawStateCmd>(_handle, std::move(_args), std::move(_info), std::move(_mesh_data)));
    }

    // void CommandList::SubmitArgs(ShaderPipeline& _pso, Arguments&& _args) {
    //     commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
    // }

    // void CommandList::SubmitConstants(ShaderPipeline& _pso, Array<uint>&& _constants) {
    //     commands.push_back(MakeUnique<SetConstantCmd>(_pso, std::move(_constants)));
    // }

    void CommandList::AddCallback(std::function<void()>&& _callback) {
        callbacks.emplace_back(std::move(_callback));
    }

    void CommandList::BeginBarriers(uint _read_tex_cnt, uint _write_tex_cnt, uint _read_buf_cnt, uint _write_buf_cnt) {
        commands.push_back(MakeUnique<BarrierCmd>(_read_tex_cnt, _write_tex_cnt, _read_buf_cnt, _write_buf_cnt));
        current_barriers = commands.back().get();
    }

    void CommandList::InnerReadBuffer(BufferView _buffer, EBufferState _state) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->ReadBuffer(_buffer.buffer, _state, EPassType::Graphics);
    }
    void CommandList::InnerWriteBuffer(BufferView _buffer, EBufferState _state) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->WriteBuffer(_buffer.buffer, _state, EPassType::Graphics);
    }

    void CommandList::InnerReadTexture(TextureView _texture, ETextureState _state) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->ReadTexture(_texture.texture, _state, EPassType::Graphics);
    }

    void CommandList::InnerWriteTexture(TextureView _texture, ETextureState _state) {
        BarrierCmd* barrier = static_cast<BarrierCmd*>(current_barriers);
        barrier->WriteTexture(_texture.texture, _state, EPassType::Graphics);
    }

    void CommandList::EndBarriers() {
        current_barriers = nullptr;
    }

}// namespace Moer::Render
