#include "config/CVarSystem.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace Moer::CVar {

namespace {

using CVarValue = std::variant<bool, std::int64_t, double, std::string>;

std::string_view TrimView(std::string_view _text) noexcept {
    while (!_text.empty() && std::isspace(static_cast<unsigned char>(_text.front()))) {
        _text.remove_prefix(1);
    }
    while (!_text.empty() && std::isspace(static_cast<unsigned char>(_text.back()))) {
        _text.remove_suffix(1);
    }
    return _text;
}

constexpr char ToLowerAscii(char _character) noexcept {
    return _character >= 'A' && _character <= 'Z' ? static_cast<char>(_character + ('a' - 'A')) : _character;
}

std::string NormalizeName(std::string_view _name) {
    std::string normalized;
    normalized.reserve(_name.size());
    for (const char character : _name) {
        normalized.push_back(ToLowerAscii(character));
    }
    return normalized;
}

bool IsValidName(std::string_view _name) noexcept {
    if (_name.empty() || _name.front() == '/') {
        return false;
    }
    for (const char character : _name) {
        const bool is_ascii_alphanumeric = (character >= '0' && character <= '9') ||
                                           (character >= 'A' && character <= 'Z') ||
                                           (character >= 'a' && character <= 'z');
        if (!(is_ascii_alphanumeric || character == '.' || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool StartsWithInsensitive(std::string_view _text, std::string_view _prefix) noexcept {
    if (_prefix.size() > _text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < _prefix.size(); ++index) {
        if (ToLowerAscii(_text[index]) != ToLowerAscii(_prefix[index])) {
            return false;
        }
    }
    return true;
}

template<typename T>
constexpr EType TypeOf();

template<>
constexpr EType TypeOf<bool>() {
    return EType::Bool;
}

template<>
constexpr EType TypeOf<std::int64_t>() {
    return EType::Int;
}

template<>
constexpr EType TypeOf<double>() {
    return EType::Float;
}

template<>
constexpr EType TypeOf<std::string>() {
    return EType::String;
}

std::string ValueToString(bool _value) {
    return _value ? "true" : "false";
}

std::string ValueToString(std::int64_t _value) {
    char buffer[32]{};
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), _value);
    return error == std::errc{} ? std::string(buffer, static_cast<std::size_t>(end - buffer)) : std::string{};
}

std::string ValueToString(double _value) {
    char buffer[64]{};
    const auto [end, error] = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        _value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
    );
    return error == std::errc{} ? std::string(buffer, static_cast<std::size_t>(end - buffer)) : std::string{};
}

std::string ValueToString(const std::string& _value) {
    return _value;
}

template<typename T>
CVarSetResult ParseValue(std::string_view _text, T& _value);

template<>
CVarSetResult ParseValue<bool>(std::string_view _text, bool& _value) {
    const std::string normalized = NormalizeName(TrimView(_text));
    if (normalized == "1" || normalized == "true" || normalized == "on") {
        _value = true;
        return {.status = ESetStatus::Changed};
    }
    if (normalized == "0" || normalized == "false" || normalized == "off") {
        _value = false;
        return {.status = ESetStatus::Changed};
    }
    return {
        .status = ESetStatus::TypeMismatch,
        .detail = "expected bool (0/1, true/false, on/off)",
    };
}

template<>
CVarSetResult ParseValue<std::int64_t>(std::string_view _text, std::int64_t& _value) {
    _text = TrimView(_text);
    if (_text.empty()) {
        return {
            .status = ESetStatus::TypeMismatch,
            .detail = "expected integer",
        };
    }
    const auto [end, error] = std::from_chars(_text.data(), _text.data() + _text.size(), _value);
    if (error != std::errc{} || end != _text.data() + _text.size()) {
        return {
            .status = ESetStatus::TypeMismatch,
            .detail = "expected integer",
        };
    }
    return {.status = ESetStatus::Changed};
}

template<>
CVarSetResult ParseValue<double>(std::string_view _text, double& _value) {
    _text = TrimView(_text);
    if (_text.empty()) {
        return {
            .status = ESetStatus::TypeMismatch,
            .detail = "expected finite floating-point value",
        };
    }
    const auto [end, error] =
        std::from_chars(_text.data(), _text.data() + _text.size(), _value, std::chars_format::general);
    if (error != std::errc{} || end != _text.data() + _text.size() || !std::isfinite(_value)) {
        return {
            .status = ESetStatus::TypeMismatch,
            .detail = "expected finite floating-point value",
        };
    }
    return {.status = ESetStatus::Changed};
}

template<>
CVarSetResult ParseValue<std::string>(std::string_view _text, std::string& _value) {
    _value.assign(_text);
    return {.status = ESetStatus::Changed};
}

bool IntIsBelowMinimum(std::int64_t _value, double _minimum) noexcept {
    constexpr double minimum_int           = -9223372036854775808.0;
    constexpr double maximum_int_exclusive = 9223372036854775808.0;
    const double     threshold             = std::ceil(_minimum);
    if (threshold <= minimum_int) {
        return false;
    }
    if (threshold >= maximum_int_exclusive) {
        return true;
    }
    return _value < static_cast<std::int64_t>(threshold);
}

bool IntIsAboveMaximum(std::int64_t _value, double _maximum) noexcept {
    constexpr double minimum_int           = -9223372036854775808.0;
    constexpr double maximum_int_exclusive = 9223372036854775808.0;
    const double     threshold             = std::floor(_maximum);
    if (threshold >= maximum_int_exclusive) {
        return false;
    }
    if (threshold < minimum_int) {
        return true;
    }
    return _value > static_cast<std::int64_t>(threshold);
}

thread_local std::uint32_t callback_destroy_depth = 0;
thread_local std::uint32_t snapshot_visitor_depth = 0;

struct CallbackOwner {
    CallbackOwner(void* _context, Detail::DestroyCallback _destroy) noexcept :
        context(_context),
        destroy(_destroy) {}

    ~CallbackOwner() {
        Reset();
    }

    CallbackOwner(const CallbackOwner&)            = delete;
    CallbackOwner& operator=(const CallbackOwner&) = delete;

    CallbackOwner(CallbackOwner&& _other) noexcept :
        context(std::exchange(_other.context, nullptr)),
        destroy(std::exchange(_other.destroy, nullptr)) {}

    CallbackOwner& operator=(CallbackOwner&& _other) noexcept {
        if (this != &_other) {
            Reset();
            context = std::exchange(_other.context, nullptr);
            destroy = std::exchange(_other.destroy, nullptr);
        }
        return *this;
    }

    void Reset() noexcept {
        void*                   retired_context = std::exchange(context, nullptr);
        Detail::DestroyCallback retired_destroy = std::exchange(destroy, nullptr);
        if (retired_destroy) {
            ++callback_destroy_depth;
            retired_destroy(retired_context);
            --callback_destroy_depth;
        }
    }

    void*                   context = nullptr;
    Detail::DestroyCallback destroy = nullptr;
};

class EntryBase;

struct ActiveOperationNode {
    EntryBase*           entry    = nullptr;
    ActiveOperationNode* previous = nullptr;
};

struct ActiveCallbackNode {
    EntryBase*          entry    = nullptr;
    ActiveCallbackNode* previous = nullptr;
};

thread_local ActiveOperationNode* active_operation = nullptr;
thread_local ActiveCallbackNode*  active_callback  = nullptr;

bool IsInsideNonblockingResetContext() noexcept {
    return active_callback != nullptr || callback_destroy_depth != 0 || snapshot_visitor_depth != 0;
}

class SnapshotVisitorScope {
public:
    SnapshotVisitorScope() noexcept {
        ++snapshot_visitor_depth;
    }

    ~SnapshotVisitorScope() {
        --snapshot_visitor_depth;
    }

    SnapshotVisitorScope(const SnapshotVisitorScope&)            = delete;
    SnapshotVisitorScope& operator=(const SnapshotVisitorScope&) = delete;
};

void InvokeSnapshotVisitor(
    SnapshotVisitor _visitor, const CVarSnapshotView& _snapshot, void* _context
) {
    SnapshotVisitorScope scope;
    _visitor(_snapshot, _context);
}

class EntryBase {
public:
    EntryBase(std::uint64_t _id, std::string _registry_key, CVarDescriptor _descriptor, EType _type) :
        id(_id),
        registry_key(std::move(_registry_key)),
        descriptor(std::move(_descriptor)),
        type(_type) {}

    virtual ~EntryBase() = default;

    EntryBase(const EntryBase&)            = delete;
    EntryBase& operator=(const EntryBase&) = delete;

    class Operation {
    public:
        explicit Operation(EntryBase& _entry) : entry(&_entry) {
            valid = entry->TryBeginOperation(node);
        }

        ~Operation() {
            if (valid) {
                entry->EndOperation(node);
            }
        }

        Operation(const Operation&)            = delete;
        Operation& operator=(const Operation&) = delete;

        [[nodiscard]] bool IsValid() const noexcept {
            return valid;
        }

    private:
        EntryBase*          entry = nullptr;
        ActiveOperationNode node;
        bool                valid = false;
    };

    virtual std::string   CopyValueString() const                                   = 0;
    virtual CVarSetResult SetFromString(std::string_view _text, ESetSource _source) = 0;
    virtual CVarSetResult SynchronizeOwnerValue(const CVarValue& _value)            = 0;

    void SealStartup() noexcept {
        if (HasFlag(descriptor.flags, EFlags::StartupOnly)) {
            std::lock_guard lock(mutation_mutex);
            startup_sealed.store(true, std::memory_order_release);
        }
    }

    void RetireAndWait() noexcept {
        std::unique_ptr<CallbackOwner> retired_callback_owner;
        {
            std::lock_guard lock(mutation_mutex);
            retired.store(true, std::memory_order_release);
            retired_callback_owner.reset(RetireCallbacksLocked());
        }
        retired_callback_owner.reset();

        std::unique_lock lock(lifetime_mutex);
        if (IsActiveOnCurrentThread() || IsInsideNonblockingResetContext()) {
            return;
        }
        lifetime_cv.wait(lock, [this] {
            return active_operations == 0;
        });
        lock.unlock();

        {
            std::lock_guard mutation_lock(mutation_mutex);
            retired_callback_owner.reset(DetachCallbackOwnerLocked());
        }
        retired_callback_owner.reset();
    }

    [[nodiscard]] bool IsRetired() const noexcept {
        return retired.load(std::memory_order_acquire);
    }

    std::uint64_t    id = 0;
    std::string      registry_key;
    CVarDescriptor   descriptor;
    EType            type = EType::String;
    std::atomic_bool startup_sealed{false};

protected:
    virtual CallbackOwner* RetireCallbacksLocked() noexcept     = 0;
    virtual CallbackOwner* DetachCallbackOwnerLocked() noexcept = 0;

    std::mutex mutation_mutex;

private:
    bool TryBeginOperation(ActiveOperationNode& _node) noexcept {
        {
            std::lock_guard lock(lifetime_mutex);
            if (retired.load(std::memory_order_acquire)) {
                return false;
            }
            ++active_operations;
        }
        _node.entry      = this;
        _node.previous   = active_operation;
        active_operation = &_node;
        return true;
    }

    void EndOperation(ActiveOperationNode& _node) noexcept {
        active_operation = _node.previous;
        std::lock_guard lock(lifetime_mutex);
        --active_operations;
        if (active_operations == 0) {
            lifetime_cv.notify_all();
        }
    }

    bool IsActiveOnCurrentThread() const noexcept {
        for (ActiveOperationNode* node = active_operation; node != nullptr; node = node->previous) {
            if (node->entry == this) {
                return true;
            }
        }
        return false;
    }

    std::mutex              lifetime_mutex;
    std::condition_variable lifetime_cv;
    std::size_t             active_operations = 0;
    std::atomic_bool        retired{false};
};

template<typename T, typename Callback>
class Entry final : public EntryBase {
public:
    Entry(
        std::uint64_t  _id,
        std::string    _registry_key,
        CVarDescriptor _descriptor,
        T              _value,
        Callback       _callback,
        CallbackOwner  _callback_owner
    ) :
        EntryBase(_id, std::move(_registry_key), std::move(_descriptor), TypeOf<T>()),
        value(std::move(_value)),
        callback(_callback),
        callback_owner(
            _callback_owner.context || _callback_owner.destroy ?
                std::make_unique<CallbackOwner>(std::move(_callback_owner)) :
                nullptr
        ) {}

    std::string CopyValueString() const override {
        std::shared_lock lock(value_mutex);
        return ValueToString(value);
    }

    CVarSetResult SetFromString(std::string_view _text, ESetSource _source) override {
        Operation operation(*this);
        if (!operation.IsValid()) {
            return {
                .status = ESetStatus::InvalidRegistration,
                .detail = "cvar registration is no longer active",
            };
        }

        MutationOutcome  outcome;
        std::unique_lock mutation_lock(mutation_mutex);
        if (IsRetired()) {
            return {
                .status = ESetStatus::InvalidRegistration,
                .detail = "cvar registration is no longer active",
            };
        }
        if (HasFlag(descriptor.flags, EFlags::ReadOnly)) {
            return {
                .status = ESetStatus::ReadOnly,
                .detail = "cvar is read-only",
            };
        }
        if (HasFlag(descriptor.flags, EFlags::StartupOnly)) {
            if (_source != ESetSource::StartupConfig) {
                return {
                    .status = ESetStatus::StartupSealed,
                    .detail = "cvar can only be changed by startup configuration",
                };
            }
            if (startup_sealed.load(std::memory_order_acquire)) {
                return {
                    .status = ESetStatus::StartupSealed,
                    .detail = "cvar is sealed after startup",
                };
            }
        }

        T             parsed{};
        CVarSetResult result = ParseValue<T>(_text, parsed);
        if (!result.Succeeded()) {
            return result;
        }
        outcome = SetParsedValueLocked(std::move(parsed), true);
        mutation_lock.unlock();
        return CompleteMutation(std::move(outcome));
    }

    CVarSetResult SynchronizeOwnerValue(const CVarValue& _value) override {
        Operation operation(*this);
        if (!operation.IsValid()) {
            return {
                .status = ESetStatus::InvalidRegistration,
                .detail = "cvar registration is no longer active",
            };
        }

        const T* typed_value = std::get_if<T>(&_value);
        if (!typed_value) {
            return {
                .status = ESetStatus::TypeMismatch,
                .detail = "owner value type does not match cvar type",
            };
        }

        MutationOutcome  outcome;
        std::unique_lock mutation_lock(mutation_mutex);
        if (IsRetired()) {
            return {
                .status = ESetStatus::InvalidRegistration,
                .detail = "cvar registration is no longer active",
            };
        }
        outcome = SetParsedValueLocked(*typed_value, false);
        mutation_lock.unlock();
        return CompleteMutation(std::move(outcome));
    }

private:
    struct CallbackEvent {
        CallbackEvent(const T& _old_value, const T& _new_value) :
            old_value(_old_value),
            new_value(_new_value) {}

        T    old_value;
        T    new_value;
        bool completed          = false;
        bool callback_succeeded = true;
        bool callback_canceled  = false;
    };

    struct MutationOutcome {
        CVarSetResult                  result;
        std::shared_ptr<CallbackEvent> callback_event;
        bool                           owns_dispatch = false;
    };

    CVarSetResult ValidateRange(const T& _value) const {
        if constexpr (std::is_same_v<T, std::int64_t>) {
            if (descriptor.min_value && IntIsBelowMinimum(_value, *descriptor.min_value)) {
                return {
                    .status = ESetStatus::OutOfRange,
                    .detail = "value is below the allowed minimum",
                };
            }
            if (descriptor.max_value && IntIsAboveMaximum(_value, *descriptor.max_value)) {
                return {
                    .status = ESetStatus::OutOfRange,
                    .detail = "value is above the allowed maximum",
                };
            }
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(_value)) {
                return {
                    .status = ESetStatus::TypeMismatch,
                    .detail = "expected finite floating-point value",
                };
            }
            if (descriptor.min_value && _value < *descriptor.min_value) {
                return {
                    .status = ESetStatus::OutOfRange,
                    .detail = "value is below the allowed minimum",
                };
            }
            if (descriptor.max_value && _value > *descriptor.max_value) {
                return {
                    .status = ESetStatus::OutOfRange,
                    .detail = "value is above the allowed maximum",
                };
            }
        }
        return {.status = ESetStatus::Changed};
    }

    MutationOutcome SetParsedValueLocked(T _new_value, bool _invoke_callback) {
        CVarSetResult range_result = ValidateRange(_new_value);
        if (!range_result.Succeeded()) {
            return {.result = range_result};
        }

        MutationOutcome outcome{
            .result = {.status = ESetStatus::Changed},
        };
        T old_value;
        {
            std::unique_lock value_lock(value_mutex);
            if (value == _new_value) {
                outcome.result.status = ESetStatus::Unchanged;
                return outcome;
            }
            const bool produces_callback = _invoke_callback && callback && callback_owner;
            if (produces_callback && callback_dispatching && callback_admissions_remaining == 0) {
                return {
                    .result =
                        {
                            .status = ESetStatus::CallbackQueueFull,
                            .detail = "callback dispatch budget is exhausted",
                        },
                };
            }

            old_value = value;
            if (produces_callback) {
                outcome.callback_event = std::make_shared<CallbackEvent>(old_value, _new_value);
                callback_queue.push_back(outcome.callback_event);
            }
            value = std::move(_new_value);
        }

        if (outcome.callback_event) {
            if (!callback_dispatching) {
                callback_dispatching          = true;
                callback_admissions_remaining = descriptor.callback_dispatch_budget - 1;
                outcome.owns_dispatch         = true;
            } else {
                --callback_admissions_remaining;
            }
        }
        return outcome;
    }

    CVarSetResult CompleteMutation(MutationOutcome _outcome) {
        if (!_outcome.callback_event) {
            return _outcome.result;
        }
        if (_outcome.owns_dispatch) {
            DrainCallbacks();
        }

        std::lock_guard lock(mutation_mutex);
        if (!_outcome.callback_event->completed) {
            _outcome.result.callback_deferred = true;
            _outcome.result.detail            = "value changed; callback completion was deferred";
            return _outcome.result;
        }
        if (!_outcome.callback_event->callback_succeeded) {
            _outcome.result.callback_failed = true;
            _outcome.result.detail          = "value changed, but the on-change callback failed";
        } else if (_outcome.callback_event->callback_canceled) {
            _outcome.result.callback_canceled = true;
            _outcome.result.detail            = "value changed; callback was canceled by unregistration";
        }
        return _outcome.result;
    }

    bool InvokeCallback(Callback _callback, void* _context, const CallbackEvent& _event) {
        ActiveCallbackNode callback_node{
            .entry    = this,
            .previous = active_callback,
        };
        active_callback         = &callback_node;
        bool callback_succeeded = false;
        if constexpr (std::is_same_v<T, std::string>) {
            callback_succeeded =
                _callback(_context, std::string_view(_event.old_value), std::string_view(_event.new_value));
        } else {
            callback_succeeded = _callback(_context, _event.old_value, _event.new_value);
        }
        active_callback = callback_node.previous;
        return callback_succeeded;
    }

    void DrainCallbacks() {
        for (;;) {
            std::shared_ptr<CallbackEvent> callback_event;
            Callback                       callback_to_invoke = nullptr;
            void*                          callback_context   = nullptr;
            std::unique_ptr<CallbackOwner> retired_callback_owner;
            {
                std::unique_lock lock(mutation_mutex);
                if (IsRetired() || callback_queue.empty()) {
                    callback_dispatching          = false;
                    callback_admissions_remaining = 0;
                    if (IsRetired() && !callback_invoking) {
                        retired_callback_owner.reset(DetachCallbackOwnerLocked());
                    }
                    lock.unlock();
                    retired_callback_owner.reset();
                    return;
                }

                callback_event = std::move(callback_queue.front());
                callback_queue.pop_front();
                callback_to_invoke = callback;
                callback_context   = callback_owner ? callback_owner->context : nullptr;
                callback_invoking  = true;
            }

            const bool callback_succeeded =
                callback_to_invoke && callback_context &&
                InvokeCallback(callback_to_invoke, callback_context, *callback_event);

            bool stop_dispatch = false;
            {
                std::unique_lock lock(mutation_mutex);
                callback_invoking                  = false;
                callback_event->callback_succeeded = callback_succeeded;
                callback_event->completed          = true;
                if (IsRetired()) {
                    CancelQueuedCallbacksLocked();
                    callback_dispatching          = false;
                    callback_admissions_remaining = 0;
                    retired_callback_owner.reset(DetachCallbackOwnerLocked());
                    stop_dispatch = true;
                } else if (callback_queue.empty()) {
                    callback_dispatching          = false;
                    callback_admissions_remaining = 0;
                    stop_dispatch                 = true;
                }
            }
            retired_callback_owner.reset();
            if (stop_dispatch) {
                return;
            }
        }
    }

    void CancelQueuedCallbacksLocked() noexcept {
        for (const std::shared_ptr<CallbackEvent>& event : callback_queue) {
            event->callback_canceled = true;
            event->completed         = true;
        }
        callback_queue.clear();
    }

protected:
    CallbackOwner* RetireCallbacksLocked() noexcept override {
        callback = nullptr;
        CancelQueuedCallbacksLocked();
        if (callback_invoking) {
            return nullptr;
        }
        callback_dispatching          = false;
        callback_admissions_remaining = 0;
        return callback_owner.release();
    }

    CallbackOwner* DetachCallbackOwnerLocked() noexcept override {
        if (callback_invoking) {
            return nullptr;
        }
        callback = nullptr;
        CancelQueuedCallbacksLocked();
        callback_admissions_remaining = 0;
        return callback_owner.release();
    }

private:
    mutable std::shared_mutex                  value_mutex;
    T                                          value;
    Callback                                   callback = nullptr;
    std::unique_ptr<CallbackOwner>             callback_owner;
    std::deque<std::shared_ptr<CallbackEvent>> callback_queue;
    std::size_t                                callback_admissions_remaining = 0;
    bool                                       callback_dispatching          = false;
    bool                                       callback_invoking             = false;
};

using BoolEntry   = Entry<bool, Detail::BoolCallback>;
using IntEntry    = Entry<std::int64_t, Detail::IntCallback>;
using FloatEntry  = Entry<double, Detail::FloatCallback>;
using StringEntry = Entry<std::string, Detail::StringCallback>;

struct RegistryData {
    std::mutex                                                    registry_mutex;
    std::unordered_map<std::string, std::shared_ptr<EntryBase>>   by_name;
    std::unordered_map<std::uint64_t, std::shared_ptr<EntryBase>> by_id;
    std::atomic<std::uint64_t>                                    next_id{1};
    bool                                                          startup_sealed = false;
};

RegistryData& GetRegistry() {
    // Registrations and their caller-owned destroy thunks may have static
    // lifetime and re-enter this API during process teardown. Keep the
    // registry alive until the process releases its address space instead of
    // depending on cross-module static destruction order.
    static RegistryData* const registry = new RegistryData();
    return *registry;
}

std::shared_ptr<EntryBase> FindEntryById(std::uint64_t _id) {
    RegistryData&   registry = GetRegistry();
    std::lock_guard lock(registry.registry_mutex);
    const auto      found = registry.by_id.find(_id);
    return found == registry.by_id.end() ? nullptr : found->second;
}

std::shared_ptr<EntryBase> FindEntryByName(std::string_view _name) {
    RegistryData&   registry = GetRegistry();
    std::lock_guard lock(registry.registry_mutex);
    const auto      found = registry.by_name.find(NormalizeName(_name));
    return found == registry.by_name.end() ? nullptr : found->second;
}

void Unregister(std::uint64_t _id) noexcept {
    if (_id == 0) {
        return;
    }

    std::shared_ptr<EntryBase> retired_entry;
    RegistryData&              registry = GetRegistry();
    {
        std::lock_guard lock(registry.registry_mutex);
        const auto      by_id = registry.by_id.find(_id);
        if (by_id == registry.by_id.end()) {
            return;
        }

        retired_entry      = by_id->second;
        const auto by_name = registry.by_name.find(retired_entry->registry_key);
        if (by_name != registry.by_name.end() && by_name->second.get() == retired_entry.get()) {
            registry.by_name.erase(by_name);
        }
        registry.by_id.erase(by_id);
    }

    retired_entry->RetireAndWait();
    retired_entry.reset();
}

CVarDescriptor CopyDescriptor(CVarDescriptorView _view) {
    return {
        .name                     = std::string(_view.name),
        .helper                   = std::string(_view.helper),
        .true_helper              = std::string(_view.true_helper),
        .false_helper             = std::string(_view.false_helper),
        .flags                    = _view.flags,
        .min_value                = _view.min_value,
        .max_value                = _view.max_value,
        .callback_dispatch_budget = _view.callback_dispatch_budget,
    };
}

template<typename Value>
bool DescriptorIsInvalid(CVarDescriptorView _descriptor, const Value& _value) {
    const bool has_non_finite_bound = (_descriptor.min_value && !std::isfinite(*_descriptor.min_value)) ||
                                      (_descriptor.max_value && !std::isfinite(*_descriptor.max_value));
    const bool has_inverted_range =
        _descriptor.min_value && _descriptor.max_value && *_descriptor.min_value > *_descriptor.max_value;
    const bool has_range_for_non_numeric = !std::is_same_v<Value, std::int64_t> &&
                                           !std::is_same_v<Value, double> &&
                                           (_descriptor.min_value || _descriptor.max_value);
    const bool has_invalid_callback_budget = _descriptor.callback_dispatch_budget == 0 ||
                                             _descriptor.callback_dispatch_budget > MaxCallbackDispatchBudget;
    bool default_is_invalid = false;
    if constexpr (std::is_same_v<Value, std::int64_t>) {
        default_is_invalid = (_descriptor.min_value && IntIsBelowMinimum(_value, *_descriptor.min_value)) ||
                             (_descriptor.max_value && IntIsAboveMaximum(_value, *_descriptor.max_value));
    } else if constexpr (std::is_same_v<Value, double>) {
        default_is_invalid = !std::isfinite(_value) ||
                             (_descriptor.min_value && _value < *_descriptor.min_value) ||
                             (_descriptor.max_value && _value > *_descriptor.max_value);
    }
    return !IsValidName(_descriptor.name) || has_non_finite_bound || has_inverted_range ||
           has_range_for_non_numeric || has_invalid_callback_budget || default_is_invalid;
}

template<typename EntryType, typename Value, typename Callback>
RegistrationResult RegisterEntry(
    CVarDescriptorView _descriptor,
    Value              _value,
    Callback           _callback,
    CallbackOwner      _callback_owner
) {
    const bool has_callback               = _callback != nullptr;
    const bool has_callback_context       = _callback_owner.context != nullptr;
    const bool has_callback_destroy       = _callback_owner.destroy != nullptr;
    const bool has_empty_callback_binding = !has_callback && !has_callback_context && !has_callback_destroy;
    const bool has_complete_callback_binding = has_callback && has_callback_context && has_callback_destroy;
    if (DescriptorIsInvalid(_descriptor, _value) ||
        (!has_empty_callback_binding && !has_complete_callback_binding)) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "invalid cvar descriptor, default, or callback binding",
        };
    }

    RegistryData&       registry = GetRegistry();
    const std::uint64_t id       = registry.next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "cvar registration id space was exhausted",
        };
    }

    std::string registry_key = NormalizeName(_descriptor.name);
    auto        entry        = std::make_shared<EntryType>(
        id,
        registry_key,
        CopyDescriptor(_descriptor),
        std::move(_value),
        _callback,
        std::move(_callback_owner)
    );

    bool               duplicate    = false;
    bool               id_collision = false;
    std::exception_ptr insertion_failure;
    {
        std::lock_guard lock(registry.registry_mutex);
        if (registry.by_name.contains(registry_key)) {
            duplicate = true;
        } else if (registry.by_id.contains(id)) {
            id_collision = true;
        } else {
            if (registry.startup_sealed) {
                entry->SealStartup();
            }
            try {
                const bool name_inserted = registry.by_name.emplace(registry_key, entry).second;
                const bool id_inserted   = registry.by_id.emplace(id, entry).second;
                if (!name_inserted || !id_inserted) {
                    if (name_inserted) {
                        registry.by_name.erase(registry_key);
                    }
                    if (id_inserted) {
                        registry.by_id.erase(id);
                    }
                    id_collision = true;
                }
            } catch (...) {
                registry.by_name.erase(registry_key);
                registry.by_id.erase(id);
                insertion_failure = std::current_exception();
            }
        }
    }

    if (insertion_failure) {
        entry.reset();
        std::rethrow_exception(insertion_failure);
    }
    if (duplicate) {
        return {
            .status = ERegistrationStatus::DuplicateName,
            .detail = "a cvar with the same case-insensitive name exists",
        };
    }
    if (id_collision) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "cvar registration id space was exhausted",
        };
    }
    return {
        .status       = ERegistrationStatus::Registered,
        .registration = Detail::RegistrationAccess::Create(id),
    };
}

