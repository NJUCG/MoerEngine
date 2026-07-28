#ifndef MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H
#define MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H

#include "profile_consumer/ProfileSession.h"

#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Moer::ProfileDump {

inline constexpr std::string_view kProfileSummaryContractName{"moer.profile.summary"};
inline constexpr std::uint32_t    kProfileSummaryContractVersion{1};

inline constexpr std::string_view kProfileSummaryAllocationFailureJson{
    R"json({"contract":"moer.profile.summary","version":1,"load":{"status":"resource_exhausted","incomplete_reason":"none","error_code":"resource_allocation_failed","limit":"none","codec_status":"ok","input_bytes":"0","valid_prefix_bytes":"0","error_byte_offset":null,"error_packet_index":null,"incomplete_byte_offset":null,"incomplete_packet_index":null}})json"
};
inline constexpr std::string_view kProfileSummaryUnexpectedFailureJson{
    R"json({"contract":"moer.profile.summary","version":1,"load":{"status":"resource_exhausted","incomplete_reason":"none","error_code":"unexpected_failure","limit":"none","codec_status":"ok","input_bytes":"0","valid_prefix_bytes":"0","error_byte_offset":null,"error_packet_index":null,"incomplete_byte_offset":null,"incomplete_packet_index":null}})json"
};

[[nodiscard]] inline constexpr std::string_view ProfileSummaryEmergencyJson(SessionErrorCode _error
) noexcept {
    return _error == SessionErrorCode::ResourceAllocationFailed ? kProfileSummaryAllocationFailureJson :
                                                                  kProfileSummaryUnexpectedFailureJson;
}

[[nodiscard]] inline constexpr int ProfileSummaryExitCode(SessionLoadStatus _status) noexcept {
    switch (_status) {
        case SessionLoadStatus::Complete:
            return 0;
        case SessionLoadStatus::ForensicTruncated:
            return 2;
        case SessionLoadStatus::InvalidArgument:
        case SessionLoadStatus::Reading:
            return 10;
        case SessionLoadStatus::OpenFailed:
        case SessionLoadStatus::ReadFailed:
            return 11;
        case SessionLoadStatus::UnsupportedVersion:
        case SessionLoadStatus::CorruptData:
        case SessionLoadStatus::ProtocolViolation:
            return 12;
        case SessionLoadStatus::LimitExceeded:
            return 13;
        case SessionLoadStatus::ResourceExhausted:
            return 14;
    }
    return 12;
}

template<typename Operation, typename EmergencyWriter>
[[nodiscard]] int
RunProfileSummaryCliBoundary(Operation&& _operation, EmergencyWriter&& _emergency_writer) noexcept {
    static_assert(std::is_nothrow_invocable_v<EmergencyWriter, SessionErrorCode>);
    try {
        return std::forward<Operation>(_operation)();
    } catch (const std::bad_alloc&) {
        std::forward<EmergencyWriter>(_emergency_writer)(SessionErrorCode::ResourceAllocationFailed);
    } catch (...) {
        std::forward<EmergencyWriter>(_emergency_writer)(SessionErrorCode::UnexpectedFailure);
    }
    return ProfileSummaryExitCode(SessionLoadStatus::ResourceExhausted);
}

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H
