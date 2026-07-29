#include "profile_capture_ui/ProfileCaptureUI.h"

#include "Engine.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"

#include <array>
#include <exception>
#include <system_error>
#include <utility>

#include <imgui.h>
#include <nfd.hpp>

namespace Moer {
namespace {

const char* ControllerStateText(
    Render::ProfileCaptureControllerState state
) noexcept {
    switch (state) {
        case Render::ProfileCaptureControllerState::Idle:
            return "Idle";
        case Render::ProfileCaptureControllerState::Starting:
            return "Starting";
        case Render::ProfileCaptureControllerState::Running:
            return "Running";
        case Render::ProfileCaptureControllerState::StoppingGpu:
            return "Stopping GPU";
        case Render::ProfileCaptureControllerState::FinalizingRuntime:
            return "Finalizing Runtime";
        case Render::ProfileCaptureControllerState::AwaitingRhiDrain:
            return "Awaiting RHI Drain";
        case Render::ProfileCaptureControllerState::AwaitingWorkerShutdown:
            return "Awaiting Worker Shutdown";
        case Render::ProfileCaptureControllerState::Shutdown:
            return "Shutdown";
    }
    return "Unknown";
}

const char* CompletionStatusText(
    Render::ProfileCaptureCompletionStatus status
) noexcept {
    switch (status) {
        case Render::ProfileCaptureCompletionStatus::Pending:
            return "Pending";
        case Render::ProfileCaptureCompletionStatus::Started:
            return "Started";
        case Render::ProfileCaptureCompletionStatus::StartedCpuOnly:
            return "Started (CPU only)";
        case Render::ProfileCaptureCompletionStatus::Stopped:
            return "Stopped";
        case Render::ProfileCaptureCompletionStatus::StoppedWithLoss:
            return "Stopped with loss";
        case Render::ProfileCaptureCompletionStatus::RejectedAdmissionClosed:
            return "Rejected: admission closed";
        case Render::ProfileCaptureCompletionStatus::RejectedQueueFull:
            return "Rejected: queue full";
        case Render::ProfileCaptureCompletionStatus::RejectedResourceExhausted:
            return "Rejected: resource exhausted";
        case Render::ProfileCaptureCompletionStatus::RejectedInvalidArgument:
            return "Rejected: invalid argument";
        case Render::ProfileCaptureCompletionStatus::RejectedAlreadyRunning:
            return "Rejected: already running";
        case Render::ProfileCaptureCompletionStatus::RejectedStaleGeneration:
            return "Rejected: stale generation";
        case Render::ProfileCaptureCompletionStatus::RejectedExternalRuntime:
            return "Rejected: external runtime";
        case Render::ProfileCaptureCompletionStatus::CancelledForShutdown:
            return "Cancelled for shutdown";
        case Render::ProfileCaptureCompletionStatus::RuntimeStartFailed:
            return "Runtime start failed";
        case Render::ProfileCaptureCompletionStatus::CpuSchemaFailed:
            return "CPU schema failed";
        case Render::ProfileCaptureCompletionStatus::GpuFrameSchemaFailed:
            return "GPU frame schema failed";
        case Render::ProfileCaptureCompletionStatus::GpuScopeSchemaFailed:
            return "GPU scope schema failed";
        case Render::ProfileCaptureCompletionStatus::CpuActivationFailed:
            return "CPU producer activation failed";
        case Render::ProfileCaptureCompletionStatus::GpuSessionStartFailed:
            return "GPU session start failed";
        case Render::ProfileCaptureCompletionStatus::RuntimeOwnershipLost:
            return "Runtime ownership lost";
        case Render::ProfileCaptureCompletionStatus::GpuSessionFaulted:
            return "GPU session faulted";
        case Render::ProfileCaptureCompletionStatus::GpuSessionAborted:
            return "GPU session aborted";
        case Render::ProfileCaptureCompletionStatus::RuntimeShutdownFaulted:
            return "Runtime shutdown faulted";
        case Render::ProfileCaptureCompletionStatus::ControllerDestroyed:
            return "Controller destroyed";
    }
    return "Unknown";
}

const char* SubmitResultText(Render::ProfileCaptureSubmitResult result) noexcept {
    switch (result) {
        case Render::ProfileCaptureSubmitResult::Queued:
            return "Queued";
        case Render::ProfileCaptureSubmitResult::AdmissionClosed:
            return "Admission closed";
        case Render::ProfileCaptureSubmitResult::QueueFull:
            return "Queue full";
        case Render::ProfileCaptureSubmitResult::InvalidArgument:
            return "Invalid argument";
        case Render::ProfileCaptureSubmitResult::ResourceExhausted:
            return "Resource exhausted";
    }
    return "Unknown";
}

const char* ActionResultText(EProfileCaptureControlActionResult result) noexcept {
    switch (result) {
        case EProfileCaptureControlActionResult::None:
            return "No request submitted";
        case EProfileCaptureControlActionResult::Submitted:
            return "Request queued";
        case EProfileCaptureControlActionResult::Completed:
            return "Request reached a terminal completion";
        case EProfileCaptureControlActionResult::PendingRequest:
            return "A UI request is already pending";
        case EProfileCaptureControlActionResult::InvalidOutputPath:
            return "Choose a non-empty output path";
        case EProfileCaptureControlActionResult::InvalidControllerState:
            return "Action is not valid in the current controller state";
        case EProfileCaptureControlActionResult::CallbackUnavailable:
            return "Engine callback is unavailable";
        case EProfileCaptureControlActionResult::CallbackFailed:
            return "Engine callback failed";
        case EProfileCaptureControlActionResult::BackendRejected:
            return "Controller rejected the request";
        case EProfileCaptureControlActionResult::InvalidSubmission:
            return "Controller returned a malformed submission or ticket";
    }
    return "Unknown";
}

bool SelectCaptureOutput(
    std::filesystem::path& output_path,
    std::string&           status
) {
    NFD::UniquePath selected_path = nullptr;
    const std::array<nfdfilteritem_t, 1> filters = {{
        {"Moer Profile Dump", "mpd"},
    }};

    const nfdresult_t result = NFD::SaveDialog(
        selected_path,
        filters.data(),
        filters.size(),
        nullptr,
        "MoerProfile.mpd"
    );
    if (result == NFD_CANCEL) {
        status = "Output selection cancelled.";
        return false;
    }
    if (result != NFD_OKAY || !selected_path) {
        const char* error_message = NFD_GetError();
        status = error_message ? error_message : "Native file dialog failed.";
        LOG_ERROR("[ProfileCaptureUI] NFD error: {}", status);
        return false;
    }

    try {
        output_path = std::filesystem::u8path(selected_path.get());
        if (output_path.extension() != ".mpd") {
            output_path.replace_extension(".mpd");
        }
        status = "Output path selected.";
        return true;
    } catch (const std::exception& error) {
        status = error.what();
        LOG_ERROR(
            "[ProfileCaptureUI] Invalid output path selected: {}",
            error.what()
        );
        return false;
    }
}

void DrawSnapshotTable(
    const Render::ProfileCaptureControllerSnapshot& snapshot
) {
    if (!ImGui::BeginTable(
            "ProfileCaptureControllerState",
            2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
        )) {
        return;
    }

    const auto row = [](const char* name, auto value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%llu", static_cast<unsigned long long>(value));
    };
    const auto bool_row = [](const char* name, bool value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value ? "yes" : "no");
    };

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("State");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(ControllerStateText(snapshot.state));

