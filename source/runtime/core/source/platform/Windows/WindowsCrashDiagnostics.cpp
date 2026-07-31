#include "WindowsCrashDiagnostics.h"

#include "PlatformWindows.h"
#include "WindowsCrashDiagnosticsTesting.h"

#include <DbgHelp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <process.h>
#include <string_view>

namespace {

constexpr std::size_t kCrashWidePathCapacity = 4096;
constexpr std::size_t kCrashMetadataCapacity = 16 * 1024;

enum class ECrashRequestState : std::uint32_t {
    Idle,
    Reserved,
    Pending,
    Processing,
    Completed,
};

template<std::size_t Capacity>
struct StoredText {
    std::array<char, Capacity> bytes{};
    std::uint32_t              size{0};
    bool                       truncated{false};

    void Assign(std::string_view _source) noexcept {
        const std::size_t stored = std::min(_source.size(), bytes.size() - 1);
        if (stored != 0) {
            std::memcpy(bytes.data(), _source.data(), stored);
        }
        bytes[stored] = '\0';
        size          = static_cast<std::uint32_t>(stored);
        truncated     = stored != _source.size();
    }

    [[nodiscard]] std::string_view View() const noexcept {
        return std::string_view(bytes.data(), size);
    }
};

struct StoredCrashRequest {
    StoredText<32>     failure_kind{};
    StoredText<512>    expression{};
    StoredText<1024>   file{};
    StoredText<256>    function{};
    StoredText<1024>   message{};
    StoredText<64>     profile_flush_status{};
    std::uint32_t      line{0};
    std::uint32_t      thread_id{0};
    bool               message_truncated{false};
    PlatformStackTrace stack{};

    void Assign(const PlatformCrashArtifactRequest& _source) noexcept {
        failure_kind.Assign(_source.failure_kind);
        expression.Assign(_source.expression);
        file.Assign(_source.file);
        function.Assign(_source.function);
        message.Assign(_source.message);
        profile_flush_status.Assign(_source.profile_flush_status);
        line              = _source.line;
        thread_id         = _source.thread_id;
        message_truncated = _source.message_truncated || message.truncated;
        stack             = _source.stack;
    }
};

struct FixedTextWriter {
    char*       data{nullptr};
    std::size_t capacity{0};
    std::size_t size{0};

    void Reset() noexcept {
        size = 0;
        if (data && capacity != 0) {
            data[0] = '\0';
        }
    }

    void Append(std::string_view _text) noexcept {
        if (!data || capacity == 0 || size >= capacity - 1) {
            return;
        }
        const std::size_t writable = std::min(_text.size(), capacity - size - 1);
        if (writable != 0) {
            std::memcpy(data + size, _text.data(), writable);
            size += writable;
        }
        data[size] = '\0';
    }

    void AppendFormat(const char* _format, ...) noexcept {
        if (!data || !_format || capacity == 0 || size >= capacity - 1) {
            return;
        }
        va_list args;
        va_start(args, _format);
        const int written = std::vsnprintf(data + size, capacity - size, _format, args);
        va_end(args);
        if (written <= 0) {
            return;
        }
        const std::size_t available = capacity - size - 1;
        size += std::min<std::size_t>(static_cast<std::size_t>(written), available);
        data[size] = '\0';
    }
};

struct CrashArtifactService {
    std::atomic<bool>               ready{false};
    std::atomic<ECrashRequestState> request_state{ECrashRequestState::Idle};
    std::atomic<std::uint64_t>      nonce{1};
    std::atomic<bool>               test_pause_before_dump{false};

    HANDLE request_event{nullptr};
    HANDLE completion_event{nullptr};
    HANDLE worker_thread{nullptr};
    DWORD  initialization_error{ERROR_SERVICE_NOT_ACTIVE};

    std::array<wchar_t, kCrashWidePathCapacity> crash_directory{};
    std::array<wchar_t, kCrashWidePathCapacity> metadata_path{};
    std::array<wchar_t, kCrashWidePathCapacity> dump_path{};
    std::array<char, kCrashMetadataCapacity>    metadata_buffer{};
    std::array<char, 512>                       debugger_buffer{};

