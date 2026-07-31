#include "config/ConfigManager.h"
#include "window/WindowPlacementPersistence.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

using namespace Moer::Render;

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class ScopedTemporaryDirectory {
public:
    ScopedTemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#else
        const auto process_id = static_cast<unsigned long long>(getpid());
#endif
        path_ =
            std::filesystem::temp_directory_path() /
            ("moer-window-placement-contract-" + std::to_string(process_id) + "-" + std::to_string(nonce));
        Require(std::filesystem::create_directories(path_), "failed to create test directory");
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&)            = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

    ~ScopedTemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Get() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedCurrentPath {
public:
    ScopedCurrentPath() : original_(std::filesystem::current_path()) {}

    ScopedCurrentPath(const ScopedCurrentPath&)            = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    ~ScopedCurrentPath() {
        std::error_code error;
        std::filesystem::current_path(original_, error);
    }

private:
    std::filesystem::path original_;
};

class ScopedEditorSettingsEnvironment {
public:
    explicit ScopedEditorSettingsEnvironment(const std::optional<std::filesystem::path>& value) {
#if defined(_WIN32)
        const DWORD required_size = GetEnvironmentVariableW(L"MOER_EDITOR_SETTINGS_DIR", nullptr, 0);
        if (required_size > 0) {
            std::vector<wchar_t> buffer(required_size);
            if (GetEnvironmentVariableW(L"MOER_EDITOR_SETTINGS_DIR", buffer.data(), required_size) > 0) {
                original_ = std::wstring(buffer.data());
            }
        }
        Require(
            SetEnvironmentVariableW(L"MOER_EDITOR_SETTINGS_DIR", value ? value->c_str() : nullptr) != FALSE,
            "failed to update MOER_EDITOR_SETTINGS_DIR"
        );
#else
        if (const char* current = std::getenv("MOER_EDITOR_SETTINGS_DIR")) {
            original_ = std::string(current);
        }
        const int update_result = value ? setenv("MOER_EDITOR_SETTINGS_DIR", value->c_str(), 1) :
                                          unsetenv("MOER_EDITOR_SETTINGS_DIR");
        Require(update_result == 0, "failed to update MOER_EDITOR_SETTINGS_DIR");
#endif
    }

    ScopedEditorSettingsEnvironment(const ScopedEditorSettingsEnvironment&)            = delete;
    ScopedEditorSettingsEnvironment& operator=(const ScopedEditorSettingsEnvironment&) = delete;

    ~ScopedEditorSettingsEnvironment() {
#if defined(_WIN32)
        SetEnvironmentVariableW(L"MOER_EDITOR_SETTINGS_DIR", original_ ? original_->c_str() : nullptr);
#else
        if (original_) {
            setenv("MOER_EDITOR_SETTINGS_DIR", original_->c_str(), 1);
        } else {
            unsetenv("MOER_EDITOR_SETTINGS_DIR");
        }
#endif
    }

private:
#if defined(_WIN32)
    std::optional<std::wstring> original_;
#else
    std::optional<std::string> original_;
#endif
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(stream), "failed to open fixture for writing");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(stream), "failed to write fixture");
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream), "failed to open fixture for reading");
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>(),
    };
}

