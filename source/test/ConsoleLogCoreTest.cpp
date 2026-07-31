#include "log/LogSystem.h"

#include "spdlog/details/os.h"
#include "spdlog/sinks/null_sink.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace Moer;

constexpr std::size_t ConsoleLogCapacity = 4096;

struct CapturedEntry {
    std::uint64_t             sequence = 0;
    spdlog::level::level_enum level    = spdlog::level::info;
    std::string               message;
};

struct CapturedBatch {
    std::vector<CapturedEntry>      entries;
    LogSystem::ConsoleLogPollResult result;
};

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

CapturedBatch Poll(std::uint64_t _next_sequence, std::size_t _max_count) {
    CapturedBatch batch;
    batch.result = LogSystem::VisitConsoleLogs(
        _next_sequence,
        _max_count,
        [](const LogSystem::ConsoleLogEntryView& _entry, void* _context) {
            static_cast<CapturedBatch*>(_context)->entries.push_back({
                .sequence = _entry.sequence,
                .level    = _entry.level,
                .message  = std::string(_entry.message),
            });
        },
        &batch
    );
    return batch;
}

std::uint64_t ClearAndGetNextSequence() {
    LogSystem::ClearConsoleLogs();
    return LogSystem::VisitConsoleLogs(1, 0, nullptr, nullptr).next_sequence;
}

class DefaultLoggerRestore final {
public:
    DefaultLoggerRestore() : saved_logger(spdlog::default_logger()) {}

    ~DefaultLoggerRestore() {
        try {
            spdlog::set_default_logger(saved_logger);
            spdlog::drop("console-log-core-contract");
            spdlog::drop("console-log-core-replacement");
        } catch (...) {
        }
    }

private:
    std::shared_ptr<spdlog::logger> saved_logger;
};

class CountingSink final : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg&) override {
        ++log_count;
    }

    void flush() override {
        ++flush_count;
    }

    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    std::atomic<std::size_t> log_count{0};
    std::atomic<std::size_t> flush_count{0};
};

class ThrowingSink final : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg&) override {
        throw std::runtime_error("intentional console log sink failure");
    }

    void flush() override {
        throw std::runtime_error("intentional console log flush failure");
    }

    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
};

class ThreadRecordingSink final : public spdlog::sinks::sink {
    struct Record {
        std::string message;
        std::size_t thread_id = 0;
    };

public:
    void log(const spdlog::details::log_msg& _message) override {
        std::lock_guard lock(records_mutex);
        records.push_back({
            .message   = std::string(_message.payload.data(), _message.payload.size()),
            .thread_id = _message.thread_id,
        });
    }

    void flush() override {}
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    [[nodiscard]] std::size_t FindThreadId(std::string_view _message) const {
        std::lock_guard lock(records_mutex);
        const auto      found = std::ranges::find(records, _message, &Record::message);
        return found == records.end() ? 0 : found->thread_id;
    }

private:
    mutable std::mutex  records_mutex;
    std::vector<Record> records;
};

struct VirtualDispatchState {
    std::atomic<std::size_t> clone_count{0};
    std::atomic<std::size_t> dispatch_count{0};
};

class VirtualDispatchLogger final : public spdlog::logger {
public:
    VirtualDispatchLogger(
        std::string                           _name,
        spdlog::sink_ptr                      _sink,
        std::shared_ptr<VirtualDispatchState> _state
    ) :
        spdlog::logger(std::move(_name), std::move(_sink)),
        state(std::move(_state)) {}

    std::shared_ptr<spdlog::logger> clone(std::string _logger_name) override {
        ++state->clone_count;
        auto cloned =
            std::make_shared<VirtualDispatchLogger>(std::move(_logger_name), sinks().front(), state);
        cloned->set_level(level());
        cloned->flush_on(flush_level());
        return cloned;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& _message) override {
        ++state->dispatch_count;
        spdlog::logger::sink_it_(_message);
    }

private:
    std::shared_ptr<VirtualDispatchState> state;
};

