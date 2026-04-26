#include "file/File.h"

#include "misc/STL.h"
#include "string/StringConvert.h"

#include <filesystem>
#include <fstream>

namespace Moer::File {
namespace {

std::filesystem::path ToLocalPath(Utf8StringView path) {
#if defined(_WIN32) || defined(_WIN64)
    const WideString wide_path = Utf8ToWide(path);
    return std::filesystem::path(std::wstring_view(wide_path.data(), wide_path.size()));
#else
    return std::filesystem::path(std::string_view(path.data(), path.size()));
#endif
}

} // namespace

EReadFileStatus ReadBinaryFile(const ReadBinaryRequest& request) {
    if (request.path.empty()) {
        return EReadFileStatus::NotFound;
    }

    std::ifstream file(ToLocalPath(request.path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return EReadFileStatus::NotFound;
    }

    const std::streamoff file_size = file.tellg();
    if (file_size < 0) {
        return EReadFileStatus::Error;
    }

    Array<std::byte> content{};
    content.resize(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (!content.empty()) {
        file.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
        if (!file) {
            return EReadFileStatus::Error;
        }
    }

    if (request.callback) {
        request.callback(std::span<const std::byte>(content.data(), content.size()), request.user_data);
    }
    return EReadFileStatus::Success;
}

} // namespace Moer::File