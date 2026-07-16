#pragma once

#include "scripting/ScriptExecutionResult.h"
#include "scripting/ScriptingApi.h"

#include <chrono>
#include <future>

namespace Moer::scripting {

// 提供 future-like 的脚本执行结果访问接口，并为后续取消等能力预留扩展点
//
// TODO: 脚本执行取消
// - 说明：允许外部停止长时间运行的脚本请求
// - 路径：补齐 ScriptExecutionFuture、ScriptHost、remote service 的 cancel 链路，并增加对应 HTTP 接口
class SCRIPTING_API ScriptExecutionFuture {
public:
    ScriptExecutionFuture() = default;
    explicit ScriptExecutionFuture(std::future<ScriptExecutionResult> completion_future) :
        m_completion_future(std::move(completion_future)) {}

    ScriptExecutionFuture(const ScriptExecutionFuture&)                = delete;
    ScriptExecutionFuture& operator=(const ScriptExecutionFuture&)     = delete;
    ScriptExecutionFuture(ScriptExecutionFuture&&) noexcept            = default;
    ScriptExecutionFuture& operator=(ScriptExecutionFuture&&) noexcept = default;

    bool valid() const noexcept {
        return m_completion_future.valid();
    }

    ScriptExecutionResult get() {
        return m_completion_future.get();
    }

    void wait() const {
        m_completion_future.wait();
    }

    template<typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const {
        return m_completion_future.wait_for(timeout_duration);
    }

    void cancel() {
        // TODO: 接入由 ScriptHost 驱动的取消链路。
    }

private:
    std::future<ScriptExecutionResult> m_completion_future;
};

} // namespace Moer::scripting