template<typename Callback>
bool CallbackBindingIsValid(Callback _callback, void* _context, Detail::DestroyCallback _destroy) noexcept {
    const bool empty    = _callback == nullptr && _context == nullptr && _destroy == nullptr;
    const bool complete = _callback != nullptr && _context != nullptr && _destroy != nullptr;
    return empty || complete;
}

bool VisitEntrySnapshot(const std::shared_ptr<EntryBase>& _entry, SnapshotVisitor _visitor, void* _context) {
    if (!_entry || !_visitor) {
        return false;
    }

    std::string      value;
    CVarSnapshotView snapshot;
    {
        EntryBase::Operation operation(*_entry);
        if (!operation.IsValid()) {
            return false;
        }
        value    = _entry->CopyValueString();
        snapshot = {
            .id                       = _entry->id,
            .name                     = _entry->descriptor.name,
            .helper                   = _entry->descriptor.helper,
            .true_helper              = _entry->descriptor.true_helper,
            .false_helper             = _entry->descriptor.false_helper,
            .value                    = value,
            .type                     = _entry->type,
            .flags                    = _entry->descriptor.flags,
            .min_value                = _entry->descriptor.min_value,
            .max_value                = _entry->descriptor.max_value,
            .callback_dispatch_budget = _entry->descriptor.callback_dispatch_budget,
            .startup_sealed           = _entry->startup_sealed.load(std::memory_order_acquire),
        };
    }

    // The shared_ptr keeps descriptor storage alive and value owns its copy.
    // End the active operation before entering caller code so cross-entry
    // Reset calls from concurrent visitors cannot form a quiescence wait cycle.
    InvokeSnapshotVisitor(_visitor, snapshot, _context);
    return true;
}

} // namespace

