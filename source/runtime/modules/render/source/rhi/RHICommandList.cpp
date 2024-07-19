//
// Created by 17152 on 2023/9/21.
//
#include "misc/STL.h"
#include "rhi/RHICommand.h"
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
        layout.SetArgs(
            RHIBufferRef(), RHIBufferRef(), RHIBufferRef(), RHIBufferRef(), RHITextureRef(), RHITextureRef(), GBufferLayout::Constant{});
        CommandList  cmd_list;
        MeshDrawData draw_data;
        auto&&       draw_dispatcher = cmd_list.Gfx(layout, {}, std::move(draw_data));
        draw_dispatcher.Draw(3, 1, 0, 0, 0);
    }

    CommandList::CommandList() {
    }
    void CommandList::ArgSetter::SetBuffer(uint64 _index, BufferView _buffer) {
        auto idx          = handle.GetBindingIdx(_index);
        temp_args[_index] = _buffer;
    }

    void CommandList::ArgSetter::SetTexture(uint64 _index, TextureView _texture) {
        auto idx          = handle.GetBindingIdx(_index);
        temp_args[_index] = _texture;
    }

    void CommandList::ArgSetter::SetConstant(void* _data, uint _size) {
        temp_constant.resize(_size);
        std::memcpy(temp_constant.data(), _data, _size);
    }

    void CommandList::DrawDispatcher::SubmitArgsIfPossible() {
        if (HasParams()) {
            cmd_list.SubmitArgs(pso, arg_setter.StealArgs());
        }
        if (b_set_consts) {
            cmd_list.SubmitConstants(pso, arg_setter.StealConstants());
        }
        b_set_params = false;
        b_set_consts = false;
    }

    CommandList::ComputeDispatcher::ComputeDispatcher(
        ComputePipeline& _pso,
        CommandList&     _cmd_list,
        ArrayArguments&& _args)
        : cmd_list(_cmd_list), pso(_pso), arg_setter(_pso) {
        cmd_list.commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
    }

    void CommandList::ComputeDispatcher::Dispatch(uint3 _group_count) {
        SubmitArgsIfPossible();
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(_group_count));
    }

    void CommandList::ComputeDispatcher::DispatchIndirect(BufferView _indirect) {
        SubmitArgsIfPossible();
        cmd_list.commands.push_back(MakeUnique<DispatchCmd>(_indirect));
    }

    CmdSubmit CommandList::Submit() {
        CmdSubmit submit{std::move(commands)};
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

    void CommandList::BeginRenderPass(PipelineHandle& _handle, RenderPassInfo&& _info, MeshDrawData&& _mesh_data) {
        commands.push_back(MakeUnique<SetDrawStateCmd>(_handle, std::move(_info), std::move(_mesh_data.vertex_buffers), _mesh_data.index_buffer));
    }

    void CommandList::SubmitArgs(ShaderPipeline& _pso, Arguments&& _args) {
        commands.push_back(MakeUnique<SetParamsCmd>(_pso, std::move(_args)));
    }

    void CommandList::SubmitConstants(ShaderPipeline& _pso, Array<uint>&& _constants) {
        commands.push_back(MakeUnique<SetConstantCmd>(_pso, std::move(_constants)));
    }

    void CommandList::DrawDispatcher::Draw(uint32 _index_cnt, uint32 _instance_count, uint _vertex_offset, uint32 _first_vertex, uint32 _first_instance) {
        SubmitArgsIfPossible();
        DrawCmd cmd = DrawIndexedCmd{_index_cnt, _instance_count, _first_vertex, _vertex_offset, _first_instance};
        cmd_list.commands.push_back(MakeUnique<RenderCmd>(cmd));
    }

    void CommandList::DrawDispatcher::DrawIndirect(
        BufferView _indirect,
        BufferView _counter,
        uint       _stride,
        uint       _draw_count) {
        SubmitArgsIfPossible();
        DrawCmd cmd = DrawIndirectCmd{_indirect, _counter, _draw_count, _stride};
        cmd_list.commands.push_back(MakeUnique<RenderCmd>(cmd));
    }

}// namespace Moer::Render
