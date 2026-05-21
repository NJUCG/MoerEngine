#pragma once

#include "scripting/MainThreadCommandQueue.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptExecutionFuture.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptingApi.h"
#include "scripting/SessionRegistry.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace Moer {
class Scene;
}

namespace Moer::scripting {

// 管理脚本线程、任务队列和异步执行结果
//
// TODO: 脚本执行取消
// - 说明：允许外部停止长时间运行的脚本请求
// - 路径：补齐 ScriptExecutionFuture、ScriptHost、remote service 的 cancel 链路，并增加对应 HTTP 接口
class SCRIPTING_API ScriptHost {
public:
    // 使用给定运行时配置创建脚本宿主对象
    explicit ScriptHost(PythonRuntimeConfig runtime_config);
    // 在对象销毁前停止脚本线程并释放相关资源
    ~ScriptHost();

    // 禁止拷贝，避免脚本线程和队列状态被多个对象共享
    ScriptHost(const ScriptHost&) = delete;
    // 禁止赋值，避免脚本宿主所有权被错误复制
    ScriptHost& operator=(const ScriptHost&) = delete;

    // 启动脚本线程并初始化嵌入式 Python 运行时
    void Start();
    // 请求脚本线程停止并等待所有运行中任务收尾
    void Stop();
    // 提交一条脚本执行请求并异步返回执行结果
    ScriptExecutionFuture Submit(ScriptExecutionRequest request);
    // 在主线程稳定帧阶段处理 pending scene 命令
    void ProcessMainThreadCommands(Scene& scene);
    // 在 scene 不可用时取消所有 pending scene 命令
    void CancelPendingSceneCommands(std::string_view reason);

    // 表示一个排队等待执行的脚本任务
    struct PendingExecution;

private:
    // 作为脚本线程入口持续消费待执行任务
    void WorkerMain(std::promise<std::string> startup_promise);

    PythonRuntimeConfig                           m_runtime_config;
    MainThreadCommandQueue                        m_main_thread_command_queue;
    SessionRegistry                               m_session_registry;
    std::thread                                   m_worker_thread;
    std::condition_variable                       m_condition;
    std::deque<std::unique_ptr<PendingExecution>> m_pending_executions;
    std::mutex                                    m_mutex;
    bool                                          m_is_running     = false;
    bool                                          m_stop_requested = false;
};

} // namespace Moer::scripting