Registration Detail::RegistrationAccess::Create(std::uint64_t _id) noexcept {
    return Registration(_id);
}

Registration::Registration(std::uint64_t _id) noexcept : id(_id) {}

Registration::~Registration() {
    Reset();
}

Registration::Registration(Registration&& _other) noexcept : id(std::exchange(_other.id, 0)) {}

Registration& Registration::operator=(Registration&& _other) noexcept {
    if (this != &_other) {
        Reset();
        id = std::exchange(_other.id, 0);
    }
    return *this;
}

CVarSetResult Registration::SynchronizeOwnerValue(bool _value) {
    const std::shared_ptr<EntryBase> entry = FindEntryById(id);
    return entry ? entry->SynchronizeOwnerValue(CVarValue(_value)) :
                   CVarSetResult{
                       .status = ESetStatus::InvalidRegistration,
                       .detail = "registration is no longer active",
                   };
}

CVarSetResult Registration::SynchronizeOwnerValue(std::int64_t _value) {
    const std::shared_ptr<EntryBase> entry = FindEntryById(id);
    return entry ? entry->SynchronizeOwnerValue(CVarValue(_value)) :
                   CVarSetResult{
                       .status = ESetStatus::InvalidRegistration,
                       .detail = "registration is no longer active",
                   };
}

