/**
 * 提供 Scene testcase 名称查询和 testcase factory
 */
#pragma once

#include "misc/STL.h"
#include "scene/testcase/SceneTestCase.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <string_view>

namespace Moer {

// 根据 testcase ID 返回可读名称
std::string_view GetSceneTestCaseName(ESceneTestCaseId test_case_id);

// 根据 testcase ID 创建对应 testcase 实例
UniquePtr<ISceneTestCase> CreateSceneTestCase(ESceneTestCaseId test_case_id);

} // namespace Moer