void TestInstallationAndCapture(
    std::shared_ptr<spdlog::logger>&            _logger,
    const std::shared_ptr<spdlog::sinks::sink>& _original_sink
) {
    LogSystem::ClearConsoleLogs();

    const std::shared_ptr<spdlog::logger> original_logger     = _logger;
    const std::size_t                     original_sink_count = original_logger->sinks().size();
    const std::string                     original_name       = original_logger->name();
    const spdlog::level::level_enum       original_level      = original_logger->level();
    auto                                  source_error_count  = std::make_shared<std::atomic<std::size_t>>(0);
    original_logger->set_error_handler([source_error_count](const std::string&) {
        ++*source_error_count;
    });
    original_logger->flush_on(spdlog::level::err);
    LogSystem::Init();
    const std::shared_ptr<spdlog::logger> installed_logger = spdlog::default_logger();
    LogSystem::Init();
    Expect(
        installed_logger && installed_logger.get() != original_logger.get() &&
            spdlog::default_logger().get() == installed_logger.get() &&
            installed_logger->name() == original_name && installed_logger->level() == original_level &&
            installed_logger->flush_level() == spdlog::level::err &&
            original_logger->level() == spdlog::level::off &&
            original_logger->flush_level() == spdlog::level::off && installed_logger->sinks().empty() &&
            original_logger->sinks().size() == original_sink_count &&
            std::ranges::any_of(
                original_logger->sinks(),
                [&_original_sink](const spdlog::sink_ptr& _sink) {
                    return _sink.get() == _original_sink.get();
                }
            ),
        "LogSystem::Init did not install one ABI-safe forwarding logger"
    );
    const auto*       original_counting_sink = dynamic_cast<CountingSink*>(_original_sink.get());
    const std::size_t retained_source_count =
        original_counting_sink ? original_counting_sink->log_count.load() : 0;
    spdlog::set_level(spdlog::level::warn);
    Expect(
        installed_logger->level() == spdlog::level::warn && original_logger->level() == spdlog::level::off,
        "global level changes re-enabled a retained source logger"
    );
    original_logger->critical("console-log-retained-source-disabled");
    Expect(
        !original_counting_sink || original_counting_sink->log_count.load() == retained_source_count,
        "a retained source logger bypassed the forwarding logger's level authority"
    );
    installed_logger->set_level(spdlog::level::trace);
    _logger = installed_logger;

    auto throwing_sink = std::make_shared<ThrowingSink>();
    auto external_sink = std::make_shared<CountingSink>();
    installed_logger->sinks().push_back(throwing_sink);
    installed_logger->sinks().push_back(external_sink);
    installed_logger->sinks().reserve(8);

    const std::uint64_t cursor = ClearAndGetNextSequence();
    LOG_INFO("console-log-contract-info");
    spdlog::warn("console-log-contract-warning");

    const CapturedBatch batch = Poll(cursor, 8);
    Expect(
        batch.result.dropped_count == 0 && batch.result.visited_count == 2 &&
            batch.result.next_sequence == cursor + 2 && batch.entries.size() == 2 &&
            batch.entries[0].sequence == cursor && batch.entries[0].level == spdlog::level::info &&
            batch.entries[0].message == "console-log-contract-info" &&
            batch.entries[1].sequence == cursor + 1 && batch.entries[1].level == spdlog::level::warn &&
            batch.entries[1].message == "console-log-contract-warning" &&
            external_sink->log_count.load() == 2,
        "a throwing extra sink blocked forwarded/default/later sink paths"
    );
    const std::size_t downstream_flush_count =
        original_counting_sink ? original_counting_sink->flush_count.load() : 0;
    installed_logger->flush();
    Expect(
        external_sink->flush_count.load() == 1 &&
            (!original_counting_sink ||
             original_counting_sink->flush_count.load() == downstream_flush_count + 1) &&
            source_error_count->load() == 3,
        "explicit flush or the preconfigured error handler was not preserved exactly once"
    );

    installed_logger->set_level(spdlog::level::err);
    installed_logger->flush_on(spdlog::level::critical);
    const std::shared_ptr<spdlog::logger> cloned_logger = installed_logger->clone("console-log-core-clone");
    Expect(
        cloned_logger && cloned_logger->name() == "console-log-core-clone" &&
            cloned_logger->sinks().size() == original_sink_count &&
            cloned_logger->level() == spdlog::level::err &&
            cloned_logger->flush_level() == spdlog::level::critical,
        "forwarding logger clone did not preserve current downstream logger behavior"
    );
    installed_logger->set_level(spdlog::level::trace);
    installed_logger->flush_on(spdlog::level::err);
}

