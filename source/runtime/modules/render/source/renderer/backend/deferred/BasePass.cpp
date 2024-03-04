#include "BasePass.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "../Common.h"

namespace Moer {
    struct BasePass::Impl {

    public:
        Impl();
        ~Impl();

    private:
        void DrawFrame(PassInput& input);
        void PrePass(RHIGraphicsCommandList* cmd_list);
        void BuildHZB();
        void PostPass();

    private:
        RHIGraphicsPipelineStateRef pipeline_state;
        RHIComputePipelineStateRef  cull_instance_pso;
        RHIComputePipelineStateRef  cull_meshlet_pso;
    };
}// namespace Moer