CVarSetResult Registration::SynchronizeOwnerValue(double _value) {
    const std::shared_ptr<EntryBase> entry = FindEntryById(id);
    return entry ? entry->SynchronizeOwnerValue(CVarValue(_value)) :
                   CVarSetResult{
                       .status = ESetStatus::InvalidRegistration,
                       .detail = "registration is no longer active",
                   };
}

CVarSetResult Registration::SynchronizeOwnerValue(std::string_view _value) {
    const std::shared_ptr<EntryBase> entry = FindEntryById(id);
    return entry ? entry->SynchronizeOwnerValue(CVarValue(std::string(_value))) :
                   CVarSetResult{
                       .status = ESetStatus::InvalidRegistration,
                       .detail = "registration is no longer active",
                   };
}

void Registration::Reset() noexcept {
    const std::uint64_t reset_id = std::exchange(id, 0);
    Unregister(reset_id);
}

RegistrationResult Detail::RegisterBoolRaw(
    CVarDescriptorView _descriptor,
    bool               _default_value,
    BoolCallback       _callback,
    void*              _context,
    DestroyCallback    _destroy
) {
    if (!CallbackBindingIsValid(_callback, _context, _destroy)) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "callback binding must be entirely empty or complete",
        };
    }
    CallbackOwner owner(_context, _destroy);
    return RegisterEntry<BoolEntry>(_descriptor, _default_value, _callback, std::move(owner));
}

RegistrationResult Detail::RegisterIntRaw(
    CVarDescriptorView _descriptor,
    std::int64_t       _default_value,
    IntCallback        _callback,
    void*              _context,
    DestroyCallback    _destroy
) {
    if (!CallbackBindingIsValid(_callback, _context, _destroy)) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "callback binding must be entirely empty or complete",
        };
    }
    CallbackOwner owner(_context, _destroy);
    return RegisterEntry<IntEntry>(_descriptor, _default_value, _callback, std::move(owner));
}

RegistrationResult Detail::RegisterFloatRaw(
    CVarDescriptorView _descriptor,
    double             _default_value,
    FloatCallback      _callback,
    void*              _context,
    DestroyCallback    _destroy
) {
    if (!CallbackBindingIsValid(_callback, _context, _destroy)) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "callback binding must be entirely empty or complete",
        };
    }
    CallbackOwner owner(_context, _destroy);
    return RegisterEntry<FloatEntry>(_descriptor, _default_value, _callback, std::move(owner));
}

RegistrationResult Detail::RegisterStringRaw(
    CVarDescriptorView _descriptor,
    std::string_view   _default_value,
    StringCallback     _callback,
    void*              _context,
    DestroyCallback    _destroy
) {
    if (!CallbackBindingIsValid(_callback, _context, _destroy)) {
        return {
            .status = ERegistrationStatus::InvalidDescriptor,
            .detail = "callback binding must be entirely empty or complete",
        };
    }
    CallbackOwner owner(_context, _destroy);
    return RegisterEntry<StringEntry>(_descriptor, std::string(_default_value), _callback, std::move(owner));
}

