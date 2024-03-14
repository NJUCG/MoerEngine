//
// Created by 17152 on 2023/9/21.
//
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
RHICommandListBase::RHICommandListBase() {
}
RHICommandListBase::~RHICommandListBase() {
}

namespace Moer {
    struct SubmitBundleInfo {
        RHICommandListBase* cmd_list;
        RHISubmitInfo       submit_info;
    };
    class CommandListManager {
        friend class RHICommandList;

    public:
        struct FrameCommandContext {
            Moer::Array<SubmitBundleInfo> submit_bundles;
            void                          ExecuteCommandLists() {
            }
        };
        CommandListManager() {
        }
        ~CommandListManager() {
        }

    private:
        //do they need to be thread local?
        Moer::Array<RHIGraphicsCommandList*> graphics_cmd_lists;
        Moer::Array<RHIComputeCommandList*>  compute_cmd_lists;
        Moer::Array<RHICopyCommandList*>     copy_cmd_lists;

        RHICommandQueue* graphics_queue;
        RHICommandQueue* compute_queue;
        RHICommandQueue* copy_queue;
    };
    RHICommandList::RHICommandList() {
    }
    struct RHICommandList::Impl {
        Impl() {
        }
        ~Impl() {
        }
        void SetViewport(const RHIViewport& _viewport) {
        }

        void SetBatchedShaderParmeters(RHIBatchedShaderParameters _params) {
        }

        void SetPSO(RHIGraphicsPipelineState* _graphics_pso){

        };

        void SetPSO(RHIComputePipelineState* _compute_pso){

        };

        void DrawIndexedInstanced(
            uint32_t _index_count,
            uint32_t _instance_count,
            uint32_t _start_index_location,
            uint32_t _start_vertex_location,
            uint32_t _start_instance_location){

        };

        void DrawIndexedIndirect(
            RHIBuffer* _argument_buffer,
            uint64_t   _arg_offset,
            RHIBuffer* _count_buffer,
            uint64_t   _count_buffer_offset,
            uint32_t   _max_draw_count,
            uint32_t   _stride);

        void Dispatch(Moer::Vector3i _group_count) {
            Dispatch(_group_count.x, _group_count.y, _group_count.z);
        }
        void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z){

        };

        void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset);

        void CopyBufferFrom(RHIBuffer* _src, RHIBuffer* _dst, uint64_t _src_offset, uint64_t _dst_offset, uint64_t _size) {
        }

        void CopyBufferFrom(RHIBuffer* _src, void* data, uint64_t _size) {
        }

        void CopyBufferTo(RHIBuffer* _src, void* data, uint64_t _size) {
        }

        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst){};
        void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture){};

        void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer){};

        //To resolve a multi-sample color texture to a non-multisample color texture
        void ResolveTexture(const RHIResolveTextureInfo& _resolve_info, RHITexture* _src, RHITexture* _dst){};

        void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency){};

        void SetScissor(const Rect2D& _scissor){};

        void BindVertexBuffers(
            uint32_t                   _start_offset,
            const Array<RHIBufferRef*> _vertex_buffers,
            const Array<uint32_t>      _offsets){};

        void BindIndexBuffer(
            const RHIBuffer*  p_index_buffer,
            uint32_t          _offset,
            EIndexElementType _type){};

        void SetAttachments() {
        }

        void BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name){};
        void EndRenderPass(){};
        //raw cmd lists
        Moer::Array<RHIGraphicsCommandList*> graphics_cmd_lists;
        RHIComputeCommandList*               compute_cmd_list;
        RHICopyCommandList*                  copy_cmd_list;
    };

}// namespace Moer