void TestBacktraceDump(
    const std::shared_ptr<spdlog::logger>& _logger,
    const std::shared_ptr<CountingSink>&   _downstream_sink
) {
    const std::uint64_t cursor               = ClearAndGetNextSequence();
    const std::size_t   downstream_log_count = _downstream_sink->log_count.load();

    _logger->set_level(spdlog::level::warn);
    _logger->enable_backtrace(4);
    _logger->debug("console-log-backtrace-debug");
    _logger->dump_backtrace();
    _logger->disable_backtrace();
    _logger->set_level(spdlog::level::trace);

    const CapturedBatch batch           = Poll(cursor, 8);
    const bool          has_debug_entry = std::ranges::any_of(batch.entries, [](const CapturedEntry& _entry) {
        return _entry.level == spdlog::level::debug && _entry.message == "console-log-backtrace-debug";
    });
    Expect(
        batch.entries.size() == 3 && has_debug_entry &&
            _downstream_sink->log_count.load() == downstream_log_count + 3,
        "backtrace dump was filtered before reaching the console or downstream sinks"
    );
}

void TestVirtualLoggerBacktraceDispatch(std::shared_ptr<spdlog::logger>& _logger) {
    auto state         = std::make_shared<VirtualDispatchState>();
    auto sink          = std::make_shared<CountingSink>();
    auto source_logger = std::make_shared<VirtualDispatchLogger>("console-log-virtual-source", sink, state);
    source_logger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(source_logger);

    LogSystem::Init();
    const std::shared_ptr<spdlog::logger> installed_logger = spdlog::default_logger();
    Expect(
        installed_logger && installed_logger.get() != source_logger.get() && state->clone_count.load() == 1,
        "LogSystem::Init did not create a private virtual-dispatch clone"
    );

    const std::uint64_t cursor = ClearAndGetNextSequence();
    installed_logger->set_level(spdlog::level::warn);
    installed_logger->enable_backtrace(4);
    installed_logger->debug("console-log-virtual-backtrace");
    installed_logger->dump_backtrace();
    installed_logger->disable_backtrace();

    const CapturedBatch batch           = Poll(cursor, 8);
    const bool          has_debug_entry = std::ranges::any_of(batch.entries, [](const CapturedEntry& _entry) {
        return _entry.level == spdlog::level::debug && _entry.message == "console-log-virtual-backtrace";
    });
    Expect(
        batch.entries.size() == 3 && has_debug_entry && state->dispatch_count.load() == 3 &&
            sink->log_count.load() == 3,
        "backtrace replay bypassed the source logger's virtual dispatch path"
    );
    _logger = installed_logger;
}

void TestBacktraceThreadMetadata(std::shared_ptr<spdlog::logger>& _logger) {
    auto sink          = std::make_shared<ThreadRecordingSink>();
    auto source_logger = std::make_shared<spdlog::logger>("console-log-thread-metadata", sink);
    source_logger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(source_logger);

    LogSystem::Init();
    const std::shared_ptr<spdlog::logger> installed_logger = spdlog::default_logger();
    const std::uint64_t                   cursor           = ClearAndGetNextSequence();
    installed_logger->set_level(spdlog::level::warn);
    installed_logger->enable_backtrace(4);

    std::atomic<std::size_t> producer_thread_id{0};
    std::jthread             producer([installed_logger, &producer_thread_id] {
        producer_thread_id.store(spdlog::details::os::thread_id());
        installed_logger->debug("console-log-worker-backtrace");
    });
    producer.join();

    installed_logger->dump_backtrace();
    installed_logger->disable_backtrace();

    const CapturedBatch batch           = Poll(cursor, 8);
    const bool          has_debug_entry = std::ranges::any_of(batch.entries, [](const CapturedEntry& _entry) {
        return _entry.message == "console-log-worker-backtrace";
    });
    Expect(
        batch.entries.size() == 3 && has_debug_entry &&
            sink->FindThreadId("console-log-worker-backtrace") == producer_thread_id.load(),
        "backtrace forwarding replaced the producer thread metadata"
    );
    _logger = installed_logger;
}

void TestRuntimeFiltering(const std::shared_ptr<spdlog::logger>& _logger) {
    const std::uint64_t cursor = ClearAndGetNextSequence();
    _logger->set_level(spdlog::level::warn);
    spdlog::info("console-log-filtered-info");
    spdlog::warn("console-log-admitted-warning");
    _logger->set_level(spdlog::level::trace);

    const CapturedBatch batch = Poll(cursor, 8);
    Expect(
        batch.entries.size() == 1 && batch.result.visited_count == 1 &&
            batch.entries.front().level == spdlog::level::warn &&
            batch.entries.front().message == "console-log-admitted-warning",
        "console log channel bypassed the default logger's runtime level"
    );
}

