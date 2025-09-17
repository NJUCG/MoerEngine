#pragma once

/**
 * cpp代码include此头文件，cuda代码不应该include此文件
 */

#include <optional>
#include <string>

#include "compile_tool.h"

namespace Moer { namespace Cuda {

using uint64 = unsigned long long;
using uint32 = unsigned int;

template<typename T>
class Result {

public:
    static Result Ok(const T& value) {
        Result result;
        result.m_value = value;
        return result;
    }

    static Result Err(const std::string& err) {
        Result result;
        result.m_err = err;
        return result;
    }

    bool ok() const { return m_value.has_value(); }

    // 调用前，务必执行 result.ok() 进行检查
    const T& value() const { return *m_value; }

    // 调用前，务必执行 result.ok() 进行检查
    T& value() { return *m_value; }

    const std::string& err() const { return m_err; }

private:
    Result() = default;

    std::optional<T> m_value;
    std::string      m_err;
};

}} // namespace Moer::Cuda
