#pragma once

#include "rhi/RHISubmissionTopology.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Moer::Render::VulkanTranslateWaveDetail {

// CPU-only input used to schedule Vulkan translation work. native_queue_id is
// the physical queue identity from RHIQueueTopology, not the logical
// EQueueType. Dependencies are explicit topology edges and must point to an
// earlier node in this stable input order.
struct TranslateWaveNode {
    SubmissionKey                  key{};
    uint32_t                       native_queue_id{0};
    bool                           async_translate{true};
    std::span<const SubmissionKey> dependencies{};
};

enum class ETranslateWaveBuildError : uint8_t {
    None = 0,
    DuplicateKey,
    UnknownDependency,
    FutureDependency,
};

enum class ETranslateWaveNodeState : uint8_t {
    Pending = 0,
    Translating,
    Translated,
    Released,
    Cancelled,
};

// Builds stable translation waves without owning threads or Vulkan objects.
//
// Explicit topology dependencies are satisfied by MarkTranslated, because
// their CPU prerequisite is complete at that point. Physical-queue ordering is
// stricter: a node sharing native_queue_id with an earlier node waits for
// MarkReleased, which represents transfer of that packet lease to Submit or
// Reject. A non-async node is an exclusive Copy/control boundary; it waits for
// the complete earlier prefix to be released and blocks the later suffix until
// it is released.
//
// NextWave scans the complete stable input and emits every currently-ready
// native lane. A blocked same-native node therefore cannot hide a later,
// independent lane. The caller still owns final ordered release/submission:
// translated packets may remain held until all earlier stable keys are ready.
class TranslateWaveScheduler final {
public:
    static constexpr size_t NoIndex = std::numeric_limits<size_t>::max();

    [[nodiscard]] bool Build(std::span<const TranslateWaveNode> _input) {
        Reset();

        nodes.reserve(_input.size());
        for (size_t input_index = 0; input_index < _input.size(); ++input_index) {
            const TranslateWaveNode& input = _input[input_index];
            if (FindNodeIndex(input.key) != NoIndex) {
                return Fail(ETranslateWaveBuildError::DuplicateKey, input_index, NoIndex);
            }
            nodes.emplace_back(InternalNode{
                .key             = input.key,
                .native_queue_id = input.native_queue_id,
                .async_translate = input.async_translate,
            });
        }

        size_t last_hard_boundary = NoIndex;
        for (size_t input_index = 0; input_index < _input.size(); ++input_index) {
            InternalNode& node = nodes[input_index];

            for (size_t dependency_index = 0; dependency_index < _input[input_index].dependencies.size();
                 ++dependency_index) {
                const SubmissionKey dependency_key  = _input[input_index].dependencies[dependency_index];
                const size_t        dependency_node = FindNodeIndex(dependency_key);
                if (dependency_node == NoIndex) {
                    return Fail(ETranslateWaveBuildError::UnknownDependency, input_index, dependency_index);
                }
                if (dependency_node >= input_index) {
                    return Fail(ETranslateWaveBuildError::FutureDependency, input_index, dependency_index);
                }

                bool duplicate_dependency = false;
                for (const size_t existing : node.dependencies) {
                    if (existing == dependency_node) {
                        duplicate_dependency = true;
                        break;
                    }
                }
                if (!duplicate_dependency) {
                    node.dependencies.emplace_back(dependency_node);
                }
            }

            for (size_t predecessor = input_index; predecessor > 0; --predecessor) {
                const size_t candidate = predecessor - 1;
                if (nodes[candidate].native_queue_id == node.native_queue_id) {
                    node.same_native_queue_predecessor = candidate;
                    break;
                }
            }

            node.hard_boundary_predecessor = last_hard_boundary;
            if (!node.async_translate) {
                last_hard_boundary = input_index;
            }
        }

        built = true;
        return true;
    }

