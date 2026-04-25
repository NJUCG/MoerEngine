#include "file/FileDialog.h"

#include "log/LogSystem.h"

#if MOER_CORE_HAS_NFD
#include "../../../../../3rdparty/nativefiledialog-extended-1.2.1/src/include/nfd.hpp"

#include <string>
#include <vector>
#endif

namespace Moer::FileDialog {
namespace {

bool g_is_initialized = false;

} // namespace

bool Init() {
#if MOER_CORE_HAS_NFD
    if (g_is_initialized) {
        return true;
    }

    if (NFD::Init() != NFD_OKAY) {
        LOG_ERROR(MOER_TEXT("FileDialog init failed: {}"), NFD::GetError());
        return false;
    }

    g_is_initialized = true;
    return true;
#else
    LOG_ERROR(MOER_TEXT("FileDialog is unavailable because nativefiledialog is not built for this platform."));
    return false;
#endif
}

void ShutDown() {
#if MOER_CORE_HAS_NFD
    if (!g_is_initialized) {
        return;
    }

    NFD::Quit();
    g_is_initialized = false;
#else
    g_is_initialized = false;
#endif
}

OpenFileResult OpenFile(const OpenFileRequest& request) {
    OpenFileResult result{};
#if MOER_CORE_HAS_NFD
    if (!g_is_initialized) {
        result.status = EOpenFileStatus::Error;
        LOG_ERROR(MOER_TEXT("FileDialog::OpenFile() called before FileDialog::Init()."));
        return result;
    }

    std::vector<std::string> native_filter_names;
    std::vector<std::string> native_filter_patterns;
    std::vector<nfdfilteritem_t> native_filters;
    native_filter_names.reserve(request.filters.size());
    native_filter_patterns.reserve(request.filters.size());
    native_filters.reserve(request.filters.size());

    for (const Filter& filter : request.filters) {
        native_filter_names.emplace_back(filter.name);
        native_filter_patterns.emplace_back(filter.pattern);
        native_filters.push_back({
            native_filter_names.back().c_str(),
            native_filter_patterns.back().c_str(),
        });
    }

    NFD::UniquePath selected_path = nullptr;
    const nfdresult_t native_result = NFD::OpenDialog(
        selected_path,
        native_filters.empty() ? nullptr : native_filters.data(),
        static_cast<nfdfiltersize_t>(native_filters.size())
    );

    if (native_result == NFD_OKAY && selected_path) {
        result.status = EOpenFileStatus::Success;
        result.path   = std::filesystem::u8path(selected_path.get());
        return result;
    }

    if (native_result == NFD_CANCEL) {
        result.status = EOpenFileStatus::Cancelled;
        return result;
    }

    result.status = EOpenFileStatus::Error;
    LOG_ERROR(MOER_TEXT("FileDialog open failed: {}"), NFD::GetError());
    return result;
#else
    (void)request;
    result.status = EOpenFileStatus::Error;
    LOG_ERROR(MOER_TEXT("FileDialog::OpenFile() is unavailable because nativefiledialog is not built for this platform."));
    return result;
#endif
}

} // namespace Moer::FileDialog