    row("Generation", snapshot.active_generation);
    bool_row("Owns runtime", snapshot.owns_runtime);
    bool_row("CPU producer", snapshot.cpu_producer_active);
    bool_row("GPU session", snapshot.gpu_session_active);
    bool_row("Accepting requests", snapshot.accepting_requests);
    row("Queued requests", snapshot.queued_requests);
    row("Queue capacity", snapshot.queue_capacity);
    row("Submitted", snapshot.requests_submitted);
    row("Accepted", snapshot.requests_accepted);
    row("Rejected", snapshot.requests_rejected);
    row("Completed", snapshot.requests_completed);
    row("Wrong-thread owner ticks", snapshot.wrong_thread_calls);
    row("Last completed request", snapshot.last_completed_request_id);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Last global completion");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(
        CompletionStatusText(snapshot.last_completion_status)
    );

    ImGui::EndTable();
}

} // namespace

ProfileCaptureUI::ProfileCaptureUI(Engine& engine) :
    control_(
        ProfileCaptureControlCallbacks{
            .request_start =
                [&engine](Render::ProfileCaptureStartOptions options) {
                    return engine.RequestProfileCaptureStart(
                        std::move(options)
                    );
                },
            .request_stop =
                [&engine](std::uint64_t generation) {
                    return engine.RequestProfileCaptureStop(generation);
                },
            .get_snapshot =
                [&engine]() {
                    return engine.GetProfileCaptureSnapshot();
                },
        }
    ) {
    try {
        const ConfigManager& config_manager =
            ConfigManager::GetInstance();
        const auto& profile_config =
            config_manager.GetConfig().engine.profile_dump;

        output_path_ = profile_config.output_path.empty() ?
            std::filesystem::path("profile/MoerProfile.mpd") :
            std::filesystem::path(profile_config.output_path);
        if (output_path_.is_relative()) {
            output_path_ =
                config_manager.GetWorkspacePath() / output_path_;
        }

        std::error_code path_error;
        const std::filesystem::path canonical_output =
            std::filesystem::weakly_canonical(output_path_, path_error);
        if (!path_error) {
            output_path_ = canonical_output;
        } else {
            output_path_ = output_path_.lexically_normal();
        }
    } catch (...) {
        output_path_.clear();
    }
}

