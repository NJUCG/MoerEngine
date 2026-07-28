#include "profile_consumer/ProfileSession.h"
#include "profile_consumer/ProfileSummaryContract.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace Moer::ProfileDump;
using Json = nlohmann::ordered_json;

[[nodiscard]] const char* StatusName(SessionLoadStatus _status) noexcept {
    switch (_status) {
        case SessionLoadStatus::Reading:
            return "reading";
        case SessionLoadStatus::Complete:
            return "complete";
        case SessionLoadStatus::ForensicTruncated:
            return "forensic_truncated";
        case SessionLoadStatus::InvalidArgument:
            return "invalid_argument";
        case SessionLoadStatus::OpenFailed:
            return "open_failed";
        case SessionLoadStatus::ReadFailed:
            return "read_failed";
        case SessionLoadStatus::UnsupportedVersion:
            return "unsupported_version";
        case SessionLoadStatus::CorruptData:
            return "corrupt_data";
        case SessionLoadStatus::ProtocolViolation:
            return "protocol_violation";
        case SessionLoadStatus::LimitExceeded:
            return "limit_exceeded";
        case SessionLoadStatus::ResourceExhausted:
            return "resource_exhausted";
    }
    return "unknown";
}

[[nodiscard]] const char* IncompleteReasonName(SessionIncompleteReason _reason) noexcept {
    switch (_reason) {
        case SessionIncompleteReason::None:
            return "none";
        case SessionIncompleteReason::MissingSessionEnd:
            return "missing_session_end";
        case SessionIncompleteReason::TruncatedHeader:
            return "truncated_header";
        case SessionIncompleteReason::TruncatedPayload:
            return "truncated_payload";
    }
    return "unknown";
}

[[nodiscard]] const char* ErrorCodeName(SessionErrorCode _code) noexcept {
    switch (_code) {
        case SessionErrorCode::None:
            return "none";
        case SessionErrorCode::InvalidArgument:
            return "invalid_argument";
        case SessionErrorCode::FileOpenFailed:
            return "file_open_failed";
        case SessionErrorCode::FileReadFailed:
            return "file_read_failed";
        case SessionErrorCode::ResourceAllocationFailed:
            return "resource_allocation_failed";
        case SessionErrorCode::UnexpectedFailure:
            return "unexpected_failure";
        case SessionErrorCode::LimitExceeded:
            return "limit_exceeded";
        case SessionErrorCode::CodecHeaderInvalid:
            return "codec_header_invalid";
        case SessionErrorCode::CodecPacketInvalid:
            return "codec_packet_invalid";
        case SessionErrorCode::SessionBeginMissing:
            return "session_begin_missing";
        case SessionErrorCode::SessionBeginDuplicate:
            return "session_begin_duplicate";
        case SessionErrorCode::SessionEndMissing:
            return "session_end_missing";
        case SessionErrorCode::SessionEndTotalsMismatch:
            return "session_end_totals_mismatch";
        case SessionErrorCode::PacketIndexMismatch:
            return "packet_index_mismatch";
        case SessionErrorCode::TrailingDataAfterSessionEnd:
            return "trailing_data_after_session_end";
        case SessionErrorCode::TruncatedPacket:
            return "truncated_packet";
        case SessionErrorCode::SchemaDecodeInvalid:
            return "schema_decode_invalid";
        case SessionErrorCode::SchemaConflict:
            return "schema_conflict";
        case SessionErrorCode::RecordPayloadInvalid:
            return "record_payload_invalid";
        case SessionErrorCode::RecordSchemaUnregistered:
            return "record_schema_unregistered";
        case SessionErrorCode::RecordSequenceDuplicate:
            return "record_sequence_duplicate";
        case SessionErrorCode::RecordSequenceInvalid:
            return "record_sequence_invalid";
        case SessionErrorCode::LossPayloadInvalid:
            return "loss_payload_invalid";
        case SessionErrorCode::LossTotalsOverflow:
            return "loss_totals_overflow";
        case SessionErrorCode::CpuScopePayloadInvalid:
            return "cpu_scope_payload_invalid";
        case SessionErrorCode::CpuScopeTimeInvalid:
            return "cpu_scope_time_invalid";
        case SessionErrorCode::CpuScopeParentMissing:
            return "cpu_scope_parent_missing";
        case SessionErrorCode::GpuFramePayloadInvalid:
            return "gpu_frame_payload_invalid";
        case SessionErrorCode::GpuFrameStatusInvalid:
            return "gpu_frame_status_invalid";
        case SessionErrorCode::GpuFrameDuplicate:
            return "gpu_frame_duplicate";
        case SessionErrorCode::GpuFrameTotalsMismatch:
            return "gpu_frame_totals_mismatch";
        case SessionErrorCode::GpuScopePayloadInvalid:
            return "gpu_scope_payload_invalid";
        case SessionErrorCode::GpuScopeIdentityInvalid:
            return "gpu_scope_identity_invalid";
        case SessionErrorCode::GpuScopeQueueInvalid:
            return "gpu_scope_queue_invalid";
        case SessionErrorCode::GpuScopeStatusInvalid:
            return "gpu_scope_status_invalid";
        case SessionErrorCode::GpuScopeTimingInvalid:
            return "gpu_scope_timing_invalid";
        case SessionErrorCode::GpuDomainConflict:
            return "gpu_domain_conflict";
        case SessionErrorCode::GpuScopeDuplicate:
            return "gpu_scope_duplicate";
        case SessionErrorCode::GpuScopeFrameMissing:
            return "gpu_scope_frame_missing";
        case SessionErrorCode::GpuScopeRootDepthInvalid:
            return "gpu_scope_root_depth_invalid";
        case SessionErrorCode::GpuScopeParentMissing:
            return "gpu_scope_parent_missing";
        case SessionErrorCode::GpuScopeParentInvalid:
            return "gpu_scope_parent_invalid";
        case SessionErrorCode::CpuScopeTopologyInvalid:
            return "cpu_scope_topology_invalid";
    }
    return "unknown";
}

