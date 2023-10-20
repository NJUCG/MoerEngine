#pragma once

#include <chrono>
namespace Moer {
    class Timer {
    public:
        void Start() noexcept {
            m_start_time = std::chrono::system_clock::now();
            m_is_running = true;
        }

        void Stop() noexcept {
            m_end_time   = std::chrono::system_clock::now();
            m_is_running = false;
        }

        double ElapsedMilliseconds() noexcept {
            std::chrono::time_point<std::chrono::system_clock> end_time =
                m_is_running ? std::chrono::system_clock::now() : m_end_time;
            return std::chrono::duration_cast<std::chrono::microseconds>(end_time - m_start_time).count() / 1000.;
        }

        double ElapsedSeconds() noexcept {
            return ElapsedMilliseconds() / 1000.0;
        }

        bool IsRunning() const noexcept { return m_is_running; }

    private:
        std::chrono::time_point<std::chrono::system_clock> m_start_time;
        std::chrono::time_point<std::chrono::system_clock> m_end_time;
        bool                                               m_is_running = false;
    };
}// namespace Moer