    StoredCrashRequest          request{};
    PlatformCrashArtifactResult result{};
};

INIT_ONCE            g_crash_init_once = INIT_ONCE_STATIC_INIT;
CrashArtifactService g_crash_service{};

bool EnsureDirectory(const wchar_t* _path, DWORD& _error) noexcept {
    if (::CreateDirectoryW(_path, nullptr) != FALSE) {
        return true;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = ::GetFileAttributesW(_path);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return true;
        }
    }
    _error = error;
    return false;
}

bool AppendWidePath(wchar_t* _destination, std::size_t _capacity, std::wstring_view _suffix) noexcept {
    if (!_destination || _capacity == 0) {
        return false;
    }
    const std::size_t current = std::wcslen(_destination);
    if (current + _suffix.size() + 1 > _capacity) {
        return false;
    }
    std::wmemcpy(_destination + current, _suffix.data(), _suffix.size());
    _destination[current + _suffix.size()] = L'\0';
    return true;
}

bool WidePathToUtf8(
    const wchar_t*                                _path,
    std::array<char, kPlatformCrashPathCapacity>& _destination
) noexcept {
    const int converted = ::WideCharToMultiByte(
        CP_UTF8, 0, _path, -1, _destination.data(), static_cast<int>(_destination.size()), nullptr, nullptr
    );
    if (converted <= 0) {
        _destination[0] = '\0';
        return false;
    }
    return true;
}

