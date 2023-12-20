#include "log/LogSystem.h"
#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "platform/Platform.h"

#include <format>
#include <memory>

#include "spdlog/fmt/bundled/color.h"

#include <spdlog/sinks/sink.h>

// class Sink : public spdlog::sinks::sink {
// public:
//     Sink(spdlog::color_mode mode = spdlog::color_mode::automatic) : formatter_{spdlog::details::make_unique<spdlog::pattern_formatter>()} {
//         styles_[spdlog::level::trace]    = fmt::fg(fmt::terminal_color::bright_black) | fmt::emphasis::bold;
//         styles_[spdlog::level::debug]    = fmt::fg(fmt::terminal_color::bright_white) | fmt::emphasis::bold;
//         styles_[spdlog::level::info]     = fmt::fg(fmt::terminal_color::bright_green) | fmt::emphasis::bold;
//         styles_[spdlog::level::warn]     = fmt::fg(fmt::terminal_color::bright_yellow) | fmt::emphasis::bold;
//         styles_[spdlog::level::err]      = fmt::fg(fmt::terminal_color::bright_red) | fmt::emphasis::bold;
//         styles_[spdlog::level::critical] = fmt::fg(fmt::terminal_color::bright_magenta) | fmt::emphasis::bold;
// #ifdef _WIN32
//         handle_                         = ::GetStdHandle(STD_OUTPUT_HANDLE);
//         DWORD console_mode              = 0;
//         in_console_                     = ::GetConsoleMode(handle_, &console_mode) != 0;
//         CONSOLE_SCREEN_BUFFER_INFO info = {};
//         ::GetConsoleScreenBufferInfo(handle_, &info);
//         colors_[spdlog::level::trace]    = FOREGROUND_INTENSITY;
//         colors_[spdlog::level::debug]    = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
//         colors_[spdlog::level::info]     = FOREGROUND_INTENSITY | FOREGROUND_GREEN;
//         colors_[spdlog::level::warn]     = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
//         colors_[spdlog::level::err]      = FOREGROUND_INTENSITY | FOREGROUND_RED;
//         colors_[spdlog::level::critical] = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE;
//         colors_[spdlog::level::off]      = info.wAttributes;
// #endif
//         set_color_mode(mode);
//     }

//     Sink(const Sink& other)            = delete;
//     Sink& operator=(const Sink& other) = delete;

//     ~Sink() override = default;

//     void log(const spdlog::details::log_msg& msg) override {
//         spdlog::memory_buf_t formatted;
//         std::lock_guard      lock{mutex_};
//         formatter_->format(msg, formatted);
//         if (should_color_ && msg.level < styles_.size() && msg.color_range_end > msg.color_range_start) {
//             print_range(formatted, 0, msg.color_range_start);
//             print_range(formatted, msg.color_range_start, msg.color_range_end, msg.level);
//             print_range(formatted, msg.color_range_end, formatted.size());
//         } else {
//             print_range(formatted, 0, formatted.size());
//         }
//         // fflush(file);
//     }

//     void set_pattern(const std::string& pattern) final {
//         std::lock_guard lock{mutex_};
//         formatter_ = std::make_unique<spdlog::pattern_formatter>(pattern);
//     }

//     void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
//         std::lock_guard lock{mutex_};
//         formatter_ = std::move(formatter);
//     }

//     void flush() override {
//         std::lock_guard lock{mutex_};
//         fflush(stdout);
//     }

//     void set_color_mode(spdlog::color_mode mode) {
//         std::lock_guard lock{mutex_};
//         switch (mode) {
//             case spdlog::color_mode::always:
//                 should_color_ = true;
//                 break;
//             case spdlog::color_mode::automatic:
// #if _WIN32
//                 should_color_ = spdlog::details::os::in_terminal(stdout) || IsDebuggerPresent();
// #else
//                 should_color_ = spdlog::details::os::in_terminal(stdout) && spdlog::details::os::is_color_terminal();
// #endif
//                 break;
//             case spdlog::color_mode::never:
//                 should_color_ = false;
//                 break;
//         }
//     }

// private:
//     void print_range(const spdlog::memory_buf_t& formatted, size_t start, size_t end) {
// #ifdef _WIN32
//         if (in_console_) {
//             auto data = formatted.data() + start;
//             auto size = static_cast<DWORD>(end - start);
//             while (size > 0) {
//                 DWORD written = 0;
//                 if (!::WriteFile(handle_, data, size, &written, nullptr) || written == 0 || written > size) {
//                     SPDLOG_THROW(spdlog::spdlog_ex("sink: print_range failed. GetLastError(): " + std::to_string(::GetLastError())));
//                 }
//                 size -= written;
//             }
//             return;
//         }
// #endif
//         fwrite(formatted.data() + start, sizeof(char), end - start, stdout);
//     }

//     void print_range(const spdlog::memory_buf_t& formatted, size_t start, size_t end, spdlog::level::level_enum level) {
// #ifdef _WIN32
//         if (in_console_) {
//             ::SetConsoleTextAttribute(handle_, colors_[level]);
//             print_range(formatted, start, end);
//             ::SetConsoleTextAttribute(handle_, colors_[spdlog::level::off]);
//             return;
//         }
// #endif
//         fmt::print(stdout, styles_[level], "{}", std::string_view{formatted.data() + start, end - start});
//     }

//     std::mutex                                      mutex_;
//     std::unique_ptr<spdlog::formatter>              formatter_;
//     Moer::StaticArray<fmt::text_style, spdlog::level::off> styles_;
// #ifdef _WIN32
//     Moer::StaticArray<DWORD, 7> colors_;
//     bool                 in_console_ = true;
//     HANDLE               handle_     = nullptr;
// #endif
//     bool should_color_ = false;
// };
namespace Moer {
namespace LogSystem {
    void Init() {
        // auto console_sink = std::make_shared<Sink>();
        // #if PLATFORM_WINDOWS
        //             console_sink->set_color_mode(spdlog::color_mode::always);
        //             console_sink->set_level(spdlog::level::debug);
        // #endif
        //             spdlog::sinks::stderr_sink_st stl;
        //             spdlog::logger                console("console", {console_sink});
        //             console.set_level(spdlog::level::debug);
    }
}
}// namespace Moer::LogSystem