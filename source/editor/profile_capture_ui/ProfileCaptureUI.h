#pragma once

#include "profile_capture_ui/ProfileCaptureControlModel.h"

#include <filesystem>
#include <string>

namespace Moer {

class Engine;

class ProfileCaptureUI final {
public:
    explicit ProfileCaptureUI(Engine& engine);

    void ShowWindow(bool* open);

private:
    ProfileCaptureControlModel control_;
    std::filesystem::path      output_path_{};
    bool                       replace_existing_{false};
    std::string                dialog_status_{};
};

} // namespace Moer
