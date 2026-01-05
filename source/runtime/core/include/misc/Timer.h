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

    bool IsRunning() const noexcept {
        return m_is_running;
    }

private:
    std::chrono::time_point<std::chrono::system_clock> m_start_time;
    std::chrono::time_point<std::chrono::system_clock> m_end_time;
    bool                                               m_is_running = false;
};

// 初始化时，传入interval；
// 每次调用时，检测当前时间是否超过上次触发时间+interval，如果超过则更新上次触发时间并返回true，否则返回false
// 返回true后，默认重置计时器，可以通过额外的参数控制是否重置
class LoopedTimer {
public:
    LoopedTimer(double interval_seconds, bool is_trigger_immediately = true) noexcept :
        m_interval(std::chrono::duration<double>(interval_seconds)) {

        Reset(is_trigger_immediately);
    }

    bool Tick(bool is_reset_when_trigger = true) noexcept {
        auto now = std::chrono::system_clock::now();

        if (now - m_last_time >= m_interval) {
            if (is_reset_when_trigger) {
                m_last_time = now;
            }
            return true;
        }
        return false;
    }

    void Reset(bool is_trigger_immediately) noexcept {
        using sys_clock = std::chrono::system_clock;
        if (is_trigger_immediately) {
            m_last_time = sys_clock::now() - std::chrono::duration_cast<sys_clock::duration>(m_interval) -
                          std::chrono::milliseconds(1);
        } else {
            m_last_time = sys_clock::now();
        }
    }

private:
    std::chrono::duration<double>                      m_interval;
    std::chrono::time_point<std::chrono::system_clock> m_last_time;
};

} // namespace Moer