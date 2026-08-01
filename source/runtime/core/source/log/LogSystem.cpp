#include "log/LogSystem.h"

#include "spdlog/details/os.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/sink.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace Moer::LogSystem {

namespace {

namespace fmt_lib = spdlog::fmt_lib;

constexpr std::size_t ConsoleLogCapacity        = 4096;
constexpr std::size_t AbiSafeLoggerNameCapacity = 64;
constexpr std::string_view FileLogPattern = "%Y-%m-%d %H:%M:%S.%e [%t] [%l] %v";

std::string MakeCoreOwnedLoggerName(std::string_view _name) {
    std::string owned_name;
    // An exported spdlog::logger can move this string across a Windows DLL
    // boundary. Force owned storage even for an empty/short logger name so the
    // move never carries MSVC's small-string representation across heaps.
    owned_name.reserve((std::max)(AbiSafeLoggerNameCapacity, _name.size()));
    if (!_name.empty()) {
        owned_name.append(_name.data(), _name.size());
    }
    return owned_name;
}

std::string MakeTimestampedFileName() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()
                              ) %
                              std::chrono::seconds(1);
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const std::tm     local_time = spdlog::details::os::localtime(time);

    std::array<char, 128> file_name{};
    const int written = std::snprintf(
        file_name.data(),
        file_name.size(),
        "MoerEditor_%04d%02d%02d_%02d%02d%02d_%03lld_p%d.log",
        local_time.tm_year + 1900,
        local_time.tm_mon + 1,
        local_time.tm_mday,
        local_time.tm_hour,
        local_time.tm_min,
        local_time.tm_sec,
        static_cast<long long>(milliseconds.count()),
        spdlog::details::os::pid()
    );
    if (written <= 0 || static_cast<std::size_t>(written) >= file_name.size()) {
        throw std::runtime_error("timestamped log file name exceeded its fixed buffer");
    }
    return std::string(file_name.data(), static_cast<std::size_t>(written));
}

struct FileLogSetupResult {
    EFileLogInitStatus                  status = EFileLogInitStatus::Disabled;
    std::shared_ptr<spdlog::sinks::sink> sink;
    std::string                         path;
    std::string                         error;
};

FileLogSetupResult CreateFileLogSink(const FileLogInitOptions& _options) {
    FileLogSetupResult result;
    if (_options.directory.empty()) {
        return result;
    }

    try {
        const std::filesystem::path directory(std::string(_options.directory));
        std::error_code             directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (directory_error || !std::filesystem::is_directory(directory, directory_error)) {
            result.status = EFileLogInitStatus::Failed;
            result.error  = "could not create log directory '" + directory.generic_string() + "'";
            if (directory_error) {
                result.error.append(": ").append(directory_error.message());
            }
            return result;
        }

        const std::filesystem::path requested_file_name = _options.file_name.empty() ?
                                                                    MakeTimestampedFileName() :
                                                                    std::string(_options.file_name);
        const std::filesystem::path file_name = requested_file_name.filename();
        if (file_name.empty() || file_name == "." || file_name == "..") {
            result.status = EFileLogInitStatus::Failed;
            result.error  = "log file name is empty or invalid";
            return result;
        }

        const std::filesystem::path log_path = directory / file_name;
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            log_path.string(), true
        );
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern(std::string(FileLogPattern));

        result.status = EFileLogInitStatus::Active;
        result.sink   = std::move(file_sink);
        result.path   = log_path.generic_string();
        return result;
    } catch (const std::exception& error) {
        result.status = EFileLogInitStatus::Failed;
        result.error  = error.what();
        return result;
    } catch (...) {
        result.status = EFileLogInitStatus::Failed;
        result.error  = "unknown file logging initialization failure";
        return result;
    }
}

struct ConsoleLogEntryStorage {
    std::uint64_t             sequence = 0;
    spdlog::level::level_enum level    = spdlog::level::info;
    std::string               message;
};

class ConsoleLogSink final : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg& _message) override {
        std::string owned_message;
        if (_message.payload.size() != 0) {
            owned_message.assign(_message.payload.data(), _message.payload.size());
        }

