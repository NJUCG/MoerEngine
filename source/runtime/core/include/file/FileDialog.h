#pragma once

#include "API_Macro.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace Moer::FileDialog {

enum class EOpenFileStatus : uint8_t {
    Success = 0,
    Cancelled,
    Error,
};

struct Filter {
    std::string_view name;
    std::string_view pattern;
};

struct OpenFileRequest {
    std::span<const Filter> filters{};
};

struct OpenFileResult {
    EOpenFileStatus       status = EOpenFileStatus::Cancelled;
    std::filesystem::path path;
};

CORE_API bool Init();
CORE_API void ShutDown();
CORE_API OpenFileResult OpenFile(const OpenFileRequest& request);

} // namespace Moer::FileDialog
