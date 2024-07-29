#ifndef MOER_RHI_IMPL_H
#define MOER_RHI_IMPL_H
#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include <variant>
namespace Moer::Render {
    struct UploadBufferCmd : public Command {
    private:
        uint64      handle{};
        uint64      offset{};
        uint64      byte_size{};
        const void* data{};

        UploadBufferCmd() : Command(EType::UploadBuffer) {}

    public:
        UploadBufferCmd(
            uint64      _handle,
            uint64      _offset,
            uint64      _byte_size,
            const void* _data) : Command(EType::UploadBuffer), handle(_handle), offset(_offset), byte_size(_byte_size), data(_data) {}
        EQueueType GetQueueType() const override { return EQueueType::Copy; }
        auto       Handle() const { return handle; }
        auto       Offset() const { return offset; }
        auto       ByteSize() const { return byte_size; }
        auto       Data() const { return data; }
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
        uint         src_offset[3]{};
        uint         dst_offset[3]{};
        uint         size[3]{};

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
        uint         dst_offset[3]{};
        uint         size[3]{};
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
        auto MipLevel() const { return mip_level; }
    };

    struct CopyTextureToBufferCmd : public Command {
    public:
        EPixelFormat format{};
        uint64       src_handle{};
        uint64       dst_handle{};
        uint         src_offset[3]{};
        uint         dst_offset{};
        uint         size[3]{};
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
        auto MipLevel() const { return mip_level; }
    };

    struct UploadTextureCmd : public Command {
    public:
        EPixelFormat format{};
        uint64       handle{};
        uint         mip_level{};
        uint         offset[3]{};
        uint         size[3]{};
        const void*  data{};

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
                                 data(_data) {
        }

        EQueueType GetQueueType() const override { return EQueueType::Copy; }

        auto Format() const { return format; }
        auto Handle() const { return handle; }
        auto MipLevel() const { return mip_level; }
        auto Offset() const { return offset; }
        auto Size() const { return size; }
        auto Data() const { return data; }
    };

    using ResourceState = std::variant<EBufferRuntimeUsageFlags, ETextureStateFlags>;
    struct BarrierCmd : public Command {
    private:
        ResourceState dst_state{};
        EPassType     pass_type{};
        uint64        handle{};

        BarrierCmd() : Command(EType::Barrier) {}

    public:
        BarrierCmd(
            EPassType     _pass_type,
            uint64        _handle,
            ResourceState _dst_state) : Command(EType::Barrier), pass_type(_pass_type), handle(_handle), dst_state(_dst_state) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto PassType() const { return pass_type; }
        auto Handle() const { return handle; }
        auto DstState() const { return dst_state; }
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

    using DrawCmd = std::variant<DrawIndexedCmd, DrawIndirectCmd>;
    struct SetDrawStateCmd : public Command {
    public:
    private:
        PipelineHandle pipeline{};
        RenderPassInfo render_pass_info;
        Array<VertexBuffer>
                    vertex_buffers;
        IndexBuffer index_buffer;
        SetDrawStateCmd() : Command(EType::SetDrawState) {}

    public:
        SetDrawStateCmd(PipelineHandle        _pipeline,
                        RenderPassInfo&&      _info,
                        Array<VertexBuffer>&& _vertex_buffers,
                        IndexBuffer           _index_buffer) : Command(EType::SetDrawState),
                                                     pipeline(_pipeline),
                                                     render_pass_info(std::move(_info)),
                                                     vertex_buffers(std::move(_vertex_buffers)),
                                                     index_buffer(_index_buffer) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto        Pipeline() const { return pipeline; }
        const auto& RenderPassInfo() const { return render_pass_info; }
        const auto& VertexBuffers() const { return vertex_buffers; }
        const auto& IndexBuffer() const { return index_buffer; }
    };

    struct RenderCmd : public Command {
    private:
        DrawCmd draw_cmd;

    private:
        RenderCmd() : Command(EType::Draw) {}

    public:
        RenderCmd(DrawCmd _draw_cmd) : Command(EType::Draw), draw_cmd(_draw_cmd) {}
        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto DrawCmd() const { return draw_cmd; }
    };

    struct UpdateDrawStateCmd : public Command {
    private:
        Rect2D scissor;
        uint4  viewport;

    private:
        UpdateDrawStateCmd() : Command(EType::UpdateDrawState) {}

    public:
        UpdateDrawStateCmd(Rect2D _scissor, uint4 _viewport) : Command(EType::UpdateDrawState), scissor(_scissor), viewport(_viewport) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto Scissor() const { return scissor; }
        auto Viewport() const { return viewport; }
    };