void ProfileCaptureUI::ShowWindow(bool* open) {
    control_.Refresh();

    if (!ImGui::Begin("Profile Capture", open)) {
        ImGui::End();
        return;
    }

    const Render::ProfileCaptureControllerSnapshot& snapshot =
        control_.Snapshot();
    DrawSnapshotTable(snapshot);

    ImGui::Spacing();
    ImGui::SeparatorText("Capture Request");

    const std::string output_text = output_path_.generic_string();
    ImGui::TextWrapped(
        "Next output: %s",
        output_text.empty() ? "<not selected>" : output_text.c_str()
    );
    if (ImGui::Button("Choose Output...")) {
        static_cast<void>(
            SelectCaptureOutput(output_path_, dialog_status_)
        );
    }
    if (!dialog_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dialog_status_.c_str());
    }
    ImGui::Checkbox("Replace existing file", &replace_existing_);

    std::error_code output_exists_error;
    const bool output_exists =
        !output_path_.empty() &&
        std::filesystem::exists(output_path_, output_exists_error);
    if (!output_exists_error && output_exists && !replace_existing_) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "Output already exists. Choose another path or explicitly enable replacement."
        );
    }

    const bool start_enabled =
        control_.CanRequestStart() && !output_path_.empty() &&
        !output_exists_error &&
        (!output_exists || replace_existing_);
    ImGui::BeginDisabled(!start_enabled);
    if (ImGui::Button("Start Capture")) {
        Render::ProfileCaptureStartOptions options;
        options.runtime.output_path      = output_path_;
        options.runtime.replace_existing = replace_existing_;
        static_cast<void>(control_.RequestStart(std::move(options)));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool stop_enabled = control_.CanRequestStop();
    ImGui::BeginDisabled(!stop_enabled);
    if (ImGui::Button("Stop Capture")) {
        static_cast<void>(control_.RequestStop());
    }
    ImGui::EndDisabled();

    if (control_.HasPendingRequest()) {
        const bool is_start =
            control_.PendingRequestKind() ==
            Render::ProfileCaptureRequestKind::Start;
        ImGui::TextColored(
            ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "%s request pending; Engine will process it at the next owner tick.",
            is_start ? "Start" : "Stop"
        );
        if (!is_start) {
            ImGui::Text(
                "Protected generation requested by Stop: %llu",
                static_cast<unsigned long long>(
                    control_.PendingExpectedGeneration()
                )
            );
        }
    }

    ImGui::Text(
        "Last UI action: %s",
        ActionResultText(control_.LastActionResult())
    );
    if (control_.LastActionResult() ==
        EProfileCaptureControlActionResult::BackendRejected) {
        ImGui::Text(
            "Submit result: %s",
            SubmitResultText(control_.LastSubmitResult())
        );
    }

    if (const auto& completion = control_.LastCompletion();
        completion.has_value()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Last UI Completion");
        ImGui::Text(
            "Request %llu (%s), generation %llu",
            static_cast<unsigned long long>(completion->request_id),
            completion->kind == Render::ProfileCaptureRequestKind::Start ?
                "Start" :
                "Stop",
            static_cast<unsigned long long>(completion->generation)
        );
        ImGui::Text(
            "Status: %s",
            CompletionStatusText(completion->status)
        );
        ImGui::Text(
            "Subsystem detail: %u / %u",
            completion->detail,
            completion->secondary_detail
        );
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "The panel submits intent only. Engine owns lifecycle transitions, "
        "GPU tail drain, ProfileDump shutdown, and generation checks."
    );

    ImGui::End();
}

} // namespace Moer
