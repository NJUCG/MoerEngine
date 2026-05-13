#pragma once

#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptExecutionResult.h"
#include "scripting/ScriptingApi.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace Moer::scripting {

// 管理脚本线程、任务队列和异步执行结果
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
    // 提交一段脚本并异步返回执行结果
    std::future<ScriptExecutionResult> SubmitSnippet(ScriptExecutionRequest request);

private:
    // 表示一个排队等待执行的脚本任务
    struct PendingExecution;

    // 作为脚本线程入口持续消费待执行任务
    void WorkerMain(std::promise<std::string> startup_promise);

    PythonRuntimeConfig                           m_runtime_config;
    std::thread                                   m_worker_thread;
    std::condition_variable                       m_condition;
    std::deque<std::unique_ptr<PendingExecution>> m_pending_executions;
    std::mutex                                    m_mutex;
    bool                                          m_is_running     = false;
    bool                                          m_stop_requested = false;
};

} // namespace Moer::scripting
