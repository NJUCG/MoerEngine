#include "GaussianCompositePass.h"
#include "PixelFormat.h"
#include "RasterTool.h"
#include "rhi/RHI.h"
#include "shaderheaders/shared/gaussian/GaussianRasterParameters.h"
namespace Moer::Render::Raster {
namespace {
class GaussianCompositePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianCompositePipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_TEX(scene_color);
    DEFINE_SHADER_TEX(gaussian_color);
    DEFINE_SHADER_TEX(transmittance);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(ranges);
    DEFINE_SHADER_TEX(output_color);
    DEFINE_SHADER_ARGS(param, scene_color, gaussian_color, transmittance, status, ranges, output_color);
};
class GaussianDebugPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(GaussianDebugPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_TEX(transmittance);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(ranges);
    DEFINE_SHADER_TEX(debug_color);
    DEFINE_SHADER_ARGS(param, transmittance, status, ranges, debug_color);
};

} // namespace
struct GaussianCompositePass::Impl {
    GaussianCompositePipeline pipeline;
    GaussianDebugPipeline     debug_pipeline;
    BindlessArrayRef          bindless;
    TextureWithHandle         output{};
    QueryFuture               timing{};
    double                    gpu_ms = 0;
    Impl(ShaderManager& manager, BindlessArrayRef array) : bindless(std::move(array)) {
        pipeline = manager.Compute<GaussianCompositePipeline>("features/gaussian/Composite.comp.hlsl");
        GfxPsoCreateInfo info(
            RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
        );
        debug_pipeline = manager.Raster()
                             .Vertex("core/utils/FullScreenQuad.hlsl")
                             .Pixel("features/gaussian/Debug.frag.hlsl")
                             .Build<GaussianDebugPipeline>(std::move(info));
    }
    ~Impl() {
        if (output.tex)
            bindless->UnbindTexture(output.hdl);
    }
};
GaussianCompositePass::GaussianCompositePass(ShaderManager& manager, BindlessArrayRef bindless) :
    m_impl(std::make_unique<Impl>(manager, std::move(bindless))) {}
GaussianCompositePass::~GaussianCompositePass() = default;
double GaussianCompositePass::GetGpuTime() {
    auto& state = *m_impl;
    if (state.timing.Valid() && state.timing.Status() != QueryStatus::Pending) {
        const auto result = state.timing.Get();
        if (const auto* value = std::get_if<TimestampQueryResult>(&result.payload))
            state.gpu_ms = value->duration_ns / 1000000.0;
        state.timing = {};
    }
    return state.gpu_ms;
}
const TextureWithHandle& GaussianCompositePass::GetOutput() const {
    return m_impl->output;
}
GaussianCompositePass::RecordParameters GaussianCompositePass::Prepare(
    TextureWithHandle  input,
    TextureRef         color,
    TextureRef         transmittance,
    BufferRef          status,
    BufferRef          ranges,
    EGaussianDebugView debug_view
) {
    auto& state = *m_impl;
    if (!state.output.tex || state.output.GetSize() != input.GetSize()) {
        if (state.output.tex)
            state.bindless->UnbindTexture(state.output.hdl);
        const auto size  = input.GetSize();
        state.output.tex = RenderDevice::Get().CreateTexture(
            "Gaussian scene composite",
            Extent3D(size.x, size.y, 1),
            PF_R32G32B32A32_SFLOAT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
                ETextureUsageFlags::TRANSFER_SRC
        );
        state.output.hdl = state.bindless->AllocateTexture(
            state.output.tex->GetView(), Sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE}
        );
    }
    return {
        std::move(input),
        state.output,
        std::move(color),
        std::move(transmittance),
        std::move(status),
        std::move(ranges),
        debug_view
    };
}
void GaussianCompositePass::Record(CommandList& commands, const RecordParameters& parameters) {
    QueryToken token;
    if (!m_impl->timing.Valid()) {
        token          = commands.BeginTimestampQuery("Gaussian composite");
        m_impl->timing = token.GetFuture();
    }
    commands.PushScopeWithTimeScope("Gaussian composite");
    commands.UpdateBindlessArray(m_impl->bindless);
    GaussianDispatchParams param{};
    const auto             size = parameters.output.GetSize();
    param.info                  = uint4(size.x, size.y, (size.x + 15) / 16, 0);
    param.job                   = uint4(uint(parameters.debug_view), 0, 0, 0);
    commands
        .Compute(
            m_impl->pipeline,
            param,
            parameters.input.tex->GetView(),
            parameters.color->GetView(),
            parameters.transmittance->GetView(),
            parameters.status->GetView(),
            parameters.ranges->GetView(),
            parameters.output.tex->GetView()
        )
        .Dispatch(uint3((size.x + 15) / 16, (size.y + 15) / 16, 1), "Gaussian composite");
    commands.AddCallback([parameters]() {});
    commands.PopScopeWithTimeScope();
    if (token.Valid())
        commands.EndTimestampQuery(token);
}
RenderGraph::PassHandle GaussianCompositePass::AddToGraph(
    RenderGraph&               graph,
    RecordParameters           parameters,
    RenderGraph::TextureHandle input,
    RenderGraph::TextureHandle color,
    RenderGraph::TextureHandle transmittance,
    RenderGraph::BufferHandle  status,
    RenderGraph::BufferHandle  ranges,
    RenderGraph::TextureHandle output
) {
    return graph.AddRecordPass(
        "GaussianComposite",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(input).Read(color).Read(transmittance).Read(status).Read(ranges).Write(output);
        },
        [this, parameters](CommandList& commands) {
            Record(commands, parameters);
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
}
void GaussianCompositePass::RecordDebug(
    CommandList&            commands,
    const RecordParameters& parameters,
    TextureRef              output
) {
    GaussianDispatchParams param{};
    const auto             size = parameters.output.GetSize();
    param.info                  = uint4(size.x, size.y, (size.x + 15) / 16, 0);
    param.job = uint4(uint(parameters.debug_view), uint(parameters.is_shadow_depth_valid), 0, 0);
    commands
        .Gfx(
            m_impl->debug_pipeline,
            param,
            parameters.transmittance->GetView(),
            parameters.status->GetView(),
            parameters.ranges->GetView(),
            parameters.color->GetView()
        )
        .Draw(
            "Gaussian debug view",
            parameters.output.GetRect2D(),
            RasterTool::GetFullScreenDrawDatas(),
            ColorAttachment(output)
        );
    commands.AddCallback([parameters, output]() {});
}

} // namespace Moer::Render::Raster
