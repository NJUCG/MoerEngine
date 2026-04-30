#pragma once

#include "scene/testcase/SceneTestCaseConfig.h"

namespace Moer {

class SceneTestCaseUI {
public:
    explicit SceneTestCaseUI(SceneTestCaseConfig& config);

    void ShowWindow(bool* p_open);

private:
    SceneTestCaseConfig& m_config;
};

} // namespace Moer
