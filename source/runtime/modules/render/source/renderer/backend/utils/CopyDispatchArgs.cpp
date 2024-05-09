#include "CopyDispatchArgs.h"
#include "rendergraph/RenderGraphPass.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"

IMPLEMENT_SHADER_TYPE(CopyDispatchArgsShader, "utils/CopyDispatchArgs.hlsl", "main", ST_COMPUTE)
namespace Moer {

    struct CopyDispatchArgs::Impl {
        RHIComputePipelineStateRef pipeline_state;
        RHIShaderRef               shader;
        static void                Init(RenderContext& _context) {
            Get().InitInternal(_context);
        }

        static void Dispose() {
            Get().DisposeInternal();
        }

        static CopyDispatchArgs::Impl& Get() {
            static Impl instance;
            return instance;
        }

    private:
        void InitInternal(RenderContext& _context) {
            shader         = ShaderResourceManager::GetInstance().GetShader<CopyDispatchArgsShader>();
            pipeline_state = g_rhi->RHICreateComputePipelineState(shader);
        }
        void DisposeInternal() {
            pipeline_state = nullptr;
            shader         = nullptr;
        }
    };

    void CopyDispatchArgs::Init(RenderContext& _context) {
        Impl::Init(_context);
    }
    void CopyDispatchArgs::Dispose() {
        Impl::Dispose();
    }

    void CopyDispatchArgs::Dispatch(RenderContext& _context, std::string_view _src_name, std::string_view _target_name, RHISRVRef _src_buffer, RHIUAVRef _target, uint32_t _src_offset, uint32_t _dst_offset, uint _group_size) {
        auto& rg = _context.GetRenderGraph();
        rg.AddComputePass(
            "Copy Dispatch Args", [&](RenderGraph::Builder& _builder) {
                auto& blackboard = rg.GetBlackBoard();

                auto src_handle    = rg.ImportIfNotExist(_src_name.data(), _src_buffer->GetBuffer());
                auto target_handle = rg.ImportIfNotExist(_target_name.data(), _target->GetBuffer());
                _builder.ReadBuffer(src_handle, EBufferLayout::READ);
                _builder.WriteBuffer(target_handle, EBufferLayout::WRITE); }, [&_context, _src_buffer, _target, _src_offset, _dst_offset, _group_size](RenderPassContext& _pass_context) {
                auto& cmd_list = *_pass_context.cmd_list;
                CopyDispatchArgsShader::Parameters params;
                params.args.src_offset = _src_offset;
                params.args.dst_offset = _dst_offset;
                params.args.group_size = _group_size;
                params.src_buffer      = _src_buffer;
                params.target          = _target;

                auto& impl = Impl::Get();

                RHIBatchedShaderParameters batched_params;
                batched_params.SetParameters(impl.shader, params);
                g_rhi->RHISetBatchedShaderParameters(impl.pipeline_state, batched_params);
                cmd_list.SetPipelineState(impl.pipeline_state);
                cmd_list.Dispatch(1, 1, 1); });
    }
}// namespace Moer