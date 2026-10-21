#include "inspector_ui/InspectorUI.h"
#include "scene/Scene.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace Moer;
int main(int argc, char** argv) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("moer-gaussian-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        const auto source = directory / "asset.ply";
        {
            std::ofstream file(source);
            file << "ply\nformat ascii 1.0\nelement vertex 1\n";
            for (const char* name :
                 {"x",
                  "y",
                  "z",
                  "scale_0",
                  "scale_1",
                  "scale_2",
                  "rot_0",
                  "rot_1",
                  "rot_2",
                  "rot_3",
                  "opacity",
                  "f_dc_0",
                  "f_dc_1",
                  "f_dc_2"})
                file << "property float " << name << "\n";
            file << "end_header\n0 0 -3 -2 -2 -2 1 0 0 0 0 .5 .2 .1\n";
        }
        Scene scene;
        scene.LoadSceneFromFile(source);
        scene.PrepareUpdateBatch(false, false);
        const auto snapshot = scene.GetGaussianSnapshot();
        if (!snapshot || snapshot->instances.empty())
            throw std::runtime_error("UI fixture failed to load");
        auto selected_entity = static_cast<entt::entity>(snapshot->instances[0].key);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        auto& io                   = ImGui::GetIO();
        io.IniFilename             = nullptr;
        io.LogFilename             = nullptr;
        io.DisplaySize             = ImVec2(560, 1000);
        io.DeltaTime               = 1.f / 60;
        unsigned char* atlas       = nullptr;
        int            atlas_width = 0, atlas_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
        io.Fonts->SetTexID((ImTextureID)1);
        InspectorUI inspector;
        bool        is_open = true;
        for (uint frame_id = 0; frame_id < 2; ++frame_id) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            inspector.ShowWindow(&is_open, &scene, selected_entity);
            ImGui::Render();
        }
        const auto* data = ImGui::GetDrawData();
        if (!data || data->TotalVtxCount == 0 || selected_entity == entt::null)
            throw std::runtime_error("Gaussian Inspector did not draw");
        if (argc > 1) {
            nlohmann::json document = {
                {"width", 560},
                {"height", 1000},
                {"atlas_width", atlas_width},
                {"atlas_height", atlas_height},
                {"lists", nlohmann::json::array()}
            };
            for (const auto* list : data->CmdLists) {
                nlohmann::json draw = {
                    {"vertices", nlohmann::json::array()},
                    {"indices", nlohmann::json::array()},
                    {"commands", nlohmann::json::array()}
                };
                for (const auto& vertex : list->VtxBuffer)
                    draw["vertices"].push_back(
                        {vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y, vertex.col}
                    );
                for (const auto index : list->IdxBuffer)
                    draw["indices"].push_back(index);
                for (const auto& command : list->CmdBuffer)
                    if (!command.UserCallback)
                        draw["commands"].push_back(
                            {{"clip",
                              {command.ClipRect.x,
                               command.ClipRect.y,
                               command.ClipRect.z,
                               command.ClipRect.w}},
                             {"count", command.ElemCount},
                             {"index", command.IdxOffset},
                             {"vertex", command.VtxOffset}}
                        );
                document["lists"].push_back(std::move(draw));
            }
            std::ofstream(argv[1]) << document;
            std::ofstream atlas_file(std::string(argv[1]) + ".atlas", std::ios::binary);
            atlas_file.write(reinterpret_cast<char*>(atlas), size_t(atlas_width) * atlas_height * 4);
        }
        scene.DestroyGaussianInstance(selected_entity);
        ImGui::NewFrame();
        inspector.ShowWindow(&is_open, &scene, selected_entity);
        ImGui::Render();
        if (selected_entity != entt::null)
            throw std::runtime_error("Deleted Inspector selection was retained");
        ImGui::DestroyContext();
        std::filesystem::remove_all(directory); // Unique, test-owned temporary directory.
        std::cout << "Gaussian Inspector headless contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