void TestPagingAndClear(const std::shared_ptr<spdlog::logger>& _logger) {
    const std::uint64_t cursor = ClearAndGetNextSequence();
    _logger->info("console-log-page-a");
    _logger->info("console-log-page-b");
    _logger->info("console-log-page-c");

    const CapturedBatch first  = Poll(cursor, 2);
    const CapturedBatch second = Poll(first.result.next_sequence, 2);
    Expect(
        first.entries.size() == 2 && first.result.visited_count == 2 &&
            first.result.next_sequence == cursor + 2 && first.result.dropped_count == 0 &&
            second.entries.size() == 1 && second.entries.front().sequence == cursor + 2 &&
            second.result.next_sequence == cursor + 3 && second.result.dropped_count == 0,
        "console log cursor paging skipped or duplicated retained entries"
    );

    const std::uint64_t stale_cursor = ClearAndGetNextSequence();
    _logger->info("console-log-clear-a");
    _logger->info("console-log-clear-b");
    LogSystem::ClearConsoleLogs();
    const LogSystem::ConsoleLogPollResult cleared =
        LogSystem::VisitConsoleLogs(stale_cursor, 0, nullptr, nullptr);
    Expect(
        cleared.next_sequence == stale_cursor + 2 && cleared.dropped_count == 2 && cleared.visited_count == 0,
        "clearing an empty-visible ring did not advance a stale cursor across removed entries"
    );

    _logger->info("console-log-after-clear");
    const CapturedBatch after_clear = Poll(cleared.next_sequence, 4);
    Expect(
        after_clear.entries.size() == 1 && after_clear.entries.front().sequence == stale_cursor + 2 &&
            after_clear.result.dropped_count == 0,
        "ClearConsoleLogs reset sequence identity or lost the next entry"
    );
}

void TestBoundedOverwrite(const std::shared_ptr<spdlog::logger>& _logger) {
    constexpr std::size_t overflow_count = 17;
    constexpr std::size_t produced_count = ConsoleLogCapacity + overflow_count;
    const std::uint64_t   cursor         = ClearAndGetNextSequence();

    for (std::size_t index = 0; index < produced_count; ++index) {
        _logger->info("console-log-overflow-{}", index);
    }

    const CapturedBatch batch = Poll(cursor, produced_count);
    Expect(
        batch.entries.size() == ConsoleLogCapacity && batch.result.visited_count == ConsoleLogCapacity &&
            batch.result.dropped_count == overflow_count &&
            batch.entries.front().sequence == cursor + overflow_count &&
            batch.entries.back().sequence == cursor + produced_count - 1 &&
            batch.result.next_sequence == cursor + produced_count,
        "bounded console log ring did not report overwritten cursor entries"
    );
}

void TestConcurrentProducers(const std::shared_ptr<spdlog::logger>& _logger) {
    constexpr std::size_t producer_count     = 8;
    constexpr std::size_t entries_per_thread = 128;
    constexpr std::size_t expected_count     = producer_count * entries_per_thread;
    const std::uint64_t   cursor             = ClearAndGetNextSequence();

    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([_logger, producer] {
            for (std::size_t index = 0; index < entries_per_thread; ++index) {
                _logger->info("console-log-concurrent-{}-{}", producer, index);
            }
        });
    }
    producers.clear();

    const CapturedBatch             batch = Poll(cursor, expected_count);
    std::unordered_set<std::string> unique_messages;
    unique_messages.reserve(expected_count);
    bool sequences_are_contiguous = batch.entries.size() == expected_count;
    for (std::size_t index = 0; index < batch.entries.size(); ++index) {
        sequences_are_contiguous =
            sequences_are_contiguous && batch.entries[index].sequence == cursor + index;
        unique_messages.insert(batch.entries[index].message);
    }
    Expect(
        batch.result.dropped_count == 0 && batch.result.visited_count == expected_count &&
            batch.result.next_sequence == cursor + expected_count && sequences_are_contiguous &&
            unique_messages.size() == expected_count,
        "concurrent console log producers lost, duplicated, or mis-sequenced entries"
    );
}

struct ReentrantVisitorContext {
    std::shared_ptr<spdlog::logger> logger;
    std::vector<CapturedEntry>      entries;
    std::uint64_t                   new_entry_sequence = 0;
    LogSystem::ConsoleLogPollResult nested_result;
    bool                            reentered = false;
};

void ReentrantVisitor(const LogSystem::ConsoleLogEntryView& _entry, void* _context) {
    auto& context = *static_cast<ReentrantVisitorContext*>(_context);
    context.entries.push_back({
        .sequence = _entry.sequence,
        .level    = _entry.level,
        .message  = std::string(_entry.message),
    });
    if (context.reentered) {
        return;
    }

    context.reentered = true;
    LogSystem::ClearConsoleLogs();
    context.logger->warn("console-log-reentrant-new");
    context.nested_result = LogSystem::VisitConsoleLogs(context.new_entry_sequence, 0, nullptr, nullptr);
}

