#pragma once

#include <cstdint>
#include <limits>

namespace Moer::Render {

enum class BindlessUpdateSlotState : std::uint8_t {
    Stale,
    Original,
    ClaimedByCommand,
};

enum class BindlessUpdateFinalizationAction : std::uint8_t {
    Ignore,
    Retire,
    Restore,
};

// A bindless mutation owns two slots: one entry in the shader-visible handle
// array and one entry in the typed descriptor array. Treat the pair as one
// lifecycle unit so an old command can never act on only half of a reused
// binding.
constexpr BindlessUpdateSlotState ClassifyBindlessUpdateSlot(
    std::uint64_t _current_array_generation,
    std::uint64_t _current_typed_generation,
    std::uint64_t _array_claim_token,
    std::uint64_t _typed_claim_token,
    std::uint64_t _command_array_generation,
    std::uint64_t _command_typed_generation,
    std::uint64_t _command_token
) {
    if (_command_token == 0) {
        return BindlessUpdateSlotState::Stale;
    }

    if (_current_array_generation == _command_array_generation &&
        _current_typed_generation == _command_typed_generation &&
        _array_claim_token == 0 && _typed_claim_token == 0) {
        return BindlessUpdateSlotState::Original;
    }

    if (_command_array_generation != std::numeric_limits<std::uint64_t>::max() &&
        _command_typed_generation != std::numeric_limits<std::uint64_t>::max() &&
        _current_array_generation == _command_array_generation + 1 &&
        _current_typed_generation == _command_typed_generation + 1 &&
        _array_claim_token == _command_token && _typed_claim_token == _command_token) {
        return BindlessUpdateSlotState::ClaimedByCommand;
    }

    return BindlessUpdateSlotState::Stale;
}

constexpr BindlessUpdateFinalizationAction GetBindlessUpdateFinalizationAction(
    BindlessUpdateSlotState _state,
    bool                    _is_free,
    bool                    _gpu_update_succeeded
) {
    if (_state == BindlessUpdateSlotState::Stale) {
        return BindlessUpdateFinalizationAction::Ignore;
    }
    if (_gpu_update_succeeded) {
        return _is_free ? BindlessUpdateFinalizationAction::Retire
                        : BindlessUpdateFinalizationAction::Ignore;
    }
    return _is_free ? BindlessUpdateFinalizationAction::Restore
                    : BindlessUpdateFinalizationAction::Retire;
}

constexpr bool RollbackBindlessUpdateSlotClaim(
    std::uint64_t& _current_array_generation,
    std::uint64_t& _current_typed_generation,
    std::uint64_t& _array_claim_token,
    std::uint64_t& _typed_claim_token,
    std::uint64_t  _command_array_generation,
    std::uint64_t  _command_typed_generation,
    std::uint64_t  _command_token
) {
    if (ClassifyBindlessUpdateSlot(
            _current_array_generation,
            _current_typed_generation,
            _array_claim_token,
            _typed_claim_token,
            _command_array_generation,
            _command_typed_generation,
            _command_token
        ) != BindlessUpdateSlotState::ClaimedByCommand) {
        return false;
    }
    _current_array_generation = _command_array_generation;
    _current_typed_generation = _command_typed_generation;
    _array_claim_token         = 0;
    _typed_claim_token         = 0;
    return true;
}

} // namespace Moer::Render
