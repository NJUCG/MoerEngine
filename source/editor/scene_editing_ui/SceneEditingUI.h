#pragma once

#include "scene/testcase/SceneTestCaseConfig.h"

namespace Moer {

class SceneEditingUI {
public:
    explicit SceneEditingUI(SceneTestCaseConfig& test_case_config);

    void ShowWindow(bool* p_open);

private:
    SceneTestCaseConfig& m_test_case_config;
};

} // namespace Moer