std::filesystem::path Normalize(const std::filesystem::path& path) {
    std::error_code       error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

void TestSaveLoadRoundTripAndOverwrite(const std::filesystem::path& root) {
    const std::filesystem::path state_path = root / "nested" / "window.toml";
    const WindowPlacement       initial{
              .x         = -1440,
              .y         = 72,
              .width     = 1280,
              .height    = 720,
              .maximized = false,
    };
    Require(SaveWindowPlacement(state_path, initial), "initial window placement save failed");

    const auto loaded_initial = LoadWindowPlacement(state_path);
    Require(loaded_initial.has_value(), "saved window placement did not load");
    Require(*loaded_initial == initial, "window placement round trip changed values");

    const WindowPlacement replacement{
        .x         = 240,
        .y         = 160,
        .width     = 1600,
        .height    = 900,
        .maximized = true,
    };
    Require(SaveWindowPlacement(state_path, replacement), "window placement overwrite failed");

    const auto loaded_replacement = LoadWindowPlacement(state_path);
    Require(loaded_replacement.has_value(), "overwritten window placement did not load");
    Require(*loaded_replacement == replacement, "window placement overwrite retained stale values");
}

void TestInvalidFilesAreRejected(const std::filesystem::path& root) {
    const std::filesystem::path malformed_path = root / "malformed.toml";
    WriteText(malformed_path, "version = [this is not valid TOML\n");
    Require(!LoadWindowPlacement(malformed_path).has_value(), "malformed window state was accepted");

    const std::filesystem::path unknown_version_path = root / "unknown-version.toml";
    WriteText(
        unknown_version_path,
        "version = 99\n"
        "x = 10\n"
        "y = 20\n"
        "width = 1280\n"
        "height = 720\n"
        "maximized = false\n"
    );
    Require(
        !LoadWindowPlacement(unknown_version_path).has_value(), "unknown window state version was accepted"
    );
}

void TestPlacementSanitization() {
    constexpr std::array monitors{
        MonitorWorkArea{.x = 0, .y = 0, .width = 1920, .height = 1080},
        MonitorWorkArea{.x = -1600, .y = 0, .width = 1600, .height = 900},
    };

    const WindowPlacement on_secondary{
        .x         = -1500,
        .y         = 80,
        .width     = 1200,
        .height    = 700,
        .maximized = true,
    };
    const auto sanitized_secondary = SanitizeWindowPlacement(on_secondary, monitors);
    Require(sanitized_secondary.has_value(), "secondary-monitor placement was rejected");
    Require(
        *sanitized_secondary == on_secondary, "valid negative secondary-monitor coordinates were changed"
    );

    const WindowPlacement offscreen{
        .x         = 6000,
        .y         = 4000,
        .width     = 800,
        .height    = 600,
        .maximized = false,
    };
    const auto sanitized_offscreen = SanitizeWindowPlacement(offscreen, monitors);
    Require(sanitized_offscreen.has_value(), "off-screen placement was rejected");
    Require(
        *sanitized_offscreen ==
            WindowPlacement{
                .x         = 560,
                .y         = 240,
                .width     = 800,
                .height    = 600,
                .maximized = false,
            },
        "off-screen placement was not centered on the nearest monitor"
    );

    const WindowPlacement oversized{
        .x         = -2000,
        .y         = -200,
        .width     = 4000,
        .height    = 2000,
        .maximized = false,
    };
    const auto sanitized_oversized = SanitizeWindowPlacement(oversized, monitors);
    Require(sanitized_oversized.has_value(), "oversized placement was rejected");
    Require(
        *sanitized_oversized ==
            WindowPlacement{
                .x         = 0,
                .y         = 0,
                .width     = 1920,
                .height    = 1080,
                .maximized = false,
            },
        "oversized placement was not clamped to its best monitor work area"
    );

    Require(
        !SanitizeWindowPlacement(offscreen, {}).has_value(), "placement without a usable monitor was accepted"
    );
}

void TestLegacyMigrationPriority(const std::filesystem::path& root) {
    const std::filesystem::path target  = root / "settings" / "imgui.ini";
    const std::filesystem::path missing = root / "legacy-missing.ini";
    const std::filesystem::path empty   = root / "legacy-empty.ini";
    const std::filesystem::path first   = root / "legacy-first.ini";
    const std::filesystem::path second  = root / "legacy-second.ini";

    WriteText(empty, "");
    WriteText(first, "[Window][First]\nPos=10,20\n");
    WriteText(second, "[Window][Second]\nPos=30,40\n");

    const std::array candidates{missing, empty, first, second};
    const auto       migrated = MigrateLegacyImGuiSettings(target, candidates);
    Require(migrated.has_value(), "no usable legacy ImGui settings were migrated");
    Require(Normalize(*migrated) == Normalize(first), "legacy ImGui migration ignored candidate priority");
    Require(ReadText(target) == ReadText(first), "migrated ImGui settings content is wrong");
    Require(ReadText(first) == "[Window][First]\nPos=10,20\n", "legacy migration modified its source file");

    WriteText(first, "[Window][Changed]\n");
    Require(
        !MigrateLegacyImGuiSettings(target, candidates).has_value(),
        "legacy migration overwrote an existing target"
    );
    Require(
        ReadText(target) == "[Window][First]\nPos=10,20\n",
        "existing ImGui settings changed during a second migration"
    );
}

void TestEditorSettingsPathIsProjectLocalAndCwdIndependent(const std::filesystem::path& root) {
    const std::filesystem::path workspace = std::filesystem::absolute(root / "workspace");
    const std::filesystem::path cwd_a     = root / "cwd-a";
    const std::filesystem::path cwd_b     = root / "cwd-b";
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(cwd_a);
    std::filesystem::create_directories(cwd_b);
    WriteText(workspace / "MoerEngine.toml", "# minimal contract fixture\n");

    ScopedCurrentPath               restore_current_path;
    ScopedEditorSettingsEnvironment restore_environment(std::nullopt);

    std::filesystem::current_path(cwd_a);
    Moer::ConfigManager& config_manager = Moer::ConfigManager::GetInstance();
    config_manager.Init(workspace);
    const std::filesystem::path resolved = config_manager.GetEditorSettingsPath();
    Require(resolved.is_absolute(), "editor settings path is not absolute");
    Require(
        Normalize(resolved) == Normalize(workspace / "saved" / "editor"),
        "editor settings path was not kept inside the project workspace"
    );
    Require(std::filesystem::is_directory(resolved), "project-local settings directory was not created");
    Require(
        Normalize(config_manager.GetScenePath()) ==
            Normalize(workspace / "asset" / "scenes" / "sponza" / "Sponza.gltf"),
        "relative scene path was not anchored to the workspace"
    );
    Require(
        Normalize(config_manager.GetConfig().engine.scene.scene_path) ==
            Normalize(config_manager.GetScenePath()),
        "runtime config retained a CWD-dependent scene path"
    );
    Require(
        Normalize(config_manager.GetConfig().editor.preset_imgui_config_path) ==
            Normalize(workspace / "asset" / "preset_imgui.ini"),
        "relative ImGui preset path was not anchored to the workspace"
    );

    std::filesystem::current_path(cwd_b);
    Require(
        config_manager.GetEditorSettingsPath() == resolved,
        "editor settings path changed after switching the current directory"
    );

    config_manager.Init(workspace);
    Require(
        config_manager.GetEditorSettingsPath() == resolved,
        "editor settings path changed when reinitialized from another directory"
    );
}

void TestEditorSettingsOverride(const std::filesystem::path& root) {
    const std::filesystem::path settings_root = std::filesystem::absolute(root / "settings");
    const std::filesystem::path workspace     = std::filesystem::absolute(root / "workspace");
    const std::filesystem::path alternate_cwd = std::filesystem::absolute(root / "alternate-cwd");
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(alternate_cwd);
    WriteText(workspace / "MoerEngine.toml", "# minimal contract fixture\n");

    Moer::ConfigManager& config_manager = Moer::ConfigManager::GetInstance();
    {
        const std::optional<std::filesystem::path> absolute_override{settings_root};
        ScopedEditorSettingsEnvironment            restore_environment(absolute_override);
        config_manager.Init(workspace);
        Require(
            Normalize(config_manager.GetEditorSettingsPath()) == Normalize(settings_root),
            "absolute editor settings override did not resolve to the requested directory"
        );
    }

    {
        ScopedCurrentPath restore_current_path;
        std::filesystem::current_path(alternate_cwd);
        const std::optional<std::filesystem::path> relative_override{"relative-settings"};
        ScopedEditorSettingsEnvironment            restore_environment(relative_override);
        config_manager.Init(workspace);
        Require(
            Normalize(config_manager.GetEditorSettingsPath()) == Normalize(workspace / "relative-settings"),
            "relative editor settings override was not anchored to the project workspace"
        );
    }
}

} // namespace

int main() {
    try {
        ScopedTemporaryDirectory     temporary_directory;
        const std::filesystem::path& root = temporary_directory.Get();

        TestSaveLoadRoundTripAndOverwrite(root / "save-load");
        TestInvalidFilesAreRejected(root / "invalid");
        TestPlacementSanitization();
        TestLegacyMigrationPriority(root / "migration");
        TestEditorSettingsPathIsProjectLocalAndCwdIndependent(root / "config");
        TestEditorSettingsOverride(root / "override");
    } catch (const std::exception& error) {
        std::cerr << "WindowPlacementPersistenceContract: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "WindowPlacementPersistenceContract: all checks passed\n";
    return EXIT_SUCCESS;
}