[[nodiscard]] const char* CodecStatusName(DecodeStatus _status) noexcept {
    switch (_status) {
        case DecodeStatus::Ok:
            return "ok";
        case DecodeStatus::NeedMoreData:
            return "need_more_data";
        case DecodeStatus::InvalidMagic:
            return "invalid_magic";
        case DecodeStatus::UnsupportedVersion:
            return "unsupported_version";
        case DecodeStatus::InvalidHeader:
            return "invalid_header";
        case DecodeStatus::UnknownPacketType:
            return "unknown_packet_type";
        case DecodeStatus::UnsupportedFlags:
            return "unsupported_flags";
        case DecodeStatus::PayloadTooLarge:
            return "payload_too_large";
        case DecodeStatus::ChecksumMismatch:
            return "checksum_mismatch";
        case DecodeStatus::MalformedPayload:
            return "malformed_payload";
        case DecodeStatus::LimitExceeded:
            return "limit_exceeded";
        case DecodeStatus::SchemaHashMismatch:
            return "schema_hash_mismatch";
        case DecodeStatus::UnknownSchema:
            return "unknown_schema";
    }
    return "unknown";
}

[[nodiscard]] const char* LimitName(SessionLimitKind _limit) noexcept {
    switch (_limit) {
        case SessionLimitKind::None:
            return "none";
        case SessionLimitKind::Codec:
            return "codec";
        case SessionLimitKind::InputBytes:
            return "input_bytes";
        case SessionLimitKind::Packets:
            return "packets";
        case SessionLimitKind::Schemas:
            return "schemas";
        case SessionLimitKind::SchemaBytes:
            return "schema_bytes";
        case SessionLimitKind::Records:
            return "records";
        case SessionLimitKind::LossNotices:
            return "loss_notices";
        case SessionLimitKind::CpuScopes:
            return "cpu_scopes";
        case SessionLimitKind::GpuFrames:
            return "gpu_frames";
        case SessionLimitKind::GpuScopes:
            return "gpu_scopes";
        case SessionLimitKind::CpuTracks:
            return "cpu_tracks";
        case SessionLimitKind::GpuTracks:
            return "gpu_tracks";
        case SessionLimitKind::GpuDomains:
            return "gpu_domains";
        case SessionLimitKind::UniqueStrings:
            return "unique_strings";
        case SessionLimitKind::StringBytes:
            return "string_bytes";
        case SessionLimitKind::LogicalModelBytes:
            return "logical_model_bytes";
        case SessionLimitKind::ScopeDepth:
            return "scope_depth";
        case SessionLimitKind::TopologyWorkItems:
            return "topology_work_items";
    }
    return "unknown";
}

