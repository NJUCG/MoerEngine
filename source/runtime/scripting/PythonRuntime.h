#pragma once

#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptExecutionResult.h"
#include "scripting/ScriptingApi.h"

#include <memory>
#include <thread>

namespace Moer::scripting {

// 封装嵌入式 Python 解释器的初始化、关闭和脚本执行能力
class SCRIPTING_API PythonRuntime {
public:
    // 构造一个尚未初始化的 Python 运行时对象
    PythonRuntime();
    // 在对象销毁时释放底层解释器状态
    ~PythonRuntime();

    // 禁止拷贝，避免解释器状态被多个对象共享
    PythonRuntime(const PythonRuntime&) = delete;
    // 禁止赋值，避免解释器所有权被错误复制
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    // 按给定路径配置初始化嵌入式 Python 解释器
    void Initialize(const PythonRuntimeConfig& config);
    // 关闭嵌入式 Python 解释器并清理运行时状态
    void Finalize();
    // 返回当前解释器是否已经完成初始化
    bool IsInitialized() const;
    // 在当前运行时中执行一段脚本并返回执行结果
    ScriptExecutionResult ExecuteSnippet(const ScriptExecutionRequest& request);

private:
    // 持有解释器内部状态的私有实现类型
    struct State;

    // 校验当前调用线程是否拥有解释器访问权
    void EnsureOwnerThread() const;

    PythonRuntimeConfig    m_config;
    std::thread::id        m_owner_thread_id{};
    std::unique_ptr<State> m_state;
    bool                   m_is_initialized = false;
};

} // namespace Moer::scripting
