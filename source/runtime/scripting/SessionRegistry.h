#pragma once

#include "scripting/ScriptingApi.h"

#include <pybind11/pytypes.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace py = pybind11;

namespace Moer::scripting {

// Session 在这里的含义，不是切换整个 Python 解释器或整个进程内存，而是给脚本执行切换
// 一份独立的 Python globals dict。
//
// 1. Python 的 globals 是什么，为什么它可以表示“上下文”
//    Python 允许通过 exec(code, globals, locals) 指定脚本执行时使用的全局命名空间字典。
//    顶层变量、函数定义、import 进当前脚本名字空间的结果，都会写入这份 globals dict。
//    因此，如果后续执行继续复用同一份 globals，脚本就能看到上一次留下的名字；如果换成
//    另一份 globals，脚本看到的顶层名字空间就会切换。这正是这里 session 语义所需的
//    “脚本上下文”。
//
// 2. 切换 globals 时，哪些东西被切换了，哪些东西没被切换
//    例子：
//      g1 = {}
//      exec("x = 1\nimport math", g1, g1)
//      g2 = {}
//      exec("print('x' in globals())", g2, g2)  # False
//      exec("print(x)", g1, g1)                 # 1
//
//    切换的是：脚本可见的顶层名字空间，例如 x、顶层 def、以及 import 到当前 session
//    名字空间的符号。
//    没切换的是：同一个 Python 解释器实例、builtins、sys.modules 的模块缓存、以及宿主进程
//    和扩展模块里的全局状态。

// 表示一个可复用的 Python 执行上下文
struct SCRIPTING_API ScriptSession {
    std::string session_id;

    py::dict&       RequireGlobals();
    const py::dict& RequireGlobals() const;

    // globals 是 Python 执行模型中的概念，这里可以简单理解为“上下文”里的全局命名空间。
    std::optional<py::dict> globals;
};

// 管理 SharedGlobal / NamedSession / Stateless 所需的 Python 上下文
class SCRIPTING_API SessionRegistry {
public:
    SessionRegistry() = default;

    SessionRegistry(const SessionRegistry&)            = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    void Reset(const py::dict& shared_globals, const py::dict& session_seed_globals);
    void Clear();

    ScriptSession& GetSharedGlobalSession();
    ScriptSession& GetOrCreateNamedSession(std::string_view session_id);
    ScriptSession  CreateStatelessSession() const;

private:
    py::dict CloneSessionSeedGlobals() const;

    std::optional<py::dict>                        m_session_seed_globals;
    ScriptSession                                  m_shared_global_session;
    std::unordered_map<std::string, ScriptSession> m_named_sessions;
};

} // namespace Moer::scripting
