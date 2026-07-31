#pragma once

#include "API_Macro.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Moer::CVar {

enum class EType : std::uint8_t {
    Bool,
    Int,
    Float,
    String,
};

enum class EFlags : std::uint32_t {
    None        = 0,
    ReadOnly    = 1u << 0,
    StartupOnly = 1u << 1,
};

constexpr EFlags operator|(EFlags _lhs, EFlags _rhs) noexcept {
    return static_cast<EFlags>(static_cast<std::uint32_t>(_lhs) | static_cast<std::uint32_t>(_rhs));
}

constexpr bool HasFlag(EFlags _flags, EFlags _flag) noexcept {
    return (static_cast<std::uint32_t>(_flags) & static_cast<std::uint32_t>(_flag)) != 0;
}

inline constexpr std::size_t DefaultCallbackDispatchBudget = 64;
inline constexpr std::size_t MaxCallbackDispatchBudget     = 4096;

enum class ESetSource : std::uint8_t {
    Runtime,
    StartupConfig,
};

enum class ESetStatus : std::uint8_t {
    Changed,
    Unchanged,
    NotFound,
    TypeMismatch,
    OutOfRange,
    ReadOnly,
    StartupSealed,
    CallbackQueueFull,
    InvalidRegistration,
};

struct CVarSetResult {
    ESetStatus  status            = ESetStatus::NotFound;
    bool        callback_failed   = false;
    bool        callback_deferred = false;
    bool        callback_canceled = false;
    const char* detail            = nullptr;

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == ESetStatus::Changed || status == ESetStatus::Unchanged;
    }
};

struct CVarDescriptor {
    std::string           name;
    std::string           helper;
    std::string           true_helper;
    std::string           false_helper;
    EFlags                flags = EFlags::None;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::size_t           callback_dispatch_budget = DefaultCallbackDispatchBudget;
};

struct CVarDescriptorView {
    std::string_view      name;
    std::string_view      helper;
    std::string_view      true_helper;
    std::string_view      false_helper;
    EFlags                flags = EFlags::None;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::size_t           callback_dispatch_budget = DefaultCallbackDispatchBudget;
};

struct CVarSnapshotView {
    std::uint64_t         id = 0;
    std::string_view      name;
    std::string_view      helper;
    std::string_view      true_helper;
    std::string_view      false_helper;
    std::string_view      value;
    EType                 type  = EType::String;
    EFlags                flags = EFlags::None;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::size_t           callback_dispatch_budget = DefaultCallbackDispatchBudget;
    bool                  startup_sealed           = false;
};

struct CVarSnapshot {
    std::uint64_t         id = 0;
    std::string           name;
    std::string           helper;
    std::string           true_helper;
    std::string           false_helper;
    std::string           value;
    EType                 type  = EType::String;
    EFlags                flags = EFlags::None;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::size_t           callback_dispatch_budget = DefaultCallbackDispatchBudget;
    bool                  startup_sealed           = false;
};

enum class ERegistrationStatus : std::uint8_t {
    Registered,
    InvalidDescriptor,
    DuplicateName,
};

class Registration;

namespace Detail {
struct RegistrationAccess {
    static Registration Create(std::uint64_t _id) noexcept;
};
} // namespace Detail

// Each cvar serializes its own commits and callback FIFO. User callbacks and
// caller-side destroy thunks run without registry or entry locks. A concurrent
// mutation may commit while an earlier callback is running; it returns with
// callback_deferred instead of waiting. The active dispatcher considers that
// callback later in commit order, but Reset may cancel it first. A deferred
// result has no completion query in this phase, so its eventual callback
// success/failure/cancellation is intentionally not reported to that caller.
// Each dispatch epoch admits at most callback_dispatch_budget events; further
// callback-producing mutations fail with CallbackQueueFull without committing
// their value. Callbacks may re-enter the registry.
class Registration {
public:
    Registration() noexcept = default;
    CORE_API ~Registration();

