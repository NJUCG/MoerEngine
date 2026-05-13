#pragma once

#include "scripting/ScriptingApi.h"

#include <filesystem>

namespace Moer::scripting {

// 描述嵌入式 Python 运行时需要访问的目录布局
struct SCRIPTING_API PythonRuntimeConfig {
    // 按当前工作目录布局生成默认的 Python 运行时配置
    static PythonRuntimeConfig Default();

    std::filesystem::path runtime_root;
    std::filesystem::path program_path;
    std::filesystem::path stdlib_dir;
    std::filesystem::path dll_dir;
};

} // namespace Moer::scripting
