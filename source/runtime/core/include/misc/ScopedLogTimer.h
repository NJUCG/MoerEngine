#pragma once

#include "log/LogSystem.h"
#include "misc/Timer.h"

#include <source_location>
#include <string>
#include <string_view>

namespace Moer {

// Scoped lifetime timer that emits one debug log when leaving the current scope
class ScopedLogTimer {
public:
    explicit ScopedLogTimer(
        std::string_view      log_message,
        std::source_location  location = std::source_location::current()
    ) :
        m_log_message(log_message),
        m_location(location) {
        m_timer.Start();
    }

    ScopedLogTimer(
        std::string_view      prefix,
        std::string_view      stage_name,
        std::source_location  location = std::source_location::current()
    ) :
        m_location(location) {
        m_log_message.reserve(prefix.size() + stage_name.size() + 1);
        m_log_message.append(prefix);
        if (!m_log_message.empty() && !stage_name.empty()) {
            m_log_message.push_back(' ');
        }
        m_log_message.append(stage_name);

        m_timer.Start();
    }

    ScopedLogTimer(const ScopedLogTimer&)            = delete;
    ScopedLogTimer& operator=(const ScopedLogTimer&) = delete;
    ScopedLogTimer(ScopedLogTimer&&)                 = delete;
    ScopedLogTimer& operator=(ScopedLogTimer&&)      = delete;

    ~ScopedLogTimer() {
        if (!m_is_enabled) {
            return;
        }

        m_timer.Stop();
        spdlog::log(
            spdlog::source_loc{
                m_location.file_name(),
                static_cast<int>(m_location.line()),
                m_location.function_name()
            },
            spdlog::level::debug,
            "{} took {:.2f} ms",
            m_log_message,
            m_timer.ElapsedMilliseconds()
        );
    }

    void Cancel() noexcept {
        m_is_enabled = false;
    }

    double ElapsedMilliseconds() noexcept {
        return m_timer.ElapsedMilliseconds();
    }

private:
    std::string          m_log_message;
    std::source_location m_location;
    Timer                m_timer;
    bool                 m_is_enabled = true;
};

} // namespace Moer