        std::lock_guard lock(storage_mutex);
        entries.push_back({
            .sequence = next_sequence,
            .level    = _message.level,
            .message  = std::move(owned_message),
        });
        ++next_sequence;
        if (entries.size() > ConsoleLogCapacity) {
            entries.pop_front();
        }
    }

    void flush() override {}

    // The channel intentionally retains the formatted payload rather than a
    // second sink-specific presentation of it.
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    [[nodiscard]] ConsoleLogPollResult
    Visit(std::uint64_t _next_sequence, std::size_t _max_count, ConsoleLogVisitor _visitor, void* _context) {
        ConsoleLogPollResult result;
        result.next_sequence = _next_sequence == 0 ? 1 : _next_sequence;

        std::vector<ConsoleLogEntryStorage> copied_entries;
        {
            std::lock_guard lock(storage_mutex);
            if (entries.empty()) {
                if (result.next_sequence < next_sequence) {
                    result.dropped_count = next_sequence - result.next_sequence;
                    result.next_sequence = next_sequence;
                }
                return result;
            }

            const std::uint64_t first_sequence = entries.front().sequence;
            if (result.next_sequence < first_sequence) {
                result.dropped_count = first_sequence - result.next_sequence;
                result.next_sequence = first_sequence;
            }
            if (_max_count == 0 || !_visitor) {
                return result;
            }

            copied_entries.reserve((std::min)(_max_count, entries.size()));
            for (const ConsoleLogEntryStorage& entry : entries) {
                if (entry.sequence < result.next_sequence) {
                    continue;
                }
                copied_entries.push_back(entry);
                result.next_sequence = entry.sequence + 1;
                if (copied_entries.size() == _max_count) {
                    break;
                }
            }
        }

        for (const ConsoleLogEntryStorage& entry : copied_entries) {
            _visitor(
                {
                    .sequence = entry.sequence,
                    .level    = entry.level,
                    .message  = entry.message,
                },
                _context
            );
        }
        result.visited_count = copied_entries.size();
        return result;
    }

    void Clear() {
        std::lock_guard lock(storage_mutex);
        entries.clear();
    }

private:
    std::mutex                         storage_mutex;
    std::deque<ConsoleLogEntryStorage> entries;
    std::uint64_t                      next_sequence = 1;
};

// spdlog exposes these operations to derived logger implementations. This
// adapter is never instantiated; it only obtains public member pointers through
// the derived access path, then invokes the base operations on the actual
// logger. That preserves complete log_msg metadata and the configured error
// handler without changing the vendored spdlog API or object layout.
class LoggerDispatchAccess : public spdlog::logger {
public:
    using spdlog::logger::err_handler_;
    using spdlog::logger::log_it_;

    static void Log(spdlog::logger& _logger, const spdlog::details::log_msg& _message) {
        const bool log_enabled       = _logger.should_log(_message.level);
        const bool backtrace_enabled = _logger.should_backtrace();
        if (!log_enabled && !backtrace_enabled) {
            return;
        }

        using LogFunction = void (spdlog::logger::*)(const spdlog::details::log_msg&, bool, bool);
        const LogFunction log_function = &LoggerDispatchAccess::log_it_;
        (_logger.*log_function)(_message, log_enabled, backtrace_enabled);
    }

    static void ReportError(spdlog::logger& _logger, const std::string& _message) {
        using ErrorFunction                = void (spdlog::logger::*)(const std::string&);
        const ErrorFunction error_function = &LoggerDispatchAccess::err_handler_;
        (_logger.*error_function)(_message);
    }
};

