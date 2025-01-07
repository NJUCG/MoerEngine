#ifndef MOER_LIGHTING_PASS_H
#define MOER_LIGHTING_PASS_H
#include "RTResource.h"
#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"
namespace Moer {
    class Scene;
    namespace Render {
        class PresampleLightGridPipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(PresampleLightGridPipeline);
        };

        class GenerateInitialSamplePipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(GenerateInitialSamplePipeline);
        };

        class TemporalResmaplePipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(TemporalResmaplePipeline);
        };

        class SpatialResamplePipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(SpatialResamplePipeline);
        };

        class FusedResamplingPipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(FusedResamplingPipeline);
        };

        class DIShadeSamplePipeline : public ComputePipeline {
        public:
            DEFINE_COMPUTE_PIPELINE_CLASS(DIShadeSamplePipeline);
        };

        class LightingPass {
        public:
            LightingPass(RenderDevice& _device, class ShaderManager& _manager, Scene& _scene);

            void Process(CommandList& _cmd_list, RTContext& _rt_ctx);
        };
    }// namespace Render
}// namespace Moer
#endif