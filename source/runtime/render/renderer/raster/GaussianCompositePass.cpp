#include "GaussianCompositePass.h"
#include "PixelFormat.h"
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
}
struct GaussianCompositePass::Impl {
    GaussianCompositePipeline pipeline;
    BindlessArrayRef bindless;
    TextureWithHandle output{};
    Impl(ShaderManager& manager, BindlessArrayRef array) : bindless(std::move(array)) {
        pipeline = manager.Compute<GaussianCompositePipeline>("features/gaussian/Composite.comp.hlsl");
    }
    ~Impl() { if (output.tex) bindless->UnbindTexture(output.hdl); }
};
GaussianCompositePass::GaussianCompositePass(ShaderManager& manager, BindlessArrayRef bindless) :
    m_impl(std::make_unique<Impl>(manager, std::move(bindless))) {}
GaussianCompositePass::~GaussianCompositePass() = default;
const TextureWithHandle& GaussianCompositePass::GetOutput() const { return m_impl->output; }
GaussianCompositePass::RecordParameters GaussianCompositePass::Prepare(TextureWithHandle input,
    TextureRef color, TextureRef transmittance, BufferRef status, BufferRef ranges, EGaussianDebugView debug_view) {
    auto& state = *m_impl;
    if (!state.output.tex || state.output.GetSize() != input.GetSize()) {
        if (state.output.tex) state.bindless->UnbindTexture(state.output.hdl);
        const auto size = input.GetSize();
        state.output.tex = RenderDevice::Get().CreateTexture("Gaussian scene composite", Extent3D(size.x,size.y,1),
            PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
            ETextureUsageFlags::TRANSFER_SRC);
        state.output.hdl = state.bindless->AllocateTexture(state.output.tex->GetView(),
            Sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE});
    }
    return {std::move(input), state.output, std::move(color), std::move(transmittance),
        std::move(status), std::move(ranges), debug_view};
}
void GaussianCompositePass::Record(CommandList& commands, const RecordParameters& parameters) {
    commands.UpdateBindlessArray(m_impl->bindless);
    GaussianDispatchParams param{};
    const auto size = parameters.output.GetSize();
    param.info = uint4(size.x, size.y, (size.x + 15) / 16, 0);
    param.job = uint4(uint(parameters.debug_view), 0, 0, 0);
    commands.Compute(m_impl->pipeline, param, parameters.input.tex->GetView(), parameters.color->GetView(),
        parameters.transmittance->GetView(), parameters.status->GetView(), parameters.ranges->GetView(),
        parameters.output.tex->GetView())
        .Dispatch(uint3((size.x+15)/16,(size.y+15)/16,1), "Gaussian composite");
    commands.AddCallback([parameters]() {});
}
RenderGraph::PassHandle GaussianCompositePass::AddToGraph(RenderGraph& graph, RecordParameters parameters,
    RenderGraph::TextureHandle input, RenderGraph::TextureHandle color, RenderGraph::TextureHandle transmittance,
    RenderGraph::BufferHandle status, RenderGraph::BufferHandle ranges, RenderGraph::TextureHandle output) {
    return graph.AddRecordPass("GaussianComposite", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(input).Read(color).Read(transmittance).Read(status).Read(ranges).Write(output);
    }, [this, parameters](CommandList& commands) { Record(commands, parameters); },
        RenderGraph::PassExecutionClass::SerialRecord);
}
}