    [[nodiscard]] std::vector<SubmissionKey> NextWave() {
        std::vector<SubmissionKey> wave;
        if (!built || cancelled) {
            return wave;
        }

        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            InternalNode& node = nodes[node_index];
            if (!IsReady(node_index)) {
                continue;
            }

            node.state = ETranslateWaveNodeState::Translating;
            wave.emplace_back(node.key);

            // Copy/control work is ready only after its full earlier prefix
            // has released, and it remains an exclusive wave.
            if (!node.async_translate) {
                break;
            }
        }
        return wave;
    }

    [[nodiscard]] bool MarkTranslated(const SubmissionKey& _key) noexcept {
        InternalNode* node = FindNode(_key);
        if (node == nullptr || node->state != ETranslateWaveNodeState::Translating) {
            return false;
        }
        node->state = ETranslateWaveNodeState::Translated;
        return true;
    }

    [[nodiscard]] bool MarkReleased(const SubmissionKey& _key) noexcept {
        InternalNode* node = FindNode(_key);
        if (node == nullptr) {
            return false;
        }

        if (node->state == ETranslateWaveNodeState::Translated ||
            (cancelled && node->state == ETranslateWaveNodeState::Translating)) {
            node->state = ETranslateWaveNodeState::Released;
            return true;
        }
        return false;
    }

    // Pending suffix nodes have no packet lease and can be terminalized
    // immediately. In-flight or translated nodes retain their state so the
    // owner can still release their packet exactly once.
    void Cancel() noexcept {
        if (!built || cancelled) {
            return;
        }
        cancelled = true;
        for (InternalNode& node : nodes) {
            if (node.state == ETranslateWaveNodeState::Pending) {
                node.state = ETranslateWaveNodeState::Cancelled;
            }
        }
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return built && build_error == ETranslateWaveBuildError::None;
    }

    [[nodiscard]] bool IsCancelled() const noexcept {
        return cancelled;
    }

    [[nodiscard]] bool IsComplete() const noexcept {
        if (!built) {
            return false;
        }
        for (const InternalNode& node : nodes) {
            if (node.state != ETranslateWaveNodeState::Released &&
                node.state != ETranslateWaveNodeState::Cancelled) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] ETranslateWaveBuildError GetBuildError() const noexcept {
        return build_error;
    }

    [[nodiscard]] size_t GetErrorNodeIndex() const noexcept {
        return error_node_index;
    }

    [[nodiscard]] size_t GetErrorDependencyIndex() const noexcept {
        return error_dependency_index;
    }

    [[nodiscard]] std::optional<ETranslateWaveNodeState> GetState(const SubmissionKey& _key) const noexcept {
        const InternalNode* node = FindNode(_key);
        if (node == nullptr) {
            return std::nullopt;
        }
        return node->state;
    }

private:
    struct InternalNode {
        SubmissionKey           key{};
        uint32_t                native_queue_id{0};
        bool                    async_translate{true};
        std::vector<size_t>     dependencies{};
        size_t                  same_native_queue_predecessor{NoIndex};
        size_t                  hard_boundary_predecessor{NoIndex};
        ETranslateWaveNodeState state{ETranslateWaveNodeState::Pending};
    };

    void Reset() noexcept {
        nodes.clear();
        build_error            = ETranslateWaveBuildError::None;
        error_node_index       = NoIndex;
        error_dependency_index = NoIndex;
        built                  = false;
        cancelled              = false;
    }

    [[nodiscard]] bool
    Fail(ETranslateWaveBuildError _error, size_t _node_index, size_t _dependency_index) noexcept {
        nodes.clear();
        build_error            = _error;
        error_node_index       = _node_index;
        error_dependency_index = _dependency_index;
        built                  = false;
        cancelled              = false;
        return false;
    }

    [[nodiscard]] size_t FindNodeIndex(const SubmissionKey& _key) const noexcept {
        for (size_t index = 0; index < nodes.size(); ++index) {
            if (nodes[index].key == _key) {
                return index;
            }
        }
        return NoIndex;
    }

    [[nodiscard]] InternalNode* FindNode(const SubmissionKey& _key) noexcept {
        const size_t index = FindNodeIndex(_key);
        return index == NoIndex ? nullptr : &nodes[index];
    }

    [[nodiscard]] const InternalNode* FindNode(const SubmissionKey& _key) const noexcept {
        const size_t index = FindNodeIndex(_key);
        return index == NoIndex ? nullptr : &nodes[index];
    }

    [[nodiscard]] bool IsReady(size_t _node_index) const noexcept {
        const InternalNode& node = nodes[_node_index];
        if (node.state != ETranslateWaveNodeState::Pending) {
            return false;
        }

        for (const size_t dependency_index : node.dependencies) {
            const InternalNode& dependency = nodes[dependency_index];
            if (dependency.async_translate) {
                if (dependency.state != ETranslateWaveNodeState::Translated &&
                    dependency.state != ETranslateWaveNodeState::Released) {
                    return false;
                }
            } else if (dependency.state != ETranslateWaveNodeState::Released) {
                return false;
            }
        }

        if (node.same_native_queue_predecessor != NoIndex &&
            nodes[node.same_native_queue_predecessor].state != ETranslateWaveNodeState::Released) {
            return false;
        }

        if (node.hard_boundary_predecessor != NoIndex &&
            nodes[node.hard_boundary_predecessor].state != ETranslateWaveNodeState::Released) {
            return false;
        }

        if (!node.async_translate) {
            for (size_t prefix_index = 0; prefix_index < _node_index; ++prefix_index) {
                if (nodes[prefix_index].state != ETranslateWaveNodeState::Released) {
                    return false;
                }
            }
        }

        return true;
    }

    std::vector<InternalNode> nodes{};
    ETranslateWaveBuildError  build_error{ETranslateWaveBuildError::None};
    size_t                    error_node_index{NoIndex};
    size_t                    error_dependency_index{NoIndex};
    bool                      built{false};
    bool                      cancelled{false};
};

} // namespace Moer::Render::VulkanTranslateWaveDetail