[[nodiscard]] std::string U64(std::uint64_t _value) {
    return std::to_string(_value);
}

[[nodiscard]] Json OptionalU64(std::uint64_t _value) {
    return _value == kInvalidSessionIndex ? Json(nullptr) : Json(U64(_value));
}

[[nodiscard]] Json BuildJson(const SessionLoadResult& _result, const ProfileSession& _session) {
    Json document{
        {"contract", kProfileSummaryContractName},
        {"version", kProfileSummaryContractVersion},
        {
            "load",
            {
                {"status", StatusName(_result.status)},
                {"incomplete_reason", IncompleteReasonName(_result.incomplete_reason)},
                {"error_code", ErrorCodeName(_result.error_code)},
                {"limit", LimitName(_result.limit_kind)},
                {"codec_status", CodecStatusName(_result.codec_status)},
                {"input_bytes", U64(_result.input_bytes)},
                {"valid_prefix_bytes", U64(_result.valid_prefix_bytes)},
                {"error_byte_offset", OptionalU64(_result.error_byte_offset)},
                {"error_packet_index", OptionalU64(_result.error_packet_index)},
                {"incomplete_byte_offset", OptionalU64(_result.incomplete_byte_offset)},
                {"incomplete_packet_index", OptionalU64(_result.incomplete_packet_index)},
            },
        },
    };
    if (!_result.HasUsableSession()) {
        return document;
    }

    const ProfileSessionSummary& summary = _session.Summary();
    document["session"]                  = {
        {"generation", U64(summary.generation)},
        {"started_unix_ns", U64(summary.started_unix_ns)},
        {"has_session_end", summary.has_session_end},
        {
            "records_written",
            summary.has_session_end ? Json(U64(summary.session_end_records_written)) : Json(nullptr),
        },
        {
            "records_dropped",
            summary.has_session_end ? Json(U64(summary.session_end_records_dropped)) : Json(nullptr),
        },
    };
    document["packets"] = {
        {"total", U64(summary.packet_count)},
        {"schema_packets", U64(summary.schema_packet_count)},
        {"unique_schemas", U64(summary.unique_schema_count)},
        {"records", U64(summary.record_count)},
        {"loss_notices", U64(summary.loss_notice_count)},
    };
    document["records"] = {
        {"cpu_scopes", U64(summary.cpu_scope_count)},
        {"gpu_frames", U64(summary.gpu_frame_count)},
        {"gpu_scopes", U64(summary.gpu_scope_count)},
        {"unknown", U64(summary.unknown_record_count)},
    };
    document["loss"] = {
        {"noticed_records", U64(summary.lost_record_count)},
        {
            "unnotified_records",
            summary.has_session_end ? Json(U64(summary.unnotified_drop_count)) : Json(nullptr),
        },
        {"value_bytes", U64(summary.lost_value_bytes)},
        {"reason_mask", U64(summary.loss_reason_mask)},
    };
    document["cpu"] = {
        {"clock", "steady_clock_ns"},
        {"absolute_unix_anchor", false},
        {"tracks", U64(_session.CpuTracks().size())},
        {"scopes", U64(summary.cpu_scope_count)},
        {"orphan_scopes", U64(summary.orphan_cpu_scope_count)},
        {"has_range", summary.has_cpu_range},
        {"begin_ns", U64(summary.cpu_begin_ns)},
        {"end_ns", U64(summary.cpu_end_ns)},
    };
    document["gpu"] = {
        {"clock", "raw_device_ticks"},
        {"cross_cpu_alignment", "none"},
        {"cross_domain_alignment", "none"},
        {
            "frames",
            {
                {"total", U64(summary.gpu_frame_count)},
                {"complete", U64(summary.complete_gpu_frame_count)},
                {"degraded_complete", U64(summary.degraded_complete_gpu_frame_count)},
                {"incomplete", U64(summary.incomplete_gpu_frame_count)},
                {"invalid", U64(summary.invalid_gpu_frame_count)},
            },
        },
        {
            "scopes",
            {
                {"total", U64(summary.gpu_scope_count)},
                {"ready", U64(summary.ready_gpu_scope_count)},
                {"error", U64(summary.error_gpu_scope_count)},
                {"orphan", U64(summary.orphan_gpu_scope_count)},
            },
        },
        {"tracks", U64(_session.GpuTracks().size())},
    };

    Json domains = Json::array();
    for (const GpuTimestampDomain& domain : _session.GpuDomains()) {
        Json logical_queues = Json::array();
        if ((domain.logical_queue_mask & ProfileLogicalQueueBit(ProfileLogicalQueue::Graphics)) != 0) {
            logical_queues.push_back("graphics");
        }
        if ((domain.logical_queue_mask & ProfileLogicalQueueBit(ProfileLogicalQueue::Compute)) != 0) {
            logical_queues.push_back("compute");
        }
        if ((domain.logical_queue_mask & ProfileLogicalQueueBit(ProfileLogicalQueue::Copy)) != 0) {
            logical_queues.push_back("copy");
        }
        domains.push_back({
            {"native_queue_id", domain.native_queue_id},
            {"family_id", domain.family_id},
            {"logical_queues", std::move(logical_queues)},
            {"has_ready_timestamps", domain.has_ready_timestamps},
            {"timing_capability_trusted", domain.timing_capability_trusted},
            {"valid_bits", domain.valid_bits},
            {"tick_period_ns", domain.tick_period_ns},
            {"ready_scopes", U64(domain.ready_scope_count)},
            {"error_scopes", U64(domain.error_scope_count)},
        });
    }
    document["gpu"]["domains"]      = std::move(domains);
    document["logical_model_bytes"] = U64(summary.logical_model_bytes);
    return document;
}

