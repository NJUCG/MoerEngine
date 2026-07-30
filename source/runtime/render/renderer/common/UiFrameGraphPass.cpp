#include "UiFrameGraphPass.h"

#include "rhi/RHICommand.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Moer::Render {
namespace {

RenderGraph::TextureAspect TextureAspects(const TextureRef& texture) {
    RenderGraph::TextureAspect aspects = RenderGraph::TextureAspect::None;
    const auto                 rhi_aspects = texture->GetAspectFlags();
    if (uint32_t(rhi_aspects & ETextureAspectFlags::COLOR) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Color;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::DEPTH_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Depth;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::STENCIL_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Stencil;
    }
    return aspects;
}

RenderGraph::TextureHandle ImportUiGraphTexture(
    RenderGraph&      graph,
    std::string_view  name,
    const TextureRef& texture
) {
    if (!texture) {
        return {};
    }
    const auto aspects = TextureAspects(texture);
    if (aspects == RenderGraph::TextureAspect::None) {
        return {};
    }
    return graph.ImportTexture(
        name,
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = texture->GetNumMips(),
            .layer_count = texture->GetNumArray(),
            .aspects     = aspects,
        }
    );
}

void AddUniqueTarget(Array<TextureRef>& targets, const TextureRef& texture) {
    if (!texture) {
        return;
    }
    for (const auto& existing : targets) {
        if (existing.Get() == texture.Get()) {
            return;
        }
    }
    targets.emplace_back(texture);
}

Array<TextureRef> CollectPresentationTargets(
    const UiCompositionFrameData& composition,
    const UiDrawFramePacket&      draw_frame,
    const TextureRef&             main_output
) {
    Array<TextureRef> targets{};
    targets.reserve(draw_frame.platform_viewports.size() + 2);
    AddUniqueTarget(targets, main_output);
    if (composition.enabled && composition.separate_window) {
        AddUniqueTarget(targets, composition.window_frame_buffer);
    }
    for (const auto& viewport : draw_frame.platform_viewports) {
        if (!viewport.presentation_metadata_only) {
            AddUniqueTarget(targets, viewport.framebuffer);
        }
    }
    return targets;
}

bool HasTextureUsage(
    const TextureRef&  texture,
    ETextureUsageFlags required
) {
    if (!texture) {
        return false;
    }
    const auto usage_bits = static_cast<uint32_t>(texture->GetUsage());
    const auto required_bits = static_cast<uint32_t>(required);
    return (usage_bits & required_bits) == required_bits;
}

bool IsColorOnlyTexture(const TextureRef& texture) {
    if (!texture) {
        return false;
    }
    const auto aspects = static_cast<uint32_t>(texture->GetAspectFlags());
    return aspects == static_cast<uint32_t>(ETextureAspectFlags::COLOR);
}

bool SupportsUiPresentation(const TextureRef& texture) {
    return IsColorOnlyTexture(texture) &&
           HasTextureUsage(
               texture,
               ETextureUsageFlags::COLOR_ATTACHMENT |
                   ETextureUsageFlags::PRESENTATION_SOURCE |
                   ETextureUsageFlags::TRANSFER_SRC |
                   ETextureUsageFlags::TRANSFER_DST
           );
}

bool SupportsUiSceneSampling(const TextureRef& texture) {
    return IsColorOnlyTexture(texture) &&
           HasTextureUsage(texture, ETextureUsageFlags::SAMPLED);
}

bool CollectValidatedPresentationTargets(
    const UiCompositionFrameData& composition,
    const UiDrawFramePacket&      draw_frame,
    const TextureRef&             main_output,
    Array<TextureRef>&            targets
) {
    targets = CollectPresentationTargets(
        composition, draw_frame, main_output
    );
    return !targets.empty() &&
           std::none_of(
               targets.begin(),
               targets.end(),
               [](const TextureRef& target) {
                   return !SupportsUiPresentation(target);
               }
           );
}

bool CollectValidatedPresentationTargets(
    const UiFrameGraphPass::PreparedFrameRef& frame,
    const TextureRef&                         scene_color,
    const TextureRef&                         main_output,
    Array<TextureRef>&                        targets
) {
    targets.clear();
    if (!frame || !SupportsUiSceneSampling(scene_color) ||
        !SupportsUiPresentation(main_output)) {
        return false;
    }
    return CollectValidatedPresentationTargets(
        frame->GetComposition(),
        frame->GetDrawFrame(),
        main_output,
        targets
    );
}

TextureRef ResolveComposeTarget(
    const UiFrameGraphPass::PreparedFrame& frame,
    const TextureRef&                      main_output
) {
    const auto& composition = frame.GetComposition();
    if (composition.enabled && composition.separate_window &&
        composition.window_frame_buffer) {
        return composition.window_frame_buffer;
    }
    return main_output;
}

} // namespace