    struct SetParamsCmd : public Command {
    private:
        using TArguments = std::variant<Arguments, ArrayArguments>;

        TArguments      args;
        ShaderPipeline& pso;

    private:
    public:
        SetParamsCmd(ShaderPipeline& _pso, Arguments&& _args) : Command(EType::SetParams), args(std::move(_args)), pso(_pso) {}
        SetParamsCmd(ShaderPipeline& _pso, ArrayArguments&& _args) : Command(EType::SetParams), args(std::move(_args)), pso(_pso) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto&& StealArgs() const { return std::move(args); }

        auto& Pso() const { return pso; }
    };

    struct SetConstantCmd : public Command {
    private:
        Array<uint>     data;
        ShaderPipeline& pso;

    private:
    public:
        SetConstantCmd(ShaderPipeline& _pso, Array<uint>&& _data) : Command(EType::SetConstants), data(std::move(_data)), pso(_pso) {}

        EQueueType GetQueueType() const override { return EQueueType::Graphics; }

        auto&& StealData() const { return std::move(data); }

        auto& Pso() const { return pso; }
    };
    struct DispatchIndirectParam {
        BufferView indirect;
    };
    struct DispatchCmd : public Command {
    public:
        using DispatchParam = std::variant<uint3, DispatchIndirectParam>;

    private:
        DispatchParam param;
        DispatchCmd() : Command(EType::Dispatch) {}

    public:
        DispatchCmd(uint3 _param) : Command(EType::Dispatch), param(_param) {}
        DispatchCmd(BufferView _indirect) : Command(EType::Dispatch), param(DispatchIndirectParam{_indirect}) {}

        EQueueType GetQueueType() const override { return EQueueType::Compute; }

        auto Param() const { return param; }
    };

    class CmdVisitor {
    public:
        virtual void Visit(const UploadBufferCmd& _cmd)        = 0;
        virtual void Visit(const CopyBackBufferCmd& _cmd)      = 0;
        virtual void Visit(const CopyBufferCmd& _cmd)          = 0;
        virtual void Visit(const CopyTextureCmd& _cmd)         = 0;
        virtual void Visit(const CopyBufferToTextureCmd& _cmd) = 0;
        virtual void Visit(const CopyTextureToBufferCmd& _cmd) = 0;
        virtual void Visit(const UploadTextureCmd& _cmd)       = 0;
        virtual void Visit(const BarrierCmd& _cmd)             = 0;
        virtual void Visit(const SetDrawStateCmd& _cmd)        = 0;
        virtual void Visit(const RenderCmd& _cmd)              = 0;
        virtual void Visit(const SetParamsCmd& _cmd)           = 0;
        virtual void Visit(const SetConstantCmd& _cmd)         = 0;
        virtual void Visit(const DispatchCmd& _cmd)            = 0;
    };
    class RenderDevice::Impl {
    public:
        Impl(DeviceInitInfo&& _info) {}

        virtual ~Impl() = default;

    public:
        virtual FenceRef CreateFence(EFenceUsageFlags _usage = EFenceUsageFlags::TIMELINE) = 0;
        FenceRef         CreateTimeline() {
            return CreateFence(EFenceUsageFlags::TIMELINE);
        }
        FenceRef CreatePresentFence() {
            return CreateFence(EFenceUsageFlags::PRESENT);
        };
        virtual BufferRef CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) = 0;

        virtual BufferRef CreateStagingBuffer(uint64_t _byte_size) = 0;

        TextureRef CreateTexture(Extent2D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1) {
            return CreateTexture(Extent3D{_size.width, _size.height, 1}, _format, _usage, _mip_cnt, _array_size);
        };

        virtual TextureRef CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1) = 0;

        DepthBufferRef CreateDepthBuffer(Extent2D _size, EPixelFormat _format, uint32_t _array_size = 1) {
            return DepthBufferRef(MoerNew(DepthBuffer)(CreateTexture(_size, _format, ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED, 1, _array_size)));
        }

        virtual RHIViewportRef CreateViewport(const RHIViewportInitializer& _init) = 0;

        virtual BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport) = 0;

        virtual void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
        void         FlushPendingDeletes();

        const ShaderTargetInfo& GetShaderTargetInfo() const;

        virtual CommandQueue& GetCommandQueue(EQueueType _type) = 0;

        virtual SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) = 0;

        virtual PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) = 0;//gfx
        virtual PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders)                               = 0;//compute
    };

    
}// namespace Moer::Render
#endif