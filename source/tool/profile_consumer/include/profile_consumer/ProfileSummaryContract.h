#ifndef MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H
#define MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H

#include "profile_consumer/ProfileSession.h"

#include <string_view>

namespace Moer::ProfileDump {

inline constexpr std::string_view kProfileSummaryContractName{"moer.profile.summary"};
inline constexpr std::uint32_t    kProfileSummaryContractVersion{1};

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

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_SUMMARY_CONTRACT_H
