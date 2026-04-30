/**
 * 提供 Scene testcase 名称查询和 testcase factory
 */
#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"
#include "scene/testcase/SceneTestCase.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <string_view>

namespace Moer {

struct SceneTestCaseRequest {
    ESceneTestCaseId test_case_id                     = ESceneTestCaseId::None;
    bool             renderable_stress_create_enabled = false;

    float3 add_light_position = float3(0.f, 2.f, 0.f);
    float3 add_light_color    = float3(1.f, 0.2f, 0.05f);
};

// 根据 testcase ID 返回可读名称
std::string_view GetSceneTestCaseName(ESceneTestCaseId test_case_id);

// 根据 testcase 请求创建对应 testcase 实例
UniquePtr<ISceneTestCase> CreateSceneTestCase(const SceneTestCaseRequest& request);

} // namespace Moer
