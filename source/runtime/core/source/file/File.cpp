#include "file/File.h"

#include "misc/STL.h"

#include <filesystem>
#include <fstream>

namespace Moer::File {
EReadFileStatus ReadBinaryFile(const ReadBinaryRequest& request) {
    if (request.path.empty()) {
        return EReadFileStatus::NotFound;
    }

    std::ifstream file(std::filesystem::path(request.path.Native()), std::ios::binary | std::ios::ate);
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