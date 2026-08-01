#pragma once

#include "RenderGraph.h"
#include "misc/STL.h"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Moer::Render {

namespace Detail {

enum class ERGSetupState : uint8_t {
    Pending,
    Succeeded,
    Failed,
};

/** Write-once storage shared by one setup producer and immutable consumers. */
template<typename T>
class RGSetupState {
public:
    RGSetupState() = default;

    RGSetupState(const RGSetupState&)            = delete;
    RGSetupState& operator=(const RGSetupState&) = delete;

    template<typename Value>
        requires std::constructible_from<T, Value&&>
    void Publish(Value&& published_value) {
        std::lock_guard lock(mutex);
        if (state != ERGSetupState::Pending) {
            throw std::logic_error("RDG setup result was published more than once");
        }
        value.emplace(std::forward<Value>(published_value));
        state = ERGSetupState::Succeeded;
    }

    void Fail(std::string_view message) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (state != ERGSetupState::Pending) {
                return;
            }
            // Terminal state is independent from best-effort diagnostics. An
            // allocation failure while retaining the message must never leave
            // a prepared value Pending.
            state = ERGSetupState::Failed;
            try {
                error.assign(message);
            } catch (...) {
                error.clear();
            }
        } catch (...) {
        }
    }

    void AnnotateFailure(std::string_view message) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (state != ERGSetupState::Failed) {
                return;
            }
            try {
                error.assign(message);
            } catch (...) {
            }
        } catch (...) {
        }
    }

    [[nodiscard]] const T& Get() const {
        std::lock_guard lock(mutex);
        if (state == ERGSetupState::Pending) {
            throw std::logic_error(
                "RDG prepared value was read before setup completed"
            );
        }
        if (state == ERGSetupState::Failed) {
            throw std::runtime_error(
                error.empty() ? "RDG setup failed" : error
            );
        }
        return *value;
    }

    [[nodiscard]] bool IsReady() const {
        std::lock_guard lock(mutex);
        return state == ERGSetupState::Succeeded;
    }

    [[nodiscard]] bool HasFailed() const {
        std::lock_guard lock(mutex);
        return state == ERGSetupState::Failed;
    }

    [[nodiscard]] std::string GetError() const {
        std::lock_guard lock(mutex);
        return error;
    }

private:
    mutable std::mutex mutex{};
    std::optional<T>   value{};
    std::string        error{};
    ERGSetupState      state = ERGSetupState::Pending;
};

} // namespace Detail

/**
 * Immutable read capability for a graph-owned CPU setup result.
 *
 * A valid value becomes readable only after Compile() has joined setup work.
 * Copies share ownership and may be consumed concurrently by recording tasks.
 */
template<typename T>
class RGPreparedValue {
public:
    RGPreparedValue() = default;

    [[nodiscard]] bool IsValid() const {
        return static_cast<bool>(state);
    }

    [[nodiscard]] bool IsReady() const {
        return state && state->IsReady();
    }

    [[nodiscard]] bool HasFailed() const {
        return state && state->HasFailed();
    }

    [[nodiscard]] std::string GetError() const {
        return state ? state->GetError() : "RDG prepared value is invalid";
    }

    [[nodiscard]] const T& Get() const & {
        if (!state) {
            throw std::logic_error("RDG prepared value is invalid");
        }
        return state->Get();
    }
    [[nodiscard]] const T& Get() const && = delete;

private:
    friend class RenderGraph;

    explicit RGPreparedValue(
        SharedPtr<const Detail::RGSetupState<T>> setup_state
    ) : state(std::move(setup_state)) {}

    SharedPtr<const Detail::RGSetupState<T>> state{};
};

template<typename Input, typename Prepare>
    requires std::invocable<
                 std::decay_t<Prepare>&,
                 const std::decay_t<Input>&> &&
             (!std::is_void_v<RGSetupResult<Input, Prepare>>) &&
             (!std::is_reference_v<
                 std::invoke_result_t<
                     std::decay_t<Prepare>&,
                     const std::decay_t<Input>&>>) &&
             std::move_constructible<RGSetupResult<Input, Prepare>> &&
             std::constructible_from<std::decay_t<Input>, Input&&> &&
             std::constructible_from<std::decay_t<Prepare>, Prepare&&>
auto RenderGraph::AddSetupPass(
    std::string_view name,
    Input&&          immutable_input,
    Prepare&&        prepare
) -> RGPreparedValue<RGSetupResult<Input, Prepare>> {
    using InputType   = std::decay_t<Input>;
    using PrepareType = std::decay_t<Prepare>;
    using ResultType  = RGSetupResult<Input, Prepare>;
    using StateType   = Detail::RGSetupState<ResultType>;

    if constexpr (std::is_pointer_v<PrepareType>) {
        if (prepare == nullptr) {
            RegisterSetupPass(name, {}, {}, {});
            return {};
        }
    } else if constexpr (requires(const PrepareType& candidate) {
                             { candidate.operator bool() } -> std::same_as<bool>;
                         }) {
        if (!prepare.operator bool()) {
            RegisterSetupPass(name, {}, {}, {});
            return {};
        }
    }

    auto state = std::make_shared<StateType>();
    SharedPtr<const InputType> input_owner = std::make_shared<InputType>(
        std::forward<Input>(immutable_input)
    );
    auto prepare_owner = std::make_shared<PrepareType>(
        std::forward<Prepare>(prepare)
    );

    const bool registered = RegisterSetupPass(
        name,
        [state, input_owner, prepare_owner] {
            state->Publish(std::invoke(*prepare_owner, *input_owner));
        },
        [state](std::string_view error) { state->Fail(error); },
        [state](std::string_view error) { state->AnnotateFailure(error); }
    );
    if (!registered) {
        return {};
    }
    return RGPreparedValue<ResultType>(std::move(state));
}

} // namespace Moer::Render
