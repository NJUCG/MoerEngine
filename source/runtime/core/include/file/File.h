#pragma once

#include "API_Macro.h"
#include "string/String.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Moer::File {

enum class EReadFileStatus : uint8_t {
    Success = 0,
    NotFound,
    Error,
};

using ReadBinaryCallback = void (*)(std::span<const std::byte> data, void* user_data);

struct ReadBinaryRequest {
    Utf8StringView     path{};
    ReadBinaryCallback callback{nullptr};
    void*              user_data{nullptr};
};

CORE_API EReadFileStatus ReadBinaryFile(const ReadBinaryRequest& request);

} // namespace Moer::File