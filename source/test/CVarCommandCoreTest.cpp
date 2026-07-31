#include "command/EngineCommandProcessor.h"
#include "config/CVarSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace Moer;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

bool OutputContains(const Command::CommandOutputBatch& _batch, std::string_view _text) {
    return std::ranges::any_of(_batch.lines, [_text](const Command::CommandOutputLine& _line) {
        return _line.text.find(_text) != std::string::npos;
    });
}

struct RegisterOnDestroy {
    std::atomic_bool* destroyed = nullptr;

    ~RegisterOnDestroy() {
        CVar::RegistrationResult nested =
            CVar::RegisterBool(CVar::CVarDescriptor{.name = "Test.Lifetime.DestructorReentry"}, false);
        if (destroyed) {
            destroyed->store(nested.Succeeded(), std::memory_order_release);
        }
    }
};

struct CountOnDestroy {
    std::atomic_uint32_t* count = nullptr;

    ~CountOnDestroy() {
        if (count) {
            count->fetch_add(1, std::memory_order_release);
        }
    }
};

void TestRegistrationLifetimeAndDescriptors() {
    CVar::CVarDescriptor descriptor{
        .name         = "Test.Lifetime.Toggle",
        .helper       = "lifetime helper",
        .true_helper  = "enabled",
        .false_helper = "disabled",
    };
    CVar::RegistrationResult result = CVar::RegisterBool(descriptor, false);
    Expect(result.Succeeded(), "valid bool cvar did not register");
    Expect(CVar::Find("test.lifetime.toggle")->value == "false", "lookup was not case-insensitive");

    CVar::RegistrationResult duplicate =
        CVar::RegisterBool(CVar::CVarDescriptor{.name = "TEST.LIFETIME.TOGGLE"}, true);
    Expect(
        duplicate.status == CVar::ERegistrationStatus::DuplicateName,
        "case-insensitive duplicate registration was accepted"
    );

    CVar::Registration moved = std::move(result.registration);
    Expect(moved.IsValid() && !result.registration.IsValid(), "registration move did not transfer ownership");
    moved.Reset();
    Expect(!CVar::Find("Test.Lifetime.Toggle"), "registration reset did not unregister the cvar");
    Expect(
        moved.SynchronizeOwnerValue(true).status == CVar::ESetStatus::InvalidRegistration,
        "stale registration accepted an owner update"
    );

    std::atomic_bool                   destructor_reentered{false};
    std::shared_ptr<RegisterOnDestroy> destroy_capture = std::make_shared<RegisterOnDestroy>();
    destroy_capture->destroyed                         = &destructor_reentered;
    CVar::RegistrationResult destructor_cvar           = CVar::RegisterBool(
        CVar::CVarDescriptor{.name = "Test.Lifetime.DestructorCapture"},
        false,
        [destroy_capture](bool, bool) {}
    );
    destroy_capture.reset();
    destructor_cvar.registration.Reset();
    Expect(
        destructor_reentered.load(std::memory_order_acquire),
        "callback capture was destroyed under the registry lock"
    );

    Expect(
        CVar::RegisterInt(CVar::CVarDescriptor{.name = "bad name"}, 1).status ==
            CVar::ERegistrationStatus::InvalidDescriptor,
        "invalid cvar name was accepted"
    );
    Expect(
        CVar::RegisterBool(
            CVar::CVarDescriptor{
                .name      = "Test.Invalid.BoolRange",
                .min_value = 0.0,
            },
            false
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "non-numeric cvar accepted a numeric range"
    );
    Expect(
        CVar::RegisterInt(
            CVar::CVarDescriptor{
                .name      = "Test.Invalid.InvertedRange",
                .min_value = 5.0,
                .max_value = 1.0,
            },
            3
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "inverted numeric range was accepted"
    );
    Expect(
        CVar::RegisterFloat(
            CVar::CVarDescriptor{
                .name      = "Test.Invalid.NonFiniteRange",
                .min_value = std::numeric_limits<double>::quiet_NaN(),
            },
            1.0
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "non-finite numeric range was accepted"
    );
    Expect(
        CVar::RegisterBool(
            CVar::CVarDescriptor{
                .name                     = "Test.Invalid.ZeroCallbackBudget",
                .callback_dispatch_budget = 0,
            },
            false
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "zero callback dispatch budget was accepted"
    );
    Expect(
        CVar::RegisterBool(
            CVar::CVarDescriptor{
                .name                     = "Test.Invalid.ExcessiveCallbackBudget",
                .callback_dispatch_budget = CVar::MaxCallbackDispatchBudget + 1,
            },
            false
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "excessive callback dispatch budget was accepted"
    );

    int incomplete_context = 0;
    Expect(
        CVar::Detail::RegisterBoolRaw(
            CVar::Detail::ViewOf(CVar::CVarDescriptor{
                .name = "Test.Invalid.IncompleteCallback",
            }),
            false,
            nullptr,
            &incomplete_context,
            nullptr
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "incomplete raw callback binding was accepted"
    );
}

void TestTypedParsingAndRanges() {
    CVar::RegistrationResult bool_cvar =
        CVar::RegisterBool(CVar::CVarDescriptor{.name = "Test.Types.Bool"}, false);
    CVar::RegistrationResult int_cvar = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name      = "Test.Types.Int",
            .min_value = -2.0,
            .max_value = 4.0,
        },
        2
    );
    CVar::RegistrationResult float_cvar = CVar::RegisterFloat(
        CVar::CVarDescriptor{
            .name      = "Test.Types.Float",
            .min_value = 0.0,
            .max_value = 2.0,
        },
        1.0
    );
    CVar::RegistrationResult owner_float =
        CVar::RegisterFloat(CVar::CVarDescriptor{.name = "Test.Types.OwnerFloat"}, 3.5);
    CVar::RegistrationResult string_cvar =
        CVar::RegisterString(CVar::CVarDescriptor{.name = "Test.Types.String"}, "initial");
    Expect(
        bool_cvar.Succeeded() && int_cvar.Succeeded() && float_cvar.Succeeded() &&
            owner_float.Succeeded() && string_cvar.Succeeded(),
        "typed cvar registration failed"
    );

    Expect(
        CVar::SetValueFromString("TEST.TYPES.BOOL", " ON ").status == CVar::ESetStatus::Changed,
        "bool parser rejected ON"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Bool", "maybe").status == CVar::ESetStatus::TypeMismatch,
        "bool parser accepted invalid text"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Int", "4").Succeeded(),
        "int parser rejected the inclusive maximum"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Int", "5").status == CVar::ESetStatus::OutOfRange,
        "int range accepted a value above maximum"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Int", "4x").status == CVar::ESetStatus::TypeMismatch,
        "int parser accepted trailing text"
    );
    CVar::RegistrationResult precise_int = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name      = "Test.Types.PreciseInt",
            .max_value = 9007199254740992.0,
        },
        std::int64_t{9007199254740992}
    );
    Expect(
        precise_int.Succeeded() &&
            CVar::SetValueFromString("Test.Types.PreciseInt", "9007199254740993").status ==
                CVar::ESetStatus::OutOfRange,
        "int64 range comparison lost precision above 2^53"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Float", "1.25").Succeeded(),
        "float parser rejected a finite value"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Float", "inf").status == CVar::ESetStatus::TypeMismatch,
        "float parser accepted infinity"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.Float", "-0.1").status == CVar::ESetStatus::OutOfRange,
        "float range accepted a value below minimum"
    );
    Expect(
        owner_float.registration.SynchronizeOwnerValue(4.5).Succeeded(),
        "owner synchronization rejected a finite float"
    );
    Expect(
        owner_float.registration.SynchronizeOwnerValue(std::numeric_limits<double>::quiet_NaN()).status ==
            CVar::ESetStatus::TypeMismatch,
        "owner synchronization accepted NaN"
    );
    Expect(
        owner_float.registration.SynchronizeOwnerValue(std::numeric_limits<double>::infinity()).status ==
            CVar::ESetStatus::TypeMismatch,
        "owner synchronization accepted positive infinity"
    );
    Expect(
        owner_float.registration.SynchronizeOwnerValue(-std::numeric_limits<double>::infinity()).status ==
            CVar::ESetStatus::TypeMismatch,
        "owner synchronization accepted negative infinity"
    );
    Expect(
        CVar::Find("Test.Types.OwnerFloat")->value == "4.5",
        "rejected non-finite owner synchronization changed the float value"
    );
    Expect(
        CVar::SetValueFromString("Test.Types.String", "hello world").Succeeded(),
        "string parser rejected spaces"
    );
    Expect(
        CVar::Find("Test.Types.String")->value == "hello world", "string value snapshot changed its contents"
    );
    Expect(
        CVar::SetValueFromString("missing.cvar", "1").status == CVar::ESetStatus::NotFound,
        "unknown cvar did not report NotFound"
    );

    Expect(
        CVar::RegisterInt(
            CVar::CVarDescriptor{
                .name      = "Test.Invalid.IntDefault",
                .max_value = 3.0,
            },
            4
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "out-of-range integer default was accepted"
    );
    Expect(
        CVar::RegisterFloat(
            CVar::CVarDescriptor{.name = "Test.Invalid.FloatDefault"}, std::numeric_limits<double>::infinity()
        )
                .status == CVar::ERegistrationStatus::InvalidDescriptor,
        "non-finite float default was accepted"
    );
}

void TestFlagsOwnerSyncAndCallbacks() {
    std::atomic_uint32_t     read_only_callbacks{0};
    CVar::RegistrationResult read_only = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name  = "Test.Flags.ReadOnly",
            .flags = CVar::EFlags::ReadOnly,
        },
        1,
        [&read_only_callbacks](std::int64_t, std::int64_t) {
            read_only_callbacks.fetch_add(1, std::memory_order_relaxed);
        }
    );
    Expect(read_only.Succeeded(), "read-only cvar did not register");
    Expect(
        CVar::SetValueFromString("Test.Flags.ReadOnly", "2").status == CVar::ESetStatus::ReadOnly,
        "runtime update changed a read-only cvar"
    );
    Expect(
        read_only.registration.SynchronizeOwnerValue(std::int64_t{2}).Succeeded(),
        "owner could not synchronize a read-only cvar"
    );
    Expect(
        read_only_callbacks.load(std::memory_order_relaxed) == 0,
        "owner synchronization invoked the on-change callback"
    );
    Expect(
        read_only.registration.SynchronizeOwnerValue(true).status == CVar::ESetStatus::TypeMismatch,
        "owner synchronization accepted the wrong type"
    );

    CVar::RegistrationResult target =
        CVar::RegisterInt(CVar::CVarDescriptor{.name = "Test.Callback.Target"}, 0);
    std::atomic_uint32_t     callback_count{0};
    CVar::RegistrationResult source = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.Source"},
        0,
        [&callback_count](std::int64_t, std::int64_t _new_value) {
            Expect(
                CVar::Find("Test.Callback.Source")->value == std::to_string(_new_value),
                "callback ran before the new value became visible"
            );
            Expect(
                CVar::SetValueFromString("Test.Callback.Target", std::to_string(_new_value)).Succeeded(),
                "callback could not re-enter the registry"
            );
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    );
    Expect(target.Succeeded() && source.Succeeded(), "callback fixture did not register");
    Expect(
        CVar::SetValueFromString("Test.Callback.Source", "7").Succeeded(), "callback source update failed"
    );
    Expect(
        callback_count.load(std::memory_order_relaxed) == 1 &&
            CVar::Find("Test.Callback.Target")->value == "7",
        "callback did not complete its re-entrant update"
    );
    Expect(
        source.registration.SynchronizeOwnerValue(std::int64_t{8}).Succeeded() &&
            callback_count.load(std::memory_order_relaxed) == 1,
        "owner synchronization unexpectedly invoked a callback"
    );

    CVar::RegistrationResult throwing = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.Throwing"},
        0,
        [](std::int64_t, std::int64_t) {
            throw std::runtime_error("injected callback failure");
        }
    );
    const CVar::CVarSetResult throwing_result = CVar::SetValueFromString("Test.Callback.Throwing", "1");
    Expect(
        throwing_result.Succeeded() && throwing_result.callback_failed &&
            CVar::Find("Test.Callback.Throwing")->value == "1",
        "callback exception escaped or rolled back the committed value"
    );

    CVar::Registration*      self_registration = nullptr;
    CVar::RegistrationResult self_removing     = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.SelfRemoving"},
        0,
        [&self_registration](std::int64_t, std::int64_t) {
            self_registration->Reset();
        }
    );
    self_registration = &self_removing.registration;
    Expect(
        CVar::SetValueFromString("Test.Callback.SelfRemoving", "1").Succeeded(),
        "self-unregistering callback invalidated in-flight storage"
    );
    Expect(
        !CVar::Find("Test.Callback.SelfRemoving"), "self-unregistering callback left the cvar discoverable"
    );

    std::atomic_uint32_t     cross_reset_entered{0};
    CVar::Registration*      cross_a_registration = nullptr;
    CVar::Registration*      cross_b_registration = nullptr;
    CVar::RegistrationResult cross_a              = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.CrossResetA"},
        0,
        [&cross_reset_entered, &cross_b_registration](std::int64_t, std::int64_t) {
            cross_reset_entered.fetch_add(1, std::memory_order_acq_rel);
            while (cross_reset_entered.load(std::memory_order_acquire) != 2) {
                std::this_thread::yield();
            }
            cross_b_registration->Reset();
        }
    );
    CVar::RegistrationResult cross_b = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.CrossResetB"},
        0,
        [&cross_reset_entered, &cross_a_registration](std::int64_t, std::int64_t) {
            cross_reset_entered.fetch_add(1, std::memory_order_acq_rel);
            while (cross_reset_entered.load(std::memory_order_acquire) != 2) {
                std::this_thread::yield();
            }
            cross_a_registration->Reset();
        }
    );
    Expect(cross_a.Succeeded() && cross_b.Succeeded(), "cross-reset callback fixture did not register");
    cross_a_registration = &cross_a.registration;
    cross_b_registration = &cross_b.registration;
    std::atomic_bool cross_a_succeeded{false};
    std::atomic_bool cross_b_succeeded{false};
    std::jthread     cross_a_setter([&] {
        cross_a_succeeded.store(
            CVar::SetValueFromString("Test.Callback.CrossResetA", "1").Succeeded(), std::memory_order_release
        );
    });
    std::jthread     cross_b_setter([&] {
        cross_b_succeeded.store(
            CVar::SetValueFromString("Test.Callback.CrossResetB", "1").Succeeded(), std::memory_order_release
        );
    });
    cross_a_setter.join();
    cross_b_setter.join();
    Expect(
        cross_a.status == CVar::ERegistrationStatus::Registered &&
            cross_b.status == CVar::ERegistrationStatus::Registered &&
            cross_a_succeeded.load(std::memory_order_acquire) &&
            cross_b_succeeded.load(std::memory_order_acquire) && !CVar::Find("Test.Callback.CrossResetA") &&
            !CVar::Find("Test.Callback.CrossResetB"),
        "cross-resetting callbacks deadlocked or left a registration active"
    );

    std::atomic_bool         blocking_entered{false};
    std::atomic_bool         release_blocking{false};
    std::atomic_bool         setter_succeeded{false};
    std::atomic_bool         reset_returned{false};
    CVar::RegistrationResult blocking = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.Quiescence"},
        0,
        [&blocking_entered, &release_blocking](std::int64_t, std::int64_t) {
            blocking_entered.store(true, std::memory_order_release);
            while (!release_blocking.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    );
    std::jthread setter([&] {
        setter_succeeded.store(
            CVar::SetValueFromString("Test.Callback.Quiescence", "1").Succeeded(), std::memory_order_release
        );
    });
    while (!blocking_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::jthread resetter([&] {
        blocking.registration.Reset();
        reset_returned.store(true, std::memory_order_release);
    });
    while (CVar::Find("Test.Callback.Quiescence")) {
        std::this_thread::yield();
    }
    Expect(
        !reset_returned.load(std::memory_order_acquire),
        "Reset returned before an in-flight callback quiesced"
    );
    release_blocking.store(true, std::memory_order_release);
    setter.join();
    resetter.join();
    Expect(
        setter_succeeded.load(std::memory_order_acquire) && reset_returned.load(std::memory_order_acquire),
        "callback update or quiescent Reset did not finish"
    );

    std::atomic_bool          first_callback_entered{false};
    std::atomic_bool          release_first_callback{false};
    std::mutex                callback_order_mutex;
    std::vector<std::int64_t> callback_order;
    CVar::RegistrationResult  ordered = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.Ordered"},
        0,
        [&first_callback_entered, &release_first_callback, &callback_order_mutex, &callback_order](
            std::int64_t, std::int64_t _new_value
        ) {
            if (_new_value == 1) {
                first_callback_entered.store(true, std::memory_order_release);
                while (!release_first_callback.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            std::lock_guard lock(callback_order_mutex);
            callback_order.push_back(_new_value);
        }
    );
    std::jthread first_setter([&] {
        CVar::SetValueFromString("Test.Callback.Ordered", "1");
    });
    while (!first_callback_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::jthread second_setter([&] {
        CVar::SetValueFromString("Test.Callback.Ordered", "2");
    });
    release_first_callback.store(true, std::memory_order_release);
    first_setter.join();
    second_setter.join();
    Expect(
        ordered.Succeeded() && callback_order == std::vector<std::int64_t>{1, 2} &&
            CVar::Find("Test.Callback.Ordered")->value == "2",
        "concurrent updates reordered callbacks relative to commits"
    );

    std::atomic_bool          joined_update_deferred{false};
    std::mutex                joined_order_mutex;
    std::vector<std::int64_t> joined_order;
    CVar::RegistrationResult  joined = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.JoinedSetter"},
        0,
        [&joined_update_deferred, &joined_order_mutex, &joined_order](std::int64_t, std::int64_t _new_value) {
            {
                std::lock_guard lock(joined_order_mutex);
                joined_order.push_back(_new_value);
            }
            if (_new_value != 1) {
                return;
            }
            std::jthread concurrent_setter([&joined_update_deferred] {
                const CVar::CVarSetResult result =
                    CVar::SetValueFromString("Test.Callback.JoinedSetter", "2");
                joined_update_deferred.store(
                    result.Succeeded() && result.callback_deferred, std::memory_order_release
                );
            });
            concurrent_setter.join();
        }
    );
    const CVar::CVarSetResult joined_result = CVar::SetValueFromString("Test.Callback.JoinedSetter", "1");
    Expect(
        joined.Succeeded() && joined_result.Succeeded() && !joined_result.callback_deferred &&
            joined_update_deferred.load(std::memory_order_acquire) &&
            joined_order == std::vector<std::int64_t>{1, 2} &&
            CVar::Find("Test.Callback.JoinedSetter")->value == "2",
        "callback could not join a concurrent setter without deadlock or FIFO loss"
    );

    std::atomic_uint32_t     bounded_callback_count{0};
    std::atomic_uint32_t     bounded_deferred_count{0};
    std::atomic_uint32_t     bounded_rejected_count{0};
    CVar::RegistrationResult bounded_callbacks = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name                     = "Test.Callback.BoundedDispatch",
            .callback_dispatch_budget = 3,
        },
        0,
        [&bounded_callback_count,
         &bounded_deferred_count,
         &bounded_rejected_count](std::int64_t, std::int64_t _new_value) {
            bounded_callback_count.fetch_add(1, std::memory_order_relaxed);
            if (_new_value >= 4) {
                return;
            }
            const CVar::CVarSetResult result =
                CVar::SetValueFromString("Test.Callback.BoundedDispatch", std::to_string(_new_value + 1));
            if (result.callback_deferred) {
                bounded_deferred_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (result.status == CVar::ESetStatus::CallbackQueueFull) {
                bounded_rejected_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    );
    const CVar::CVarSetResult bounded_result = CVar::SetValueFromString("Test.Callback.BoundedDispatch", "1");
    const std::optional<CVar::CVarSnapshot> bounded_snapshot = CVar::Find("Test.Callback.BoundedDispatch");
    Expect(
        bounded_callbacks.Succeeded() && bounded_result.Succeeded() &&
            bounded_callback_count.load(std::memory_order_acquire) == 3 &&
            bounded_deferred_count.load(std::memory_order_acquire) == 2 &&
            bounded_rejected_count.load(std::memory_order_acquire) == 1 && bounded_snapshot &&
            bounded_snapshot->value == "3" && bounded_snapshot->callback_dispatch_budget == 3,
        "callback dispatch budget did not bound queue growth and drainer lifetime"
    );
    const CVar::CVarSetResult bounded_next_epoch =
        CVar::SetValueFromString("Test.Callback.BoundedDispatch", "10");
    Expect(
        bounded_next_epoch.Succeeded() && !bounded_next_epoch.callback_deferred &&
            bounded_callback_count.load(std::memory_order_acquire) == 4 &&
            bounded_rejected_count.load(std::memory_order_acquire) == 1 &&
            CVar::Find("Test.Callback.BoundedDispatch")->value == "10",
        "callback dispatch budget did not reset for the next idle-to-dispatch epoch"
    );

    std::atomic_uint32_t     queued_callback_count{0};
    std::atomic_bool         queued_first_entered{false};
    std::atomic_bool         allow_self_reset{false};
    std::atomic_bool         queued_second_deferred{false};
    CVar::Registration*      queued_registration = nullptr;
    CVar::RegistrationResult queued              = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Callback.QueuedAfterReset"},
        0,
        [&queued_callback_count, &queued_first_entered, &allow_self_reset, &queued_registration](
            std::int64_t, std::int64_t _new_value
        ) {
            queued_callback_count.fetch_add(1, std::memory_order_relaxed);
            if (_new_value != 1) {
                return;
            }
            queued_first_entered.store(true, std::memory_order_release);
            while (!allow_self_reset.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            queued_registration->Reset();
        }
    );
    queued_registration = &queued.registration;
    std::jthread queued_first([&] {
        CVar::SetValueFromString("Test.Callback.QueuedAfterReset", "1");
    });
    while (!queued_first_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::jthread queued_second([&] {
        const CVar::CVarSetResult result = CVar::SetValueFromString("Test.Callback.QueuedAfterReset", "2");
        queued_second_deferred.store(
            result.Succeeded() && result.callback_deferred, std::memory_order_release
        );
    });
    while (!queued_second_deferred.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    allow_self_reset.store(true, std::memory_order_release);
    queued_first.join();
    queued_second.join();
    Expect(
        queued_second_deferred.load(std::memory_order_acquire) &&
            queued_callback_count.load(std::memory_order_acquire) == 1 &&
            !CVar::Find("Test.Callback.QueuedAfterReset"),
        "self-Reset allowed a queued mutation to invoke a late callback"
    );

    CVar::RegistrationResult visitor_blocker =
        CVar::RegisterBool(CVar::CVarDescriptor{.name = "Test.Visitor.Pin.Blocker"}, false);
    std::atomic_uint32_t            pinned_callback_destroyed{0};
    std::shared_ptr<CountOnDestroy> pinned_lifetime = std::make_shared<CountOnDestroy>();
    pinned_lifetime->count                          = &pinned_callback_destroyed;
    auto pinned_callback                            = [pinned_lifetime](std::int64_t, std::int64_t) {};
    pinned_lifetime.reset();
    CVar::RegistrationResult visitor_target = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Test.Visitor.Pin.Target"}, 0, std::move(pinned_callback)
    );
    struct VisitorPinContext {
        std::atomic_bool entered{false};
        std::atomic_bool release{false};
    } visitor_context;
    std::jthread visitor([&] {
        CVar::Detail::VisitAllSnapshotsRaw(
            "Test.Visitor.Pin.",
            [](const CVar::CVarSnapshotView& _snapshot, void* _context) {
                auto& context = *static_cast<VisitorPinContext*>(_context);
                if (_snapshot.name != "Test.Visitor.Pin.Blocker") {
                    return;
                }
                context.entered.store(true, std::memory_order_release);
                while (!context.release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            },
            &visitor_context
        );
    });
    while (!visitor_context.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    visitor_target.registration.Reset();
    Expect(
        pinned_callback_destroyed.load(std::memory_order_acquire) == 1,
        "Reset deferred caller callback destruction behind a snapshot pin"
    );
    visitor_context.release.store(true, std::memory_order_release);
    visitor.join();
}

void TestListingAndCandidates() {
    CVar::RegistrationResult z = CVar::RegisterBool(
        CVar::CVarDescriptor{
            .name   = "Test.List.zulu",
            .helper = "z helper",
        },
        false
    );
    CVar::RegistrationResult a = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name   = "Test.List.Alpha",
            .helper = "a helper",
        },
        1
    );
    CVar::RegistrationResult b = CVar::RegisterString(
        CVar::CVarDescriptor{
            .name   = "Test.List.bravo",
            .helper = "b helper",
        },
        "value"
    );
    Expect(z.Succeeded() && a.Succeeded() && b.Succeeded(), "list fixtures did not register");

    const std::vector<CVar::CVarSnapshot> listed = CVar::List("TEST.LIST.");
    Expect(listed.size() == 3, "case-insensitive prefix list missed cvars");
    Expect(
        listed[0].name == "Test.List.Alpha" && listed[1].name == "Test.List.bravo" &&
            listed[2].name == "Test.List.zulu",
        "cvar list was not deterministically case-insensitive sorted"
    );

    Command::EngineCommandProcessor              processor;
    const std::vector<Command::CommandCandidate> cvar_candidates = processor.GetCandidates("test.list.");
    Expect(
        cvar_candidates.size() == 3 && cvar_candidates.front().text == "Test.List.Alpha" &&
            !cvar_candidates.front().is_command,
        "cvar autocomplete was incomplete or unsorted"
    );
    const std::vector<Command::CommandCandidate> command_candidates = processor.GetCandidates("/CV");
    Expect(
        command_candidates.size() == 1 && command_candidates.front().text == "/cvar.list" &&
            command_candidates.front().is_command,
        "command autocomplete was not case-insensitive"
    );
    Expect(processor.GetCandidates("test.list.", 2).size() == 2, "autocomplete ignored its result bound");
}

void TestCommandProcessing() {
    CVar::RegistrationResult toggle = CVar::RegisterBool(
        CVar::CVarDescriptor{
            .name         = "Cmd.Toggle",
            .helper       = "toggle helper",
            .true_helper  = "toggle on",
            .false_helper = "toggle off",
        },
        false
    );
    CVar::RegistrationResult count = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name      = "Cmd.Count",
            .helper    = "count helper",
            .min_value = 0.0,
            .max_value = 5.0,
        },
        1
    );
    CVar::RegistrationResult text = CVar::RegisterString(CVar::CVarDescriptor{.name = "Cmd.Text"}, "initial");
    Command::EngineCommandProcessor*     reentrant_processor = nullptr;
    std::atomic<Command::EProcessStatus> reentrant_process_status{Command::EProcessStatus::Empty};
    CVar::RegistrationResult             reentrant = CVar::RegisterInt(
        CVar::CVarDescriptor{.name = "Cmd.Reentrant"},
        0,
        [&reentrant_processor, &reentrant_process_status](std::int64_t, std::int64_t) {
            Expect(
                reentrant_processor->SubmitText("/help") == Command::ESubmitStatus::Accepted,
                "callback could not submit a follow-up command"
            );
            reentrant_process_status.store(
                reentrant_processor->ProcessPending().status, std::memory_order_release
            );
        }
    );
    Expect(
        toggle.Succeeded() && count.Succeeded() && text.Succeeded() && reentrant.Succeeded(),
        "command fixtures did not register"
    );

    Command::EngineCommandProcessor processor({.pending_capacity = 32, .output_capacity = 128});
    reentrant_processor = &processor;
    Expect(processor.SubmitText("   ") == Command::ESubmitStatus::Empty, "empty command was queued");
    const std::vector<std::string_view> commands = {
        "Cmd.Toggle on",
        "Cmd.Toggle",
        "Cmd.Toggle ?",
        "Cmd.Count = 5",
        "Cmd.Count 6",
        "Cmd.Count nope",
        "Cmd.Text hello world",
        "Cmd.Text =",
        "Cmd.Text question?",
        "Cmd.Reentrant 1",
        "/help",
        "/cvar.list cmd.",
        "/cvar.list absent.",
        "/unknown",
        "missing.cvar",
    };
    for (const std::string_view command : commands) {
        Expect(
            processor.SubmitText(command) == Command::ESubmitStatus::Accepted, "valid command was not queued"
        );
    }
    const Command::ProcessPendingResult first_drain  = processor.ProcessPending(5);
    const Command::ProcessPendingResult second_drain = processor.ProcessPending();
    Expect(
        first_drain.status == Command::EProcessStatus::Processed && first_drain.processed == 5 &&
            second_drain.status == Command::EProcessStatus::Processed &&
            second_drain.processed == commands.size() - 5,
        "bounded command processing returned the wrong result"
    );
    Expect(
        CVar::Find("Cmd.Toggle")->value == "true" && CVar::Find("Cmd.Count")->value == "5" &&
            CVar::Find("Cmd.Text")->value == "question?",
        "command execution produced the wrong cvar values"
    );
    Expect(
        reentrant_process_status.load(std::memory_order_acquire) == Command::EProcessStatus::Busy,
        "callback re-entry into ProcessPending deadlocked or consumed work"
    );
    Expect(
        processor.ProcessPending().processed == 1,
        "callback-submitted command was not retained for the next drain"
    );
    Expect(
        processor.ProcessPending(0).status == Command::EProcessStatus::MaxCommandsZero,
        "zero process budget was not distinguishable"
    );

    const Command::CommandOutputBatch output = processor.PollOutput();
    Expect(
        OutputContains(output, "Cmd.Toggle = true") && OutputContains(output, "toggle helper") &&
            OutputContains(output, "Commands:") &&
            OutputContains(output, "No cvar matches prefix: absent.") &&
            OutputContains(output, "unknown command") && OutputContains(output, "unknown cvar") &&
            OutputContains(output, "above the allowed maximum") && OutputContains(output, "expected integer"),
        "command output missed query/help/list/error diagnostics"
    );
    Expect(
        processor.PollOutput(output.next_sequence).lines.empty(),
        "output cursor replayed already-consumed lines"
    );
}

void TestBoundedQueuesAndOutputCursor() {
    Command::EngineCommandProcessor processor({.pending_capacity = 32, .output_capacity = 3});
    std::atomic_uint32_t            accepted{0};
    std::atomic_uint32_t            full{0};
    std::vector<std::jthread>       producers;
    for (std::uint32_t thread_index = 0; thread_index < 8; ++thread_index) {
        producers.emplace_back([&processor, &accepted, &full] {
            for (std::uint32_t index = 0; index < 64; ++index) {
                const Command::ESubmitStatus status = processor.SubmitText("/unknown");
                if (status == Command::ESubmitStatus::Accepted) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else if (status == Command::ESubmitStatus::QueueFull) {
                    full.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    producers.clear();
    Expect(
        accepted.load(std::memory_order_relaxed) == 32 && full.load(std::memory_order_relaxed) == 480,
        "concurrent producers exceeded or underfilled the pending bound"
    );
    Expect(processor.ProcessPending().processed == 32, "bounded pending queue lost accepted commands");

    const Command::CommandOutputBatch retained = processor.PollOutput(1);
    Expect(
        retained.dropped_count == 29 && retained.lines.size() == 3 && retained.lines.front().sequence == 30 &&
            retained.next_sequence == 33,
        "output ring did not report overwritten cursor entries"
    );
    const Command::CommandOutputBatch limited = processor.PollOutput(30, 2);
    Expect(
        limited.lines.size() == 2 && limited.next_sequence == 32, "output polling ignored its result bound"
    );

    processor.ClearOutput();
    const Command::CommandOutputBatch cleared = processor.PollOutput(1);
    Expect(
        cleared.lines.empty() && cleared.dropped_count == 32 &&
            cleared.next_sequence == retained.next_sequence,
        "clearing output did not advance an older cursor across the removed sequence range"
    );
    Expect(
        processor.SubmitText("/still-unknown") == Command::ESubmitStatus::Accepted &&
            processor.ProcessPending().processed == 1,
        "processor did not recover after clearing output"
    );
    const Command::CommandOutputBatch after_clear = processor.PollOutput(retained.next_sequence);
    Expect(
        after_clear.lines.size() == 1 && after_clear.lines.front().sequence == 33 &&
            after_clear.dropped_count == 0,
        "clear output reset sequence identity or lost new output"
    );

    Command::EngineCommandProcessor zero_limits({.pending_capacity = 0, .output_capacity = 0});
    Expect(
        zero_limits.SubmitText("/unknown") == Command::ESubmitStatus::Accepted &&
            zero_limits.SubmitText("/unknown") == Command::ESubmitStatus::QueueFull &&
            zero_limits.ProcessPending().processed == 1 && zero_limits.PollOutput().lines.size() == 1,
        "documented zero-capacity normalization changed"
    );
}

void TestStartupSealLast() {
    CVar::RegistrationResult startup = CVar::RegisterInt(
        CVar::CVarDescriptor{
            .name  = "Test.Startup.BeforeSeal",
            .flags = CVar::EFlags::StartupOnly,
        },
        1
    );
    Expect(startup.Succeeded(), "startup-only cvar did not register");
    Expect(
        CVar::SetValueFromString("Test.Startup.BeforeSeal", "2", CVar::ESetSource::Runtime).status ==
            CVar::ESetStatus::StartupSealed,
        "runtime source changed a startup-only cvar"
    );
    Expect(
        CVar::SetValueFromString("Test.Startup.BeforeSeal", "2", CVar::ESetSource::StartupConfig).Succeeded(),
        "startup config could not set an unsealed startup-only cvar"
    );

    CVar::SealStartupOnlyCVars();
    Expect(
        CVar::IsStartupConfigurationSealed() && CVar::Find("Test.Startup.BeforeSeal")->startup_sealed,
        "global startup seal did not seal an existing cvar"
    );
    Expect(
        CVar::SetValueFromString("Test.Startup.BeforeSeal", "3", CVar::ESetSource::StartupConfig).status ==
            CVar::ESetStatus::StartupSealed,
        "sealed startup-only cvar accepted a config update"
    );

    CVar::RegistrationResult late = CVar::RegisterBool(
        CVar::CVarDescriptor{
            .name  = "Test.Startup.Late",
            .flags = CVar::EFlags::StartupOnly,
        },
        false
    );
    Expect(
        late.Succeeded() && CVar::Find("Test.Startup.Late")->startup_sealed &&
            CVar::SetValueFromString("Test.Startup.Late", "true", CVar::ESetSource::StartupConfig).status ==
                CVar::ESetStatus::StartupSealed,
        "late startup-only registration escaped the global seal"
    );

    CVar::RegistrationResult dynamic =
        CVar::RegisterBool(CVar::CVarDescriptor{.name = "Test.Startup.Dynamic"}, false);
    Expect(
        dynamic.Succeeded() && !CVar::Find("Test.Startup.Dynamic")->startup_sealed &&
            CVar::SetValueFromString("Test.Startup.Dynamic", "true").Succeeded(),
        "global startup seal affected a dynamic cvar"
    );
}

} // namespace

int main() {
    try {
        TestRegistrationLifetimeAndDescriptors();
        TestTypedParsingAndRanges();
        TestFlagsOwnerSyncAndCallbacks();
        TestListingAndCandidates();
        TestCommandProcessing();
        TestBoundedQueuesAndOutputCursor();
        TestStartupSealLast();
        std::cout << "CVar/command core contract tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "CVar/command core contract test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
