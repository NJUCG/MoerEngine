#ifndef MOER_ENGINE_PROFILE_SCOPE_H
#define MOER_ENGINE_PROFILE_SCOPE_H

#include "API_Macro.h"
#include "profile/ProfileDump.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Moer::ProfileDump {

enum class CpuScopeActivationResult : std::uint8_t {
    Activated = 0,
    AlreadyActive,
    InvalidHandle,
    RuntimeNotRunning,
    StaleGeneration,
    WrongSchema,
};

namespace CpuScopeProducer {

[[nodiscard]] CORE_API CpuScopeActivationResult Activate(SchemaHandle _schema) noexcept;
CORE_API void                                            Deactivate() noexcept;
[[nodiscard]] CORE_API bool                             IsActive() noexcept;

} // namespace CpuScopeProducer

class CORE_API ScopedCpuProfile final {
public:
    static constexpr std::size_t kMaxNameBytes = 256;

    explicit ScopedCpuProfile(std::string_view _name) noexcept;
    ScopedCpuProfile(std::string_view _prefix, std::string_view _name) noexcept;
    ~ScopedCpuProfile() noexcept;

    ScopedCpuProfile(const ScopedCpuProfile&)            = delete;
    ScopedCpuProfile& operator=(const ScopedCpuProfile&) = delete;
    ScopedCpuProfile(ScopedCpuProfile&&)                 = delete;
    ScopedCpuProfile& operator=(ScopedCpuProfile&&)      = delete;

private:
    void Begin(std::string_view _prefix, std::string_view _name, bool _join) noexcept;

    std::array<char, kMaxNameBytes> name_{};
    SchemaHandle                    schema_{};
    std::uint64_t                   thread_id_{0};
    std::uint64_t                   begin_ns_{0};
    std::uint32_t                   depth_{0};
    std::uint16_t                   name_bytes_{0};
    bool                            active_{false};
};

} // namespace Moer::ProfileDump

#define MOER_PROFILE_DETAIL_JOIN_INNER(Left, Right) Left##Right
#define MOER_PROFILE_DETAIL_JOIN(Left, Right) MOER_PROFILE_DETAIL_JOIN_INNER(Left, Right)
#define MOER_PROFILE_SCOPE(Name)                                                                    \
    ::Moer::ProfileDump::ScopedCpuProfile MOER_PROFILE_DETAIL_JOIN(_moer_profile_scope_, __COUNTER__)(Name)
#define MOER_PROFILE_FUNCTION() MOER_PROFILE_SCOPE(__func__)

#endif // MOER_ENGINE_PROFILE_SCOPE_H