bool WriteAll(HANDLE _file, const char* _data, std::size_t _size) noexcept {
    std::size_t offset = 0;
    while (offset < _size) {
        const DWORD batch   = static_cast<DWORD>(std::min<std::size_t>(_size - offset, MAXDWORD));
        DWORD       written = 0;
        if (::WriteFile(_file, _data + offset, batch, &written, nullptr) == FALSE || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

std::string_view SafeText(std::string_view _text) noexcept {
    return _text.empty() ? std::string_view("<empty>") : _text;
}

void AppendMetadataText(FixedTextWriter& _writer, std::string_view _key, std::string_view _value) noexcept {
    _writer.Append(_key);
    for (const char value : SafeText(_value)) {
        switch (value) {
            case '\\':
                _writer.Append("\\\\");
                break;
            case '\r':
                _writer.Append("\\r");
                break;
            case '\n':
                _writer.Append("\\n");
                break;
            case '\0':
                _writer.Append("\\0");
                break;
            default:
                _writer.Append(std::string_view(&value, 1));
                break;
        }
    }
    _writer.Append("\n");
}

bool CreateUniqueArtifactPair(
    CrashArtifactService& _service,
    HANDLE&               _metadata_file,
    HANDLE&               _dump_file,
    std::uint32_t&        _metadata_error,
    std::uint32_t&        _dump_error
) noexcept {
    SYSTEMTIME time{};
    ::GetSystemTime(&time);
    const DWORD process_id = ::GetCurrentProcessId();
    const DWORD thread_id =
        _service.request.thread_id != 0 ? _service.request.thread_id : ::GetCurrentThreadId();

    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
        const std::uint64_t nonce          = _service.nonce.fetch_add(1, std::memory_order_relaxed);
        const int           metadata_count = _snwprintf_s(
            _service.metadata_path.data(),
            _service.metadata_path.size(),
            _TRUNCATE,
            L"%ls\\moer_controlled_fatal_%04u%02u%02u_%02u%02u%02u_%03u_"
            L"pid%lu_tid%lu_%llu.txt",
            _service.crash_directory.data(),
            static_cast<unsigned>(time.wYear),
            static_cast<unsigned>(time.wMonth),
            static_cast<unsigned>(time.wDay),
            static_cast<unsigned>(time.wHour),
            static_cast<unsigned>(time.wMinute),
            static_cast<unsigned>(time.wSecond),
            static_cast<unsigned>(time.wMilliseconds),
            static_cast<unsigned long>(process_id),
            static_cast<unsigned long>(thread_id),
            static_cast<unsigned long long>(nonce)
        );
        const int dump_count = _snwprintf_s(
            _service.dump_path.data(),
            _service.dump_path.size(),
            _TRUNCATE,
            L"%ls\\moer_controlled_fatal_%04u%02u%02u_%02u%02u%02u_%03u_"
            L"pid%lu_tid%lu_%llu.dmp",
            _service.crash_directory.data(),
            static_cast<unsigned>(time.wYear),
            static_cast<unsigned>(time.wMonth),
            static_cast<unsigned>(time.wDay),
            static_cast<unsigned>(time.wHour),
            static_cast<unsigned>(time.wMinute),
            static_cast<unsigned>(time.wSecond),
            static_cast<unsigned>(time.wMilliseconds),
            static_cast<unsigned long>(process_id),
            static_cast<unsigned long>(thread_id),
            static_cast<unsigned long long>(nonce)
        );
        if (metadata_count < 0 || dump_count < 0) {
            _metadata_error = ERROR_INSUFFICIENT_BUFFER;
            _dump_error     = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }

        _metadata_file = ::CreateFileW(
            _service.metadata_path.data(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (_metadata_file == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            _metadata_error = error;
            _dump_error     = error;
            return false;
        }

        _dump_file = ::CreateFileW(
            _service.dump_path.data(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (_dump_file != INVALID_HANDLE_VALUE) {
            return true;
        }

        _dump_error = ::GetLastError();
        if (_dump_error != ERROR_FILE_EXISTS && _dump_error != ERROR_ALREADY_EXISTS) {
            return true;
        }

        ::CloseHandle(_metadata_file);
        _metadata_file = INVALID_HANDLE_VALUE;
        static_cast<void>(::DeleteFileW(_service.metadata_path.data()));
    }

    _metadata_error = ERROR_ALREADY_EXISTS;
    _dump_error     = ERROR_ALREADY_EXISTS;
    return false;
}

void WritePreDumpMetadata(
    CrashArtifactService&        _service,
    HANDLE                       _metadata_file,
    PlatformCrashArtifactResult& _result
) noexcept {
    FixedTextWriter writer{
        .data     = _service.metadata_buffer.data(),
        .capacity = _service.metadata_buffer.size(),
    };
    writer.Append("artifact_kind=controlled_fatal_snapshot\n");
    writer.Append("artifact_phase=pre_dump\n");
    writer.Append("exception_context=none\n");
    AppendMetadataText(writer, "failure_kind=", _service.request.failure_kind.View());
    AppendMetadataText(writer, "expression=", _service.request.expression.View());
    AppendMetadataText(writer, "file=", _service.request.file.View());
    writer.AppendFormat("line=%u\n", _service.request.line);
    AppendMetadataText(writer, "function=", _service.request.function.View());
    writer.AppendFormat("thread_id=%u\n", _service.request.thread_id);
    AppendMetadataText(writer, "message=", _service.request.message.View());
    writer.AppendFormat("message_truncated=%u\n", _service.request.message_truncated ? 1U : 0U);
    AppendMetadataText(writer, "profile_flush=", _service.request.profile_flush_status.View());
    AppendMetadataText(
        writer,
        "dump_path=",
        std::string_view(_result.dump_path.data(), std::char_traits<char>::length(_result.dump_path.data()))
    );
    writer.AppendFormat("stack_count=%u\n", _service.request.stack.frame_count);
    const std::uint32_t stack_count = std::min<std::uint32_t>(
        _service.request.stack.frame_count, static_cast<std::uint32_t>(_service.request.stack.frames.size())
    );
    for (std::uint32_t index = 0; index < stack_count; ++index) {
        writer.AppendFormat(
            "stack[%u]=0x%llx\n", index, static_cast<unsigned long long>(_service.request.stack.frames[index])
        );
    }

    if (!WriteAll(_metadata_file, _service.metadata_buffer.data(), writer.size)) {
        _result.metadata_error = ::GetLastError();
        if (_result.metadata_error == ERROR_SUCCESS) {
            _result.metadata_error = ERROR_WRITE_FAULT;
        }
        return;
    }
    if (::FlushFileBuffers(_metadata_file) == FALSE) {
        _result.metadata_error = ::GetLastError();
        return;
    }
    _result.metadata_written = true;
}

void AppendCompletionMetadata(
    CrashArtifactService&        _service,
    HANDLE                       _metadata_file,
    PlatformCrashArtifactResult& _result
) noexcept {
    FixedTextWriter writer{
        .data     = _service.metadata_buffer.data(),
        .capacity = _service.metadata_buffer.size(),
    };
    writer.Append("artifact_phase=complete\n");
    writer.AppendFormat("dump_created=%u\n", _result.dump_created ? 1U : 0U);
    writer.AppendFormat("dump_written=%u\n", _result.dump_written ? 1U : 0U);
    writer.AppendFormat("dump_flushed=%u\n", _result.dump_flushed ? 1U : 0U);
    writer.AppendFormat("dump_error=%u\n", _result.dump_error);
    if (!WriteAll(_metadata_file, _service.metadata_buffer.data(), writer.size)) {
        if (_result.metadata_error == ERROR_SUCCESS) {
            _result.metadata_error = ::GetLastError();
            if (_result.metadata_error == ERROR_SUCCESS) {
                _result.metadata_error = ERROR_WRITE_FAULT;
            }
        }
        return;
    }
    if (::FlushFileBuffers(_metadata_file) == FALSE) {
        if (_result.metadata_error == ERROR_SUCCESS) {
            _result.metadata_error = ::GetLastError();
        }
        return;
    }
    _result.metadata_completed = true;
}

void ProcessCrashRequest(CrashArtifactService& _service) noexcept {
    PlatformCrashArtifactResult result{};
    HANDLE                      metadata_file = INVALID_HANDLE_VALUE;
    HANDLE                      dump_file     = INVALID_HANDLE_VALUE;

    if (!CreateUniqueArtifactPair(
            _service, metadata_file, dump_file, result.metadata_error, result.dump_error
        )) {
        _service.result = result;
        return;
    }

    static_cast<void>(WidePathToUtf8(_service.metadata_path.data(), result.metadata_path));
    static_cast<void>(WidePathToUtf8(_service.dump_path.data(), result.dump_path));

    WritePreDumpMetadata(_service, metadata_file, result);

    while (_service.test_pause_before_dump.load(std::memory_order_acquire)) {
        ::SwitchToThread();
    }

    if (dump_file != INVALID_HANDLE_VALUE) {
        result.dump_created           = true;
        const MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo
        );
        if (::MiniDumpWriteDump(
                ::GetCurrentProcess(),
                ::GetCurrentProcessId(),
                dump_file,
                dump_type,
                nullptr,
                nullptr,
                nullptr
            ) != FALSE) {
            result.dump_written = true;
            if (::FlushFileBuffers(dump_file) != FALSE) {
                result.dump_flushed = true;
            } else {
                result.dump_error = ::GetLastError();
            }
        } else {
            result.dump_error = ::GetLastError();
        }
        ::CloseHandle(dump_file);
    }

    AppendCompletionMetadata(_service, metadata_file, result);
    ::CloseHandle(metadata_file);
    result.request_completed = true;

    const std::string_view profile_flush_status = SafeText(_service.request.profile_flush_status.View());
    const int              debugger_count       = std::snprintf(
        _service.debugger_buffer.data(),
        _service.debugger_buffer.size(),
        "[MOER_FATAL] metadata=%s dump=%s profile_flush=%.*s\n",
        result.metadata_path.data(),
        result.dump_written ? result.dump_path.data() : "<failed>",
        static_cast<int>(std::min<std::size_t>(profile_flush_status.size(), 64)),
        profile_flush_status.data()
    );
    if (debugger_count > 0) {
        ::OutputDebugStringA(_service.debugger_buffer.data());
    }
    _service.result = result;
}

unsigned __stdcall CrashArtifactWorker(void* _context) noexcept {
    auto& service = *static_cast<CrashArtifactService*>(_context);
    for (;;) {
        if (::WaitForSingleObject(service.request_event, INFINITE) != WAIT_OBJECT_0) {
            continue;
        }
        ECrashRequestState expected = ECrashRequestState::Pending;
        if (!service.request_state.compare_exchange_strong(
                expected, ECrashRequestState::Processing, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
            continue;
        }
        ProcessCrashRequest(service);
        service.request_state.store(ECrashRequestState::Completed, std::memory_order_release);
        static_cast<void>(::SetEvent(service.completion_event));
    }
}

BOOL CALLBACK InitializeCrashService(PINIT_ONCE, PVOID, PVOID*) noexcept {
    auto& service = g_crash_service;
    DWORD error   = ERROR_SUCCESS;

    const DWORD directory_length = ::GetCurrentDirectoryW(
        static_cast<DWORD>(service.crash_directory.size()), service.crash_directory.data()
    );
    if (directory_length == 0 || directory_length >= service.crash_directory.size()) {
        service.initialization_error = ::GetLastError();
        return TRUE;
    }
    if (!AppendWidePath(service.crash_directory.data(), service.crash_directory.size(), L"\\logs") ||
        !EnsureDirectory(service.crash_directory.data(), error) ||
        !AppendWidePath(service.crash_directory.data(), service.crash_directory.size(), L"\\crash") ||
        !EnsureDirectory(service.crash_directory.data(), error)) {
        service.initialization_error = error != ERROR_SUCCESS ? error : ERROR_INSUFFICIENT_BUFFER;
        return TRUE;
    }

    service.request_event    = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    service.completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!service.request_event || !service.completion_event) {
        service.initialization_error = ::GetLastError();
        return TRUE;
    }

    service.worker_thread = reinterpret_cast<HANDLE>(
        ::_beginthreadex(nullptr, 256 * 1024, &CrashArtifactWorker, &service, 0, nullptr)
    );
    if (!service.worker_thread) {
        service.initialization_error = ::GetLastError();
        return TRUE;
    }

    service.initialization_error = ERROR_SUCCESS;
    service.ready.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

bool InitializeWindowsCrashDiagnostics() noexcept {
    static_cast<void>(::InitOnceExecuteOnce(&g_crash_init_once, &InitializeCrashService, nullptr, nullptr));
    return g_crash_service.ready.load(std::memory_order_acquire);
}

PlatformCrashArtifactResult SubmitWindowsCrashArtifacts(
    const PlatformCrashArtifactRequest& _request,
    std::uint32_t                       _timeout_ms
) noexcept {
    auto&                       service = g_crash_service;
    PlatformCrashArtifactResult unavailable{};
    if (!service.ready.load(std::memory_order_acquire)) {
        unavailable.metadata_error = service.initialization_error;
        unavailable.dump_error     = service.initialization_error;
        return unavailable;
    }

    ECrashRequestState expected = ECrashRequestState::Idle;
    if (!service.request_state.compare_exchange_strong(
            expected, ECrashRequestState::Reserved, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        unavailable.metadata_error = ERROR_BUSY;
        unavailable.dump_error     = ERROR_BUSY;
        return unavailable;
    }

    if (::ResetEvent(service.completion_event) == FALSE) {
        const DWORD error          = ::GetLastError();
        unavailable.metadata_error = error;
        unavailable.dump_error     = error;
        service.request_state.store(ECrashRequestState::Idle, std::memory_order_release);
        return unavailable;
    }
    service.request.Assign(_request);
    service.result = {};
    service.request_state.store(ECrashRequestState::Pending, std::memory_order_release);
    if (::SetEvent(service.request_event) == FALSE) {
        const DWORD error          = ::GetLastError();
        unavailable.metadata_error = error;
        unavailable.dump_error     = error;
        service.request_state.store(ECrashRequestState::Idle, std::memory_order_release);
        return unavailable;
    }

    const DWORD wait_result = ::WaitForSingleObject(service.completion_event, _timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
        unavailable.timed_out      = wait_result == WAIT_TIMEOUT;
        const DWORD error          = wait_result == WAIT_TIMEOUT ? WAIT_TIMEOUT : ::GetLastError();
        unavailable.metadata_error = error;
        unavailable.dump_error     = error;
        return unavailable;
    }

    if (service.request_state.load(std::memory_order_acquire) != ECrashRequestState::Completed) {
        unavailable.metadata_error = ERROR_INVALID_STATE;
        unavailable.dump_error     = ERROR_INVALID_STATE;
        return unavailable;
    }
    PlatformCrashArtifactResult result = service.result;
    service.request_state.store(ECrashRequestState::Idle, std::memory_order_release);
    return result;
}

namespace Moer::PlatformTesting {

bool ConfigureCrashWorkerPauseBeforeDump(bool _enabled) noexcept {
    auto& service = g_crash_service;
    if (service.request_state.load(std::memory_order_acquire) != ECrashRequestState::Idle) {
        return false;
    }
    service.test_pause_before_dump.store(_enabled, std::memory_order_release);
    return true;
}

} // namespace Moer::PlatformTesting
