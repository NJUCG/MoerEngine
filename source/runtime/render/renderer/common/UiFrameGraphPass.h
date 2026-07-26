#pragma once

#include "UIRenderer.h"
#include "UiCombinePass.h"
#include "rendergraph/RenderGraph.h"

#include <atomic>
#include <functional>

namespace Moer::Render {

class UiFrameGraphPassTestAccess;

/**
 * Owns the copied editor draw packet across RenderGraph recording and the
 * Executor-owned Present epilogue. The packet is immutable after capture; the
 * one-shot state protects the mutable ImGui GPU upload-ring backend.
 */
class RENDER_API UiFrameGraphPass {
public:
    enum class ERecordingState : uint8_t {
        Prepared,
        Recording,
        Recorded,
        SourceAccepted,
        FrameAccepted,
        Failed,
    };

    class RENDER_API PreparedFrame {
    public:
        PreparedFrame(
            UiCompositionFrameData composition,
            UiDrawFramePacket      draw_frame,
            EUiDrawExecutionThread execution_thread
        );

        PreparedFrame(const PreparedFrame&)            = delete;
        PreparedFrame& operator=(const PreparedFrame&) = delete;

        [[nodiscard]] const UiCompositionFrameData& GetComposition() const {
            return composition;
        }
        [[nodiscard]] const UiDrawFramePacket& GetDrawFrame() const {
            return draw_frame;
        }
        [[nodiscard]] EUiDrawExecutionThread GetExecutionThread() const {
            return execution_thread;
        }
        [[nodiscard]] ERecordingState GetRecordingState() const noexcept {
            return recording_state.load(std::memory_order_acquire);
        }
        [[nodiscard]] bool CanPresent() const noexcept {
            return GetRecordingState() == ERecordingState::FrameAccepted;
        }
        [[nodiscard]] bool CommitAcceptedSource() const noexcept;
        [[nodiscard]] bool CommitAcceptedFrame() const noexcept;
        void RejectSource() const noexcept;

        /**
         * Makes any graph recording failure terminal for this packet. A graph
         * may have entered Clear/Compose before another source fails, so the
         * renderer must never replay the packet on a linear CommandList.
         */
        void Abandon() const noexcept;

    private:
        [[nodiscard]] bool BeginRecording() const noexcept;
        [[nodiscard]] bool IsRecording() const noexcept;
        void FinishRecording() const noexcept;
        [[nodiscard]] bool ValidatePendingSlots() const noexcept;
        void CommitPendingSlots() const noexcept;

        UiCompositionFrameData composition{};
        UiDrawFramePacket      draw_frame{};
        EUiDrawExecutionThread execution_thread{EUiDrawExecutionThread::Game};
        mutable std::atomic<ERecordingState> recording_state{ERecordingState::Prepared};

        friend class UiFrameGraphPass;
        friend class UiFrameGraphPassTestAccess;
    };

    using PreparedFrameRef = SharedPtr<PreparedFrame>;

    struct GraphPasses {
        RenderGraph::PassHandle    clear{};
        RenderGraph::PassHandle    compose{};
        RenderGraph::PassHandle    draw{};
        RenderGraph::TextureHandle main_output{};
        Array<RenderGraph::TextureHandle> presentation_targets{};

        [[nodiscard]] bool IsValid() const {
            return clear.IsValid() && compose.IsValid() && draw.IsValid() &&
                   main_output.IsValid() && !presentation_targets.empty();
        }
    };

    [[nodiscard]] static PreparedFrameRef Prepare(
        UiCompositionFrameData composition,
        UiDrawFramePacket      draw_frame,
        EUiDrawExecutionThread execution_thread
    );

    [[nodiscard]] static bool AddPasses(
        RenderGraph&                     graph,
        UiCombinePass&                   combine_pass,
        const PreparedFrameRef&          frame,
        RenderGraph::TextureHandle       scene_color_handle,
        const TextureRef&                scene_color,
        const TextureRef&                main_output,
        RenderGraph::TokenHandle         presentation_ready,
        GraphPasses&                     passes
    );

    [[nodiscard]] static bool ProcessLinear(
        CommandList&            cmd_list,
        UiCombinePass&          combine_pass,
        const PreparedFrameRef& frame,
        const TextureRef&       scene_color,
        const TextureRef&       main_output
    );

private:
    /**
     * Trusted declaration seam shared by the production compositor and the
     * graph-contract test. The callback may only record the UI.Compose body;
     * all resource declarations remain owned here.
     */
    using ComposeRecorder = std::function<void(CommandList&)>;
    [[nodiscard]] static bool AddPassesWithComposeRecorder(
        RenderGraph&               graph,
        ComposeRecorder            compose_recorder,
        const PreparedFrameRef&    frame,
        RenderGraph::TextureHandle scene_color_handle,
        const TextureRef&          scene_color,
        const TextureRef&          main_output,
        RenderGraph::TokenHandle   presentation_ready,
        GraphPasses&               passes
    );
    [[nodiscard]] static bool ProcessLinearWithComposeRecorder(
        CommandList&            cmd_list,
        ComposeRecorder         compose_recorder,
        const PreparedFrameRef& frame,
        const TextureRef&       scene_color,
        const TextureRef&       main_output
    );

    static void RecordClear(
        CommandList&             cmd_list,
        const Array<TextureRef>& targets
    );
    static void RecordCompose(
        CommandList&            cmd_list,
        UiCombinePass&          combine_pass,
        const PreparedFrame&    frame,
        const TextureRef&       scene_color,
        const TextureRef&       main_output
    );
    static void RecordDraw(
        CommandList&         cmd_list,
        const PreparedFrame& frame,
        const TextureRef&    main_output
    );

    friend class UiFrameGraphPassTestAccess;
};

} // namespace Moer::Render
