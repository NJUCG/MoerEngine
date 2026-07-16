#pragma once

#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptExecutionResult.h"
#include "scripting/ScriptingApi.h"

#include <pybind11/pytypes.h>

#include <memory>
#include <thread>

namespace py = pybind11;

namespace Moer::scripting {

// 封装嵌入式 Python 解释器的初始化、关闭和脚本执行能力
//
// TODO: 增量 stdout / stderr
// - 说明：脚本执行过程中实时返回输出，而不是结束后一次性汇总
// - 路径：PythonRuntime 将当前 StringIO 改为可持续写入的 writer，执行层接收输出 chunk 后再向外转发
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
    // 在默认 SharedGlobal 上下文中执行一段脚本并返回执行结果
    ScriptExecutionResult ExecuteSnippet(const ScriptExecutionRequest& request);
    // 在指定的 Python globals 上执行一段脚本并返回执行结果
    ScriptExecutionResult ExecuteSnippet(const ScriptExecutionRequest& request, const py::dict& globals);
    // 返回当前 SharedGlobal 对应的 globals 句柄
    py::dict GetSharedGlobals() const;
    // 复制当前 SharedGlobal，供新 session 作为初始上下文使用
    py::dict CopySharedGlobals() const;

private:
    // 持有解释器内部状态的私有实现类型
    struct State;

    // 校验当前调用线程是否拥有解释器访问权
    void EnsureOwnerThread() const;
    // 校验解释器已初始化且当前线程拥有访问权
    void EnsureReadyForAccess() const;
    // 在已经获取 GIL 的前提下，在给定 globals 上执行脚本
    ScriptExecutionResult
    ExecuteSnippetOnGlobals(const ScriptExecutionRequest& request, const py::dict& globals);

    PythonRuntimeConfig    m_config;
    std::thread::id        m_owner_thread_id{};
    std::unique_ptr<State> m_state;
    bool                   m_is_initialized = false;
};

} // namespace Moer::scripting