UiFrameGraphPass::PreparedFrame::PreparedFrame(
    UiCompositionFrameData _composition,
    UiDrawFramePacket      _draw_frame,
    EUiDrawExecutionThread _execution_thread
) :
    composition(std::move(_composition)),
    draw_frame(std::move(_draw_frame)),
    slot_claim(draw_frame),
    execution_thread(_execution_thread) {}

bool UiFrameGraphPass::PreparedFrame::BeginRecording() const noexcept {
    ERecordingState expected = ERecordingState::Prepared;
    return recording_state.compare_exchange_strong(
        expected,
        ERecordingState::Recording,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool UiFrameGraphPass::PreparedFrame::IsRecording() const noexcept {
    return GetRecordingState() == ERecordingState::Recording;
}

void UiFrameGraphPass::PreparedFrame::FinishRecording() const noexcept {
    ERecordingState expected = ERecordingState::Recording;
    if (!recording_state.compare_exchange_strong(
            expected,
            ERecordingState::Recorded,
            std::memory_order_release,
            std::memory_order_acquire
        )) {
        RejectSource();
    }
}

bool UiFrameGraphPass::PreparedFrame::CommitAcceptedSource() const noexcept {
    const ERecordingState current = GetRecordingState();
    if (current == ERecordingState::SourceAccepted ||
        current == ERecordingState::FrameAccepted) {
        return true;
    }
    if (current != ERecordingState::Recorded ||
        !slot_claim.IsReadyForRecording()) {
        ERecordingState expected = ERecordingState::Recorded;
        recording_state.compare_exchange_strong(
            expected,
            ERecordingState::Failed,
            std::memory_order_release,
            std::memory_order_acquire
        );
        slot_claim.Reject();
        return false;
    }

    ERecordingState expected = ERecordingState::Recorded;
    if (!recording_state.compare_exchange_strong(
            expected,
            ERecordingState::SourceAccepted,
            std::memory_order_release,
            std::memory_order_acquire
        )) {
        return false;
    }
    if (slot_claim.CommitAccepted()) {
        return true;
    }

    expected = ERecordingState::SourceAccepted;
    recording_state.compare_exchange_strong(
        expected,
        ERecordingState::Failed,
        std::memory_order_release,
        std::memory_order_acquire
    );
    return false;
}

bool UiFrameGraphPass::PreparedFrame::CommitAcceptedFrame() const noexcept {
    if (GetRecordingState() == ERecordingState::FrameAccepted) {
        return true;
    }
    ERecordingState expected = ERecordingState::SourceAccepted;
    return recording_state.compare_exchange_strong(
        expected,
        ERecordingState::FrameAccepted,
        std::memory_order_release,
        std::memory_order_acquire
    );
}

void UiFrameGraphPass::PreparedFrame::RejectSource() const noexcept {
    ERecordingState current = GetRecordingState();
    while (current != ERecordingState::SourceAccepted &&
           current != ERecordingState::FrameAccepted &&
           current != ERecordingState::Failed) {
        if (recording_state.compare_exchange_weak(
                current,
                ERecordingState::Failed,
                std::memory_order_release,
                std::memory_order_acquire
            )) {
            slot_claim.Reject();
            return;
        }
    }
    if (current == ERecordingState::Failed) {
        slot_claim.Reject();
    }
}

void UiFrameGraphPass::PreparedFrame::Abandon() const noexcept {
    RejectSource();
}

UiFrameGraphPass::PreparedFrameRef UiFrameGraphPass::Prepare(
    UiCompositionFrameData composition,
    UiDrawFramePacket      draw_frame,
    EUiDrawExecutionThread execution_thread
) {
    return MakeShared<PreparedFrame>(
        std::move(composition),
        std::move(draw_frame),
        execution_thread
    );
}

bool UiFrameGraphPass::AddPasses(
    RenderGraph&               graph,
    UiCombinePass&             combine_pass,
    const PreparedFrameRef&    frame,
    RenderGraph::TextureHandle scene_color_handle,
    const TextureRef&          scene_color,
    const TextureRef&          main_output,
    RenderGraph::TokenHandle   presentation_ready,
    GraphPasses&               passes
) {
    return AddPassesWithComposeRecorder(
        graph,
        [&combine_pass, frame, scene_color, main_output](
            CommandList& cmd_list
        ) {
            RecordCompose(
                cmd_list,
                combine_pass,
                *frame,
                scene_color,
                main_output
            );
        },
        frame,
        scene_color_handle,
        scene_color,
        main_output,
        presentation_ready,
        passes
    );
}

bool UiFrameGraphPass::AddPassesWithComposeRecorder(
    RenderGraph&               graph,
    ComposeRecorder            compose_recorder,
    const PreparedFrameRef&    frame,
    RenderGraph::TextureHandle scene_color_handle,
    const TextureRef&          scene_color,
    const TextureRef&          main_output,
    RenderGraph::TokenHandle   presentation_ready,
    GraphPasses&               passes
) {
    passes = {};
    const TextureRef graph_scene_color =
        graph.GetPhysicalTexture(scene_color_handle);
    if (!compose_recorder || !graph_scene_color ||
        graph_scene_color.Get() != scene_color.Get() ||
        !scene_color_handle.IsValid() || !presentation_ready.IsValid()) {
        return false;
    }

    Array<TextureRef> targets{};
    if (!CollectValidatedPresentationTargets(
            frame,
            scene_color,
            main_output,
            targets
        )) {
        return false;
    }

    Array<RenderGraph::TextureHandle> target_handles{};
    target_handles.reserve(targets.size());
    RenderGraph::TextureHandle main_output_handle{};
    RenderGraph::TextureHandle compose_target_handle{};
    const TextureRef compose_target = ResolveComposeTarget(*frame, main_output);
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto handle = ImportUiGraphTexture(
            graph,
            std::format("UI.presentation_target_{}", index),
            targets[index]
        );
        if (!handle.IsValid()) {
            return false;
        }
        graph.SetInitialState(
            handle,
            RenderGraph::TextureState::Undefined,
            RenderGraph::QueueRole::None,
            RenderGraph::AccessMode::None
        );
        if (targets[index].Get() == main_output.Get()) {
            main_output_handle = handle;
        }
        if (targets[index].Get() == compose_target.Get()) {
            compose_target_handle = handle;
        }
        target_handles.emplace_back(handle);
    }
    if (!main_output_handle.IsValid() || !compose_target_handle.IsValid()) {
        return false;
    }

    passes.clear = graph.AddRecordPass(
        "UI.ClearTargets",
        [presentation_ready, target_handles](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Copy
                )
                .Read(presentation_ready)
                .SideEffect()
                .SerialRecord();
            for (const auto target : target_handles) {
                builder.Write(
                    target,
                    RenderGraph::TextureState::TransferDestination
                );
            }
        },
        [frame, targets](CommandList& cmd_list) {
            if (!frame->BeginRecording()) {
                throw std::logic_error(
                    "copied UI frame packet recording was already claimed"
                );
            }
            ScopedGpuMarker marker(
                cmd_list,
                "UI Clear Targets",
                GpuMarkerPalette::Ui()
            );
            RecordClear(cmd_list, targets);
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    passes.compose = graph.AddRecordPass(
        "UI.Compose",
        [scene_color_handle, compose_target_handle, clear = passes.clear](
            RenderGraph::PassBuilder& builder
        ) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Graphics
                )
                .DependsOn(clear)
                .Read(
                    scene_color_handle,
                    RenderGraph::TextureState::Sampled
                )
                .Write(
                    compose_target_handle,
                    RenderGraph::TextureState::RenderTarget
                )
                .SideEffect()
                .SerialRecord();
        },
        [frame, compose_recorder = std::move(compose_recorder)](
            CommandList& cmd_list
        ) {
            if (!frame->IsRecording()) {
                throw std::logic_error(
                    "copied UI frame packet compose executed outside its recording claim"
                );
            }
            compose_recorder(cmd_list);
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    passes.draw = graph.AddRecordPass(
        "UI.DrawCopiedFrame",
        [target_handles, compose = passes.compose](
            RenderGraph::PassBuilder& builder
        ) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Graphics
                )
                .DependsOn(compose)
                .SideEffect()
                .SerialRecord()
                .TranslateSerialControl();
            for (const auto target : target_handles) {
                builder.ReadWrite(
                    target,
                    RenderGraph::TextureState::RenderTarget
                );
            }
        },
        [frame, main_output](CommandList& cmd_list) {
            if (!frame->IsRecording()) {
                throw std::logic_error(
                    "copied UI frame packet draw executed outside its recording claim"
                );
            }
            RecordDraw(cmd_list, *frame, main_output);
            frame->FinishRecording();
            if (frame->GetRecordingState() != ERecordingState::Recorded) {
                throw std::logic_error(
                    "copied UI frame packet failed to finalize recording"
                );
            }
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );

    for (const auto target : target_handles) {
        graph.Export(
            target,
            RenderGraph::TextureState::PresentationSource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    passes.main_output          = main_output_handle;
    passes.presentation_targets = std::move(target_handles);
    return passes.IsValid();
}

bool UiFrameGraphPass::ProcessLinear(
    CommandList&            cmd_list,
    UiCombinePass&          combine_pass,
    const PreparedFrameRef& frame,
    const TextureRef&       scene_color,
    const TextureRef&       main_output
) {
    return ProcessLinearWithComposeRecorder(
        cmd_list,
        [&combine_pass, frame, scene_color, main_output](
            CommandList& recording_list
        ) {
            RecordCompose(
                recording_list,
                combine_pass,
                *frame,
                scene_color,
                main_output
            );
        },
        frame,
        scene_color,
        main_output
    );
}

bool UiFrameGraphPass::ProcessLinearWithComposeRecorder(
    CommandList&            cmd_list,
    ComposeRecorder         compose_recorder,
    const PreparedFrameRef& frame,
    const TextureRef&       scene_color,
    const TextureRef&       main_output
) {
    Array<TextureRef> targets{};
    if (!compose_recorder ||
        !CollectValidatedPresentationTargets(
            frame,
            scene_color,
            main_output,
            targets
        ) ||
        !frame->BeginRecording()) {
        return false;
    }

    try {
        RecordClear(cmd_list, targets);
        compose_recorder(cmd_list);
        RecordDraw(cmd_list, *frame, main_output);

        if (!RecordPresentationBoundary(
                cmd_list,
                frame->GetComposition(),
                frame->GetDrawFrame(),
                main_output
            )) {
            frame->Abandon();
            return false;
        }

        frame->FinishRecording();
        return frame->GetRecordingState() == ERecordingState::Recorded;
    } catch (...) {
        frame->Abandon();
        throw;
    }
}

bool UiFrameGraphPass::RecordPresentationBoundary(
    CommandList&                  cmd_list,
    const UiCompositionFrameData& composition,
    const UiDrawFramePacket&      draw_frame,
    const TextureRef&             main_output
) {
    Array<TextureRef> targets{};
    if (!CollectValidatedPresentationTargets(
            composition, draw_frame, main_output, targets
        )) {
        return false;
    }

    // Linear warm-up/fallback and Raster frame-tail packets remain
    // BackendTracked. Declare their presentation boundary with a terminal
    // legacy read instead of switching a mixed command stream to Explicit
    // ownership. Vulkan restores these targets to their semantic preferred
    // GENERAL layout and publishes readiness only after native acceptance.
    Array<ReadTexture> presentation_sources{};
    presentation_sources.reserve(targets.size());
    for (const auto& target : targets) {
        presentation_sources.emplace_back(ReadTexture{
            target->GetView(
                0,
                static_cast<uint8>(target->GetNumMips())
            ),
            ETextureState::TRANSFER,
            true,
        });
    }
    cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Copy,
        std::move(presentation_sources),
        {}
    );
    return true;
}

void UiFrameGraphPass::RecordClear(
    CommandList&             cmd_list,
    const Array<TextureRef>& targets
) {
    for (const auto& target : targets) {
        if (target) {
            cmd_list.ClearResource(
                target->GetView(),
                float4(0.f, 0.f, 0.f, 1.f)
            );
        }
    }
}

void UiFrameGraphPass::RecordCompose(
    CommandList&         cmd_list,
    UiCombinePass&       combine_pass,
    const PreparedFrame& frame,
    const TextureRef&    scene_color,
    const TextureRef&    main_output
) {
    const auto& composition = frame.GetComposition();
    if (composition.enabled) {
        const TextureView window_frame_buffer =
            composition.window_frame_buffer ?
                composition.window_frame_buffer->GetView() :
                TextureView{};
        combine_pass.Process(
            cmd_list,
            composition.separate_window,
            composition.output_resolution,
            composition.scene_color_position,
            composition.scene_color_resolution,
            window_frame_buffer,
            scene_color->GetView(),
            main_output->GetView()
        );
        return;
    }

    const auto extent = main_output->GetExtent();
    combine_pass.Process(
        cmd_list,
        true,
        uint2(extent.x, extent.y),
        float2(0.f, 0.f),
        float2(static_cast<float>(extent.x), static_cast<float>(extent.y)),
        main_output->GetView(),
        scene_color->GetView(),
        main_output->GetView()
    );
}

void UiFrameGraphPass::RecordDraw(
    CommandList&         cmd_list,
    const PreparedFrame& frame,
    const TextureRef&    main_output
) {
    RenderUiDrawFrame(
        cmd_list,
        main_output->GetView(),
        frame.GetDrawFrame(),
        frame.GetExecutionThread()
    );
}

} // namespace Moer::Render
