#pragma once

#include "API_Macro.h"
#include "string/String.h"

#include <cstdint>
#include <span>

namespace Moer::FileDialog {

enum class EOpenFileStatus : uint8_t {
    Success = 0,
    Cancelled,
    Error,
};

struct Filter {
    Utf8StringView name;
    Utf8StringView pattern;
};

using OpenFileCallback = void (*)(Utf8StringView selected_path, void* user_data);

struct OpenFileRequest {
    std::span<const Filter> filters{};
    OpenFileCallback       callback{nullptr};
    void*                  user_data{nullptr};
};

CORE_API bool Init();
CORE_API void ShutDown();
CORE_API EOpenFileStatus OpenFile(const OpenFileRequest& request);

} // namespace Moer::FileDialog
