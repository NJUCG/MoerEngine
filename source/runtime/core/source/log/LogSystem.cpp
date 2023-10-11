#include "log/LogSystem.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"
#include <stdarg.h>
#include <stdio.h>
#include <vadefs.h>
#include "spdlog/spdlog.h"

namespace Moer {
namespace LogSystem {
    void LogInfo(...) {
        SPDLOG_DEBUG("tes");
    }
}
}// namespace Moer::LogSystem