class ConsoleForwardingLogger final : public spdlog::logger {
public:
    ConsoleForwardingLogger(
        std::string                     _name,
        std::shared_ptr<spdlog::logger> _source_logger,
        std::shared_ptr<spdlog::logger> _dispatch_logger,
        std::shared_ptr<ConsoleLogSink> _console_sink,
        std::shared_ptr<spdlog::sinks::sink> _file_sink
    ) :
        spdlog::logger(std::move(_name)),
        source_logger(std::move(_source_logger)),
        dispatch_logger(std::move(_dispatch_logger)),
        console_sink(std::move(_console_sink)),
        file_sink(std::move(_file_sink)) {
        const spdlog::level::level_enum source_level       = source_logger->level();
        const spdlog::level::level_enum source_flush_level = source_logger->flush_level();
        set_level(source_level);
        flush_on(source_flush_level);
        source_logger->set_level(spdlog::level::off);
        source_logger->flush_on(spdlog::level::off);
        dispatch_logger->set_level(spdlog::level::trace);
        dispatch_logger->flush_on(source_flush_level);
        set_error_handler([error_logger = dispatch_logger](const std::string& _message) {
            LoggerDispatchAccess::ReportError(*error_logger, _message);
        });
    }

protected:
    void sink_it_(const spdlog::details::log_msg& _message) override {
        // The forwarding logger is the registered default, so global level and
        // flush-level changes land here. The private dispatch clone remains at
        // trace level so dump_backtrace() entries bypass a second logger-level
        // filter while still using the source logger's virtual dispatch (for
        // example an async queue or a custom sink_it_ override).
        SynchronizeConfiguration();
        SPDLOG_TRY {
            LoggerDispatchAccess::Log(*dispatch_logger, _message);
        }
        SPDLOG_LOGGER_CATCH(_message.source)

        SPDLOG_TRY {
            console_sink->log(_message);
        }
        SPDLOG_LOGGER_CATCH(_message.source)

        if (file_sink && file_sink->should_log(_message.level)) {
            SPDLOG_TRY {
                file_sink->log(_message);
                if (_message.level >= spdlog::level::err) {
                    file_sink->flush();
                }
            }
            SPDLOG_LOGGER_CATCH(_message.source)
        }

        // Keep the normal logger::sinks() extension point functional without
        // placing the downstream sink vector in a different module's heap.
        for (const spdlog::sink_ptr& sink : sinks_) {
            if (sink->should_log(_message.level)) {
                SPDLOG_TRY {
                    sink->log(_message);
                }
                SPDLOG_LOGGER_CATCH(_message.source)
            }
        }

        if (should_flush_(_message)) {
            SPDLOG_TRY {
                console_sink->flush();
            }
            SPDLOG_LOGGER_CATCH(_message.source)
            if (file_sink) {
                SPDLOG_TRY {
                    file_sink->flush();
                }
                SPDLOG_LOGGER_CATCH(_message.source)
            }
            for (const spdlog::sink_ptr& sink : sinks_) {
                SPDLOG_TRY {
                    sink->flush();
                }
                SPDLOG_LOGGER_CATCH(_message.source)
            }
        }
    }

    void flush_() override {
        SynchronizeConfiguration();
        SPDLOG_TRY {
            dispatch_logger->flush();
        }
        SPDLOG_LOGGER_CATCH(spdlog::source_loc())
        SPDLOG_TRY {
            console_sink->flush();
        }
        SPDLOG_LOGGER_CATCH(spdlog::source_loc())
        if (file_sink) {
            SPDLOG_TRY {
                file_sink->flush();
            }
            SPDLOG_LOGGER_CATCH(spdlog::source_loc())
        }
        for (const spdlog::sink_ptr& sink : sinks_) {
            SPDLOG_TRY {
                sink->flush();
            }
            SPDLOG_LOGGER_CATCH(spdlog::source_loc())
        }
    }

public:
    std::shared_ptr<spdlog::logger> clone(std::string _logger_name) override {
        // Clones preserve the original logger behavior. The console and file
        // channels are intentionally default-logger scoped; make a clone
        // default and call LogSystem::Init again if it also needs capture.
        SynchronizeConfiguration();
        std::shared_ptr<spdlog::logger> cloned = source_logger->clone(std::move(_logger_name));
        if (cloned) {
            cloned->set_level(level());
            cloned->flush_on(flush_level());
        }
        return cloned;
    }

private:
    void SynchronizeConfiguration() {
        source_logger->set_level(spdlog::level::off);
        source_logger->flush_on(spdlog::level::off);
        dispatch_logger->set_level(spdlog::level::trace);
        dispatch_logger->flush_on(flush_level());
    }

    std::shared_ptr<spdlog::logger> source_logger;
    std::shared_ptr<spdlog::logger> dispatch_logger;
    std::shared_ptr<ConsoleLogSink> console_sink;
    std::shared_ptr<spdlog::sinks::sink> file_sink;
};

struct LogRuntime {
    std::mutex                                   install_mutex;
    std::shared_ptr<ConsoleLogSink>              console_sink = std::make_shared<ConsoleLogSink>();
    std::shared_ptr<spdlog::sinks::sink>         file_sink;
    std::vector<std::shared_ptr<spdlog::logger>> installed_loggers;
    EFileLogInitStatus                           file_status = EFileLogInitStatus::Disabled;
    bool                                         file_configuration_locked = false;
    std::string                                  file_path;
};

LogRuntime& GetLogRuntime() {
    // spdlog owns its registry in a separate shared library. Keep the Core
    // sink and its state alive until address-space reclamation so registry
    // teardown cannot become the final release of a Core-defined vtable.
    static LogRuntime* const runtime = new LogRuntime();
    return *runtime;
}

} // namespace

