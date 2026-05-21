/**
 * 定义 Scene testcase 的执行上下文和基类接口
 */
#pragma once

#include "log/LogSystem.h"
#include "scene/Scene.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Moer {

struct SceneTestCaseContext {
    std::uint64_t frame_index          = 0;
    float         elapsed_time_seconds = 0.f;
};

struct SceneTestCaseRunResult {
    ESceneTestCaseId test_case_id = ESceneTestCaseId::None;
    std::string      case_name;
    std::uint64_t    begin_frame = 0;
    std::uint64_t    end_frame   = 0;
    bool             passed      = false;
    std::string      failure_summary;
};

/**
 * ISceneTestCase 是逐帧场景测试的统一接口。
 *
 * 结构:
 * - Reset(): 初始化内部状态
 * - PreTick(): 在 Scene 采样 dirty tag 前写入改动
 * - PostTick(): 在 Scene sync 后检查结果
 * - IsFinished(): 告诉 runner 何时结束
 *
 * 改这里:
 * - 改测试生命周期: SceneTestCase.h + SceneTestCaseRunner.cpp
 * - 加新的 testcase: 新建 SceneTestCase*.cpp + SceneTestCaseRegistry.cpp
 * - 改日志/名字/UI 展示: Name() + Registry/Dispatcher
 *
 * 用法:
 * - 派生类只写测试逻辑，不直接管调度
 * - 由 SceneTestCaseRunner 驱动每帧调用
 */
class ISceneTestCase {
public:
    // 析构 testcase 基类，保证派生类能正确释放
    virtual ~ISceneTestCase() = default;

    // 返回 testcase 的日志和 UI 名称
    virtual std::string_view Name() const = 0;

    // 重置 testcase 的内部状态
    virtual void Reset(Scene& scene) = 0;

    // 在 Scene 采样 dirty tag 之前执行测试写入
    virtual void PreTick(Scene& scene, const SceneTestCaseContext& context) = 0;

    // 在 Scene sync 之后检查 TickState 和派生数据
    virtual void PostTick(Scene& scene, const Scene::TickState& tick_state) = 0;

    // 返回 testcase 是否已经完成
    virtual bool IsFinished() const = 0;

    // 返回 testcase 当前是否已经失败
    virtual bool HasFailed() const {
        return false;
    }

    // 返回 testcase 的失败摘要；成功或未失败时可为空
    virtual std::string_view FailureSummary() const {
        return {};
    }
};

class SceneTestCaseBase : public ISceneTestCase {
public:
    bool IsFinished() const override {
        return m_finished;
    }

    bool HasFailed() const override {
        return m_failed;
    }

    std::string_view FailureSummary() const override {
        return m_failure_summary;
    }

protected:
    bool Expect(bool condition, std::string_view message) {
        if (condition) {
            return true;
        }

        m_failed = true;
        if (m_failure_summary.empty()) {
            m_failure_summary = message;
        }
        LOG_ERROR("SceneTestCase '{}' failed: {}", Name(), message);
        return false;
    }

    void Finish() {
        if (!m_failed) {
            LOG_INFO("SceneTestCase '{}' passed.", Name());
        }
        m_finished = true;
    }

    void ResetBaseState() {
        m_finished = false;
        m_failed   = false;
        m_failure_summary.clear();
    }

protected:
    bool        m_finished = false;
    bool        m_failed   = false;
    std::string m_failure_summary;
};

} // namespace Moer