bool Detail::VisitSnapshotRaw(std::string_view _name, SnapshotVisitor _visitor, void* _context) {
    return VisitEntrySnapshot(FindEntryByName(_name), _visitor, _context);
}

bool Detail::VisitSnapshotByIdRaw(std::uint64_t _id, SnapshotVisitor _visitor, void* _context) {
    return VisitEntrySnapshot(FindEntryById(_id), _visitor, _context);
}

std::size_t Detail::VisitAllSnapshotsRaw(std::string_view _prefix, SnapshotVisitor _visitor, void* _context) {
    if (!_visitor) {
        return 0;
    }

    std::vector<std::shared_ptr<EntryBase>> entries;
    {
        RegistryData&   registry = GetRegistry();
        std::lock_guard lock(registry.registry_mutex);
        entries.reserve(registry.by_name.size());
        for (const auto& [name, entry] : registry.by_name) {
            (void)name;
            if (_prefix.empty() || StartsWithInsensitive(entry->descriptor.name, _prefix)) {
                entries.push_back(entry);
            }
        }
    }

    std::size_t visited_count = 0;
    for (const std::shared_ptr<EntryBase>& entry : entries) {
        if (VisitEntrySnapshot(entry, _visitor, _context)) {
            ++visited_count;
        }
    }
    return visited_count;
}

CVarSetResult SetValueFromString(std::string_view _name, std::string_view _value, ESetSource _source) {
    const std::shared_ptr<EntryBase> entry = FindEntryByName(_name);
    if (!entry) {
        return {
            .status = ESetStatus::NotFound,
            .detail = "unknown cvar",
        };
    }
    return entry->SetFromString(_value, _source);
}

void SealStartupOnlyCVars() {
    RegistryData&   registry = GetRegistry();
    std::lock_guard lock(registry.registry_mutex);
    registry.startup_sealed = true;
    for (const auto& [id, entry] : registry.by_id) {
        (void)id;
        entry->SealStartup();
    }
}

bool IsStartupConfigurationSealed() noexcept {
    RegistryData&   registry = GetRegistry();
    std::lock_guard lock(registry.registry_mutex);
    return registry.startup_sealed;
}

} // namespace Moer::CVar
