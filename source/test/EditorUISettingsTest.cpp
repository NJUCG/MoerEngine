#include "EditorUISettings.h"

#include <imgui.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace Moer;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "EditorUISettingsContract failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    constexpr std::string_view ini = R"ini(
[EditorWindowVisibility][Main]
SceneColor=1
SceneView=1
Hierarchy=1
Inspector=1
Configs=1
SceneEditing=1
Console=1
ProfileCapture=0
ProfileViewer=0
MemoryProfiler=0
)ini";
    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());

    const EditorWindowVisibilitySettings& loaded = EditorUISettings::LoadWindowVisibilitySettings();
    Expect(
        loaded.loaded && loaded.console, "Console visibility was not restored from the custom ini section"
    );

    EditorWindowVisibilitySettings stored = loaded;
    stored.console                        = false;
    EditorUISettings::StoreWindowVisibilitySettings(stored);

    std::size_t            output_size = 0;
    const char*            output      = ImGui::SaveIniSettingsToMemory(&output_size);
    const std::string_view saved(output, output_size);
    Expect(
        saved.find("[EditorWindowVisibility][Main]") != std::string_view::npos &&
            saved.find("Console=0") != std::string_view::npos,
        "Console visibility was not written to the custom ini section"
    );

    ImGui::DestroyContext();
    std::cout << "EditorUISettingsContract passed\n";
    return 0;
}