    Registration(const Registration&)                     = delete;
    Registration&          operator=(const Registration&) = delete;
    CORE_API               Registration(Registration&& _other) noexcept;
    CORE_API Registration& operator=(Registration&& _other) noexcept;

    [[nodiscard]] bool IsValid() const noexcept {
        return id != 0;
    }
    [[nodiscard]] std::uint64_t GetId() const noexcept {
        return id;
    }
    [[nodiscard]] std::optional<CVarSnapshot> GetSnapshot() const;

    CORE_API CVarSetResult SynchronizeOwnerValue(bool _value);
    CORE_API CVarSetResult SynchronizeOwnerValue(std::int64_t _value);
    CORE_API CVarSetResult SynchronizeOwnerValue(double _value);
    CORE_API CVarSetResult SynchronizeOwnerValue(std::string_view _value);

    // Reset removes the name first and retires the caller callback thunk.
    // Calls made outside cvar callbacks, callback-capture destruction, and
    // snapshot visitors wait for in-flight work. Reset from any of those
    // user-code chains is non-blocking to avoid cross-entry wait cycles; the
    // last active dispatcher retires the thunk after returning from user code.
    // Snapshot visitors are not in-flight operations after capture: Reset does
    // not wait for them, and a visitor may finish with an old id/value after a
    // same-name generation is registered.
    CORE_API void Reset() noexcept;

private:
    explicit Registration(std::uint64_t _id) noexcept;

    std::uint64_t id = 0;

    friend struct Detail::RegistrationAccess;
};

struct RegistrationResult {
    ERegistrationStatus status = ERegistrationStatus::InvalidDescriptor;
    Registration        registration;
    const char*         detail = nullptr;

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == ERegistrationStatus::Registered && registration.IsValid();
    }
};

// Every string_view in CVarSnapshotView is valid only for the duration of the
// visitor call. Copy it inside the callback; never retain the view. The visitor
// runs after snapshot capture has released its active Entry operation, so Reset
// may complete concurrently while the immutable old snapshot remains valid.
// Reset called from visitor code is non-blocking with respect to other active
// Entry operations to avoid user-code wait cycles.
using SnapshotVisitor = void (*)(const CVarSnapshotView& _snapshot, void* _context);

