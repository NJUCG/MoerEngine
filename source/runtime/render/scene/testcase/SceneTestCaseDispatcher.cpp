/**
 * Implements Scene testcase request dispatching and long-running scene debug actions.
 */
#include "scene/testcase/SceneTestCaseDispatcher.h"

#include "misc/STL.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"
#include "scene/testcase/SceneTestCaseRegistry.h"
#include "scene/testcase/SceneTestCaseRunner.h"

#include <cmath>

namespace Moer {

namespace {

struct TransformMotionState {
    float3 base_translation;
    float3 direction;
    float  amplitude = 0.f;
    float  phase     = 0.f;
};

using TransformMotionStateMap = UnorderedMap<entt::entity, TransformMotionState>;

uint HashEntity(entt::entity entity, uint salt) {
    uint value = static_cast<uint>(entt::to_integral(entity)) ^ salt;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float HashToUnitFloat(uint value) {
    return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x00ffffffu);
}

float HashToRange(uint value, float min_value, float max_value) {
    return min_value + (max_value - min_value) * HashToUnitFloat(value);
}

float3 BuildMotionDirection(entt::entity entity, uint salt) {
    const float x = HashToRange(HashEntity(entity, salt + 1u), -1.f, 1.f);
    const float y = HashToRange(HashEntity(entity, salt + 2u), -1.f, 1.f);
    const float z = HashToRange(HashEntity(entity, salt + 3u), -1.f, 1.f);

    const float length = std::sqrt(x * x + y * y + z * z);
    if (length <= 1e-5f) {
        return float3(1.f, 0.f, 0.f);
    }
    return float3(x / length, y / length, z / length);
}

TransformMotionState CreateMotionState(
    entt::entity      entity,
    const ecs::CNode& node,
    uint              salt,
    float             min_amplitude,
    float             max_amplitude
) {
    return TransformMotionState{
        .base_translation = node.translation,
        .direction        = BuildMotionDirection(entity, salt),
        .amplitude        = HashToRange(HashEntity(entity, salt + 4u), min_amplitude, max_amplitude),
        .phase            = HashToRange(HashEntity(entity, salt + 5u), 0.f, 6.28318530718f),
    };
}

void RestoreTransformMotionStates(Scene& scene, TransformMotionStateMap& states) {
    auto& registry = scene.r();
    for (auto& [entity, state] : states) {
        if (!registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
            continue;
        }

        scene.Patch<ecs::CNode>(entity, [&](ecs::CNode& node) {
            node.translation = state.base_translation;
        });
    }
    states.clear();
}

template<typename ViewBuilder>
void ProcessTransformMotionGroup(
    Scene&                   scene,
    bool                     enabled,
    float                    elapsed_time_seconds,
    TransformMotionStateMap& states,
    ViewBuilder&&            build_view,
    uint                     salt,
    float                    min_amplitude,
    float                    max_amplitude
) {
    if (!enabled) {
        if (!states.empty()) {
            RestoreTransformMotionStates(scene, states);
        }
        return;
    }

    auto&           registry = scene.r();
    auto            view     = build_view(registry);
    constexpr float speed    = 1.25f;

    view.each([&](const auto entity, const auto&, const ecs::CNode& node) {
        auto [state_it, inserted] = states.try_emplace(entity);
        if (inserted) {
            state_it->second = CreateMotionState(entity, node, salt, min_amplitude, max_amplitude);
        }

        const TransformMotionState& state = state_it->second;
        const float offset = std::sin(elapsed_time_seconds * speed + state.phase) * state.amplitude;
        scene.Patch<ecs::CNode>(entity, [&](ecs::CNode& patched_node) {
            patched_node.translation = state.base_translation + state.direction * offset;
        });
    });
}

TransformMotionStateMap& RenderableMotionStates() {
    static TransformMotionStateMap s_states;
    return s_states;
}

TransformMotionStateMap& PointLightMotionStates() {
    static TransformMotionStateMap s_states;
    return s_states;
}

void DispatchRequestedSceneTestCase(SceneTestCaseConfig& config) {
    if (config.requested_test_case == ESceneTestCaseId::None) {
        return;
    }

    SceneTestCaseRequest request{};
    request.test_case_id                     = config.requested_test_case;
    request.renderable_stress_create_enabled = config.renderable_stress_create_enabled;
    request.add_light_position               = config.add_light_position;
    request.add_light_color                  = config.add_light_color;

    SceneTestCaseRunner::Get().RequestCase(CreateSceneTestCase(request));
    config.requested_test_case = ESceneTestCaseId::None;
}

void ProcessSceneMotion(SceneTestCaseConfig& config, Scene& scene, float elapsed_time_seconds) {
    ProcessTransformMotionGroup(
        scene,
        config.move_renderables_enabled,
        elapsed_time_seconds,
        RenderableMotionStates(),
        [](entt::registry& registry) {
            return registry.view<const ecs::CRenderable, const ecs::CNode>();
        },
        0x4d52524fu,
        0.08f,
        0.22f
    );

    ProcessTransformMotionGroup(
        scene,
        config.move_point_lights_enabled,
        elapsed_time_seconds,
        PointLightMotionStates(),
        [](entt::registry& registry) {
            return registry.view<const ecs::CLightPoint, const ecs::CNode>();
        },
        0x504c4954u,
        0.35f,
        1.15f
    );
}

} // namespace

void ProcessSceneTestCaseRequests(
    SceneTestCaseConfig& scene_test_case_config,
    Scene&               scene,
    float                elapsed_time_seconds
) {
    DispatchRequestedSceneTestCase(scene_test_case_config);
    ProcessSceneMotion(scene_test_case_config, scene, elapsed_time_seconds);
}

} // namespace Moer