int RunSummary(const std::filesystem::path& _path, const SessionLoadOptions& _options) {
    ProfileSession          session;
    const SessionLoadResult result = LoadProfileSessionFile(_path, _options, session);
    std::string             output = BuildJson(result, session).dump(2);
    output.push_back('\n');
    if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) {
        return ProfileSummaryExitCode(SessionLoadStatus::ResourceExhausted);
    }
    return ProfileSummaryExitCode(result.status);
}

void WriteEmergencySummary(SessionErrorCode _error) noexcept {
    const std::string_view output = ProfileSummaryEmergencyJson(_error);
    std::fwrite(output.data(), 1, output.size(), stdout);
    std::fputc('\n', stdout);
}

} // namespace

#if defined(_WIN32)
int wmain(int _argument_count, wchar_t** _arguments) {
    return RunProfileSummaryCliBoundary(
        [&]() -> int {
            if (_argument_count < 2 || _argument_count > 3) {
                std::cerr << "usage: MoerProfileSummary <capture.mpd> [--allow-truncated]\n";
                return 10;
            }

            SessionLoadOptions options{};
            if (_argument_count == 3) {
                if (std::wstring_view(_arguments[2]) != L"--allow-truncated") {
                    std::wcerr << L"unknown option: " << _arguments[2] << L'\n';
                    return 10;
                }
                options.allow_forensic_truncation = true;
            }

            return RunSummary(std::filesystem::path(_arguments[1]), options);
        },
        [](SessionErrorCode _error) noexcept {
            WriteEmergencySummary(_error);
        }
    );
}
#else
int main(int _argument_count, char** _arguments) {
    return RunProfileSummaryCliBoundary(
        [&]() -> int {
            if (_argument_count < 2 || _argument_count > 3) {
                std::cerr << "usage: MoerProfileSummary <capture.mpd> [--allow-truncated]\n";
                return 10;
            }

            SessionLoadOptions options{};
            if (_argument_count == 3) {
                if (std::string_view(_arguments[2]) != "--allow-truncated") {
                    std::cerr << "unknown option: " << _arguments[2] << '\n';
                    return 10;
                }
                options.allow_forensic_truncation = true;
            }

            return RunSummary(std::filesystem::path(_arguments[1]), options);
        },
        [](SessionErrorCode _error) noexcept {
            WriteEmergencySummary(_error);
        }
    );
}
#endif