namespace Detail {

using DestroyCallback = void (*)(void* _context) noexcept;
using BoolCallback    = bool (*)(void* _context, bool _old_value, bool _new_value) noexcept;
using IntCallback     = bool (*)(void* _context, std::int64_t _old_value, std::int64_t _new_value) noexcept;
using FloatCallback   = bool (*)(void* _context, double _old_value, double _new_value) noexcept;
using StringCallback =
    bool (*)(void* _context, std::string_view _old_value, std::string_view _new_value) noexcept;

// Raw registration takes ownership after validating a complete callback
// binding, including later invalid/duplicate/error paths. An incomplete
// binding is rejected without taking ownership; a valid binding is either
// entirely empty or provides callback, context, and destroy together.
CORE_API RegistrationResult RegisterBoolRaw(
    CVarDescriptorView _descriptor,
    bool               _default_value,
    BoolCallback       _callback,
    void*              _context,
    DestroyCallback    _destroy
);
CORE_API RegistrationResult RegisterIntRaw(
    CVarDescriptorView _descriptor,
    std::int64_t       _default_value,
    IntCallback        _callback,
    void*              _context,
    DestroyCallback    _destroy
);
CORE_API RegistrationResult RegisterFloatRaw(
    CVarDescriptorView _descriptor,
    double             _default_value,
    FloatCallback      _callback,
    void*              _context,
    DestroyCallback    _destroy
);
CORE_API RegistrationResult RegisterStringRaw(
    CVarDescriptorView _descriptor,
    std::string_view   _default_value,
    StringCallback     _callback,
    void*              _context,
    DestroyCallback    _destroy
);

CORE_API bool VisitSnapshotRaw(std::string_view _name, SnapshotVisitor _visitor, void* _context);
CORE_API bool VisitSnapshotByIdRaw(std::uint64_t _id, SnapshotVisitor _visitor, void* _context);
CORE_API std::size_t VisitAllSnapshotsRaw(std::string_view _prefix, SnapshotVisitor _visitor, void* _context);

inline CVarDescriptorView ViewOf(const CVarDescriptor& _descriptor) noexcept {
    return {
        .name                     = _descriptor.name,
        .helper                   = _descriptor.helper,
        .true_helper              = _descriptor.true_helper,
        .false_helper             = _descriptor.false_helper,
        .flags                    = _descriptor.flags,
        .min_value                = _descriptor.min_value,
        .max_value                = _descriptor.max_value,
        .callback_dispatch_budget = _descriptor.callback_dispatch_budget,
    };
}

template<typename Callback>
void DestroyOwnedCallback(void* _context) noexcept {
    delete static_cast<Callback*>(_context);
}

template<typename Callback>
bool InvokeBoolCallback(void* _context, bool _old_value, bool _new_value) noexcept {
    try {
        std::invoke(*static_cast<Callback*>(_context), _old_value, _new_value);
        return true;
    } catch (...) {
        return false;
    }
}

template<typename Callback>
bool InvokeIntCallback(void* _context, std::int64_t _old_value, std::int64_t _new_value) noexcept {
    try {
        std::invoke(*static_cast<Callback*>(_context), _old_value, _new_value);
        return true;
    } catch (...) {
        return false;
    }
}

template<typename Callback>
bool InvokeFloatCallback(void* _context, double _old_value, double _new_value) noexcept {
    try {
        std::invoke(*static_cast<Callback*>(_context), _old_value, _new_value);
        return true;
    } catch (...) {
        return false;
    }
}

template<typename Callback>
bool InvokeStringCallback(void* _context, std::string_view _old_value, std::string_view _new_value) noexcept {
    try {
        std::invoke(*static_cast<Callback*>(_context), _old_value, _new_value);
        return true;
    } catch (...) {
        return false;
    }
}

inline CVarSnapshot CopySnapshot(const CVarSnapshotView& _view) {
    return {
        .id                       = _view.id,
        .name                     = std::string(_view.name),
        .helper                   = std::string(_view.helper),
        .true_helper              = std::string(_view.true_helper),
        .false_helper             = std::string(_view.false_helper),
        .value                    = std::string(_view.value),
        .type                     = _view.type,
        .flags                    = _view.flags,
        .min_value                = _view.min_value,
        .max_value                = _view.max_value,
        .callback_dispatch_budget = _view.callback_dispatch_budget,
        .startup_sealed           = _view.startup_sealed,
    };
}

} // namespace Detail

inline RegistrationResult RegisterBool(const CVarDescriptor& _descriptor, bool _default_value) {
    return Detail::RegisterBoolRaw(Detail::ViewOf(_descriptor), _default_value, nullptr, nullptr, nullptr);
}

template<typename Callback>
RegistrationResult
RegisterBool(const CVarDescriptor& _descriptor, bool _default_value, Callback&& _callback) {
    using StoredCallback = std::decay_t<Callback>;
    auto* context        = new StoredCallback(std::forward<Callback>(_callback));
    return Detail::
        RegisterBoolRaw(Detail::ViewOf(_descriptor), _default_value, &Detail::InvokeBoolCallback<StoredCallback>, context, &Detail::DestroyOwnedCallback<StoredCallback>);
}

inline RegistrationResult RegisterInt(const CVarDescriptor& _descriptor, std::int64_t _default_value) {
    return Detail::RegisterIntRaw(Detail::ViewOf(_descriptor), _default_value, nullptr, nullptr, nullptr);
}

template<typename Callback>
RegistrationResult
RegisterInt(const CVarDescriptor& _descriptor, std::int64_t _default_value, Callback&& _callback) {
    using StoredCallback = std::decay_t<Callback>;
    auto* context        = new StoredCallback(std::forward<Callback>(_callback));
    return Detail::
        RegisterIntRaw(Detail::ViewOf(_descriptor), _default_value, &Detail::InvokeIntCallback<StoredCallback>, context, &Detail::DestroyOwnedCallback<StoredCallback>);
}