void Init() {
    static_cast<void>(Init(FileLogInitOptions{}));
}

EFileLogInitStatus Init(const FileLogInitOptions& _options) {

#if !defined(NDEBUG)
    spdlog::set_level(spdlog::level::trace);
#endif

    LogRuntime& runtime = GetLogRuntime();
    bool        report_file_active = false;
    bool        report_file_failure = false;
    std::string report_path;
    std::string report_error;

    {
        std::lock_guard install_lock(runtime.install_mutex);

        const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
        if (!logger) {
            return _options.directory.empty() ? EFileLogInitStatus::Disabled :
                                                EFileLogInitStatus::Failed;
        }

        const bool already_attached = std::ranges::any_of(
            runtime.installed_loggers,
            [&logger](const std::shared_ptr<spdlog::logger>& _installed) {
                return _installed.get() == logger.get();
            }
        );
        if (already_attached) {
            return runtime.file_status;
        }

        // logger::sinks() is an owning std::vector. Mutating or copying it into
        // a logger owned by another DLL makes later reallocation/free depend on
        // the wrong mimalloc heap. Leave the original logger untouched and
        // forward to it instead. Runtime keeps every forwarding logger alive
        // so spdlog shutdown cannot become the final release of Core-defined
        // state.
        const std::string& downstream_name = logger->name();
        const std::string_view downstream_name_view(
            downstream_name.data(), downstream_name.size()
        );
        // The private clone only handles below-threshold backtrace replay and
        // is retained for process lifetime with its Core-owned, non-SSO name
        // storage. All messages use the clone so explicit flush has exactly one
        // downstream target and custom/async virtual dispatch remains intact.
        std::shared_ptr<spdlog::logger> dispatch_logger =
            logger->clone(MakeCoreOwnedLoggerName(downstream_name_view));
        if (!dispatch_logger) {
            return _options.directory.empty() ? runtime.file_status : EFileLogInitStatus::Failed;
        }
        dispatch_logger->disable_backtrace();
        dispatch_logger->set_level(spdlog::level::trace);

        if (!runtime.file_configuration_locked) {
            FileLogSetupResult setup = CreateFileLogSink(_options);
            runtime.file_configuration_locked = true;
            runtime.file_status               = setup.status;
            runtime.file_sink                 = std::move(setup.sink);
            runtime.file_path                 = std::move(setup.path);
            report_error                      = std::move(setup.error);
            report_file_active                = runtime.file_status == EFileLogInitStatus::Active;
            report_file_failure               = runtime.file_status == EFileLogInitStatus::Failed;
            report_path                       = runtime.file_path;
        }

        auto replacement = std::make_shared<ConsoleForwardingLogger>(
            MakeCoreOwnedLoggerName(downstream_name_view),
            logger,
            std::move(dispatch_logger),
            runtime.console_sink,
            runtime.file_sink
        );

        runtime.installed_loggers.push_back(replacement);
        spdlog::set_default_logger(std::move(replacement));
    }

    if (report_file_active) {
        try {
            if (const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger()) {
                logger->info("[LogSystem] Session log: {}", report_path);
            }
        } catch (...) {
            std::fprintf(stderr, "[LogSystem] Session log: %s\n", report_path.c_str());
        }
    } else if (report_file_failure) {
        try {
            if (const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger()) {
                logger->warn("[LogSystem] File logging disabled: {}", report_error);
            }
        } catch (...) {
            std::fprintf(
                stderr, "[LogSystem] File logging disabled: %s\n", report_error.c_str()
            );
        }
    }

    return runtime.file_status;
}

void Flush() noexcept {
    std::shared_ptr<spdlog::sinks::sink> file_sink;
    try {
        LogRuntime& runtime = GetLogRuntime();
        {
            std::lock_guard lock(runtime.install_mutex);
            file_sink = runtime.file_sink;
        }
        if (file_sink) {
            file_sink->flush();
        }
    } catch (...) {
    }
}

ConsoleLogPollResult VisitConsoleLogs(
    std::uint64_t     _next_sequence,
    std::size_t       _max_count,
    ConsoleLogVisitor _visitor,
    void*             _context
) {
    return GetLogRuntime().console_sink->Visit(_next_sequence, _max_count, _visitor, _context);
}

void ClearConsoleLogs() {
    GetLogRuntime().console_sink->Clear();
}

} // namespace Moer::LogSystem