void TestVisitorReentry(const std::shared_ptr<spdlog::logger>& _logger) {
    const std::uint64_t cursor = ClearAndGetNextSequence();
    _logger->info("console-log-reentrant-a");
    _logger->info("console-log-reentrant-b");

    ReentrantVisitorContext context{
        .logger             = _logger,
        .new_entry_sequence = cursor + 2,
    };
    const LogSystem::ConsoleLogPollResult outer =
        LogSystem::VisitConsoleLogs(cursor, 2, &ReentrantVisitor, &context);
    Expect(
        outer.visited_count == 2 && outer.next_sequence == cursor + 2 && context.reentered &&
            context.entries.size() == 2 && context.entries[0].message == "console-log-reentrant-a" &&
            context.entries[1].message == "console-log-reentrant-b" &&
            context.nested_result.next_sequence == cursor + 2 && context.nested_result.dropped_count == 0,
        "console log visitor ran under the channel lock or lost its copied snapshot during reentry"
    );

    const CapturedBatch nested_entry = Poll(outer.next_sequence, 4);
    Expect(
        nested_entry.entries.size() == 1 && nested_entry.entries.front().sequence == cursor + 2 &&
            nested_entry.entries.front().message == "console-log-reentrant-new",
        "reentrant logging was not retained for the next cursor poll"
    );
}

void TestSpdlogTeardownAndReattach(std::shared_ptr<spdlog::logger>& _logger) {
    const std::uint64_t cursor = ClearAndGetNextSequence();
    _logger->warn("console-log-before-spdlog-shutdown");
    spdlog::shutdown();
    _logger.reset();

    const CapturedBatch retained = Poll(cursor, 4);
    Expect(
        retained.entries.size() == 1 &&
            retained.entries.front().message == "console-log-before-spdlog-shutdown",
        "spdlog logger teardown destroyed retained console log channel state"
    );

    auto replacement_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto replacement_logger =
        std::make_shared<spdlog::logger>("console-log-core-replacement", replacement_sink);
    replacement_logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(replacement_logger);
    LogSystem::Init();
    const std::shared_ptr<spdlog::logger> installed_replacement = spdlog::default_logger();
    LogSystem::Init();
    Expect(
        installed_replacement && installed_replacement.get() != replacement_logger.get() &&
            spdlog::default_logger().get() == installed_replacement.get() &&
            installed_replacement->sinks().empty() && replacement_logger->sinks().size() == 1,
        "LogSystem::Init did not install exactly one ABI-safe forwarding logger after replacement"
    );

    const std::uint64_t replacement_cursor = ClearAndGetNextSequence();
    installed_replacement->error("console-log-after-reattach");
    const CapturedBatch replacement_batch = Poll(replacement_cursor, 4);
    Expect(
        replacement_batch.entries.size() == 1 &&
            replacement_batch.entries.front().message == "console-log-after-reattach",
        "replacement default logger did not feed the existing console log channel"
    );
    _logger = installed_replacement;
}

} // namespace

int main() {
    DefaultLoggerRestore restore_default_logger;

    const LogSystem::ConsoleLogPollResult initial = LogSystem::VisitConsoleLogs(0, 0, nullptr, nullptr);
    Expect(
        initial.next_sequence == 1 && initial.dropped_count == 0 && initial.visited_count == 0,
        "empty console log channel did not normalize its initial cursor"
    );

    LogSystem::Init();
    const std::uint64_t process_default_cursor = ClearAndGetNextSequence();
    LOG_INFO("console-log-process-default-forwarding");
    const CapturedBatch process_default_batch = Poll(process_default_cursor, 2);
    Expect(
        process_default_batch.entries.size() == 1 &&
            process_default_batch.entries.front().message == "console-log-process-default-forwarding",
        "the process default logger did not forward into the console channel"
    );

    auto                            original_sink = std::make_shared<CountingSink>();
    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>("console-log-core-contract", original_sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    try {
        TestInstallationAndCapture(logger, original_sink);
        TestBacktraceDump(logger, original_sink);
        TestRuntimeFiltering(logger);
        TestPagingAndClear(logger);
        TestBoundedOverwrite(logger);
        TestConcurrentProducers(logger);
        TestVisitorReentry(logger);
        TestVirtualLoggerBacktraceDispatch(logger);
        TestBacktraceThreadMetadata(logger);
        TestSpdlogTeardownAndReattach(logger);
        std::cout << "Console log core contract tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Console log core contract test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