inline RegistrationResult RegisterFloat(const CVarDescriptor& _descriptor, double _default_value) {
    return Detail::RegisterFloatRaw(Detail::ViewOf(_descriptor), _default_value, nullptr, nullptr, nullptr);
}

template<typename Callback>
RegistrationResult
RegisterFloat(const CVarDescriptor& _descriptor, double _default_value, Callback&& _callback) {
    using StoredCallback = std::decay_t<Callback>;
    auto* context        = new StoredCallback(std::forward<Callback>(_callback));
    return Detail::
        RegisterFloatRaw(Detail::ViewOf(_descriptor), _default_value, &Detail::InvokeFloatCallback<StoredCallback>, context, &Detail::DestroyOwnedCallback<StoredCallback>);
}

inline RegistrationResult RegisterString(const CVarDescriptor& _descriptor, std::string_view _default_value) {
    return Detail::RegisterStringRaw(Detail::ViewOf(_descriptor), _default_value, nullptr, nullptr, nullptr);
}

template<typename Callback>
RegistrationResult
RegisterString(const CVarDescriptor& _descriptor, std::string_view _default_value, Callback&& _callback) {
    using StoredCallback = std::decay_t<Callback>;
    auto* context        = new StoredCallback(std::forward<Callback>(_callback));
    return Detail::
        RegisterStringRaw(Detail::ViewOf(_descriptor), _default_value, &Detail::InvokeStringCallback<StoredCallback>, context, &Detail::DestroyOwnedCallback<StoredCallback>);
}

inline std::optional<CVarSnapshot> Find(std::string_view _name) {
    std::optional<CVarSnapshot> snapshot;
    Detail::VisitSnapshotRaw(
        _name,
        [](const CVarSnapshotView& _view, void* _context) {
            *static_cast<std::optional<CVarSnapshot>*>(_context) = Detail::CopySnapshot(_view);
        },
        &snapshot
    );
    return snapshot;
}

inline std::optional<CVarSnapshot> FindById(std::uint64_t _id) {
    std::optional<CVarSnapshot> snapshot;
    Detail::VisitSnapshotByIdRaw(
        _id,
        [](const CVarSnapshotView& _view, void* _context) {
            *static_cast<std::optional<CVarSnapshot>*>(_context) = Detail::CopySnapshot(_view);
        },
        &snapshot
    );
    return snapshot;
}

inline std::vector<CVarSnapshot> List(std::string_view _prefix = {}) {
    std::vector<CVarSnapshot> snapshots;
    Detail::VisitAllSnapshotsRaw(
        _prefix,
        [](const CVarSnapshotView& _view, void* _context) {
            static_cast<std::vector<CVarSnapshot>*>(_context)->push_back(Detail::CopySnapshot(_view));
        },
        &snapshots
    );
    std::ranges::sort(snapshots, [](const CVarSnapshot& _lhs, const CVarSnapshot& _rhs) {
        const auto less_insensitive = [](char _left, char _right) {
            const char left = _left >= 'A' && _left <= 'Z' ? static_cast<char>(_left + ('a' - 'A')) : _left;
            const char right =
                _right >= 'A' && _right <= 'Z' ? static_cast<char>(_right + ('a' - 'A')) : _right;
            return left < right;
        };
        return std::lexicographical_compare(
            _lhs.name.begin(), _lhs.name.end(), _rhs.name.begin(), _rhs.name.end(), less_insensitive
        );
    });
    return snapshots;
}

CORE_API CVarSetResult
SetValueFromString(std::string_view _name, std::string_view _value, ESetSource _source = ESetSource::Runtime);

CORE_API void SealStartupOnlyCVars();
CORE_API bool IsStartupConfigurationSealed() noexcept;

inline std::optional<CVarSnapshot> Registration::GetSnapshot() const {
    return FindById(id);
}

} // namespace Moer::CVar
