#include "scene/Scene.h"
#include "scene/gaussian/GaussianGpuScene.h"
#include "scene/loader/GaussianPlyLoader.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Moer;

namespace {
int  failure_count = 0;
void Check(bool is_condition_met, const char* message) {
    if (!is_condition_met) {
        ++failure_count;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool Near(float a, float b) {
    return std::abs(a - b) < 1e-5f;
}

std::string Header(bool is_binary, int rest_coefficient_count = 0, int vertex_count = 1) {
    std::string header = "ply\nformat " + std::string(is_binary ? "binary_little_endian" : "ascii") +
                         " 1.0\ncomment Gaussian fixture\nelement vertex " + std::to_string(vertex_count) +
                         "\n";
    // Deliberately shuffled fields and an unknown scalar attribute.
    for (const char* field :
         {"opacity",
          "z",
          "x",
          "y",
          "f_dc_2",
          "rot_3",
          "scale_1",
          "rot_0",
          "f_dc_0",
          "scale_0",
          "rot_1",
          "scale_2",
          "rot_2",
          "f_dc_1",
          "unused"}) {
        header += "property float " + std::string(field) + "\n";
    }
    for (int coefficient_id = 0; coefficient_id < rest_coefficient_count; ++coefficient_id)
        header += "property float f_rest_" + std::to_string(coefficient_id) + "\n";
    return header + "end_header\n";
}
void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}
} // namespace

int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("moer-gaussian-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        const auto        ascii_path = directory / "valid.ply";
        const std::string values     = "0 3 1 2 0.3 0 0 2 0.1 0 0 0 0 0.2 123";
        Write(ascii_path, Header(false, 9) + values + " 1 2 3 4 5 6 7 8 9\n");
        auto loaded = LoadGaussianPly(ascii_path);
        Check(bool(loaded), "reordered ASCII degree-one PLY loads");
        if (!loaded)
            throw std::runtime_error(loaded.error_message);
        const auto& gaussian = loaded.asset->gaussians.front();
        Check(
            loaded.asset->sh_degree == 1 && loaded.asset->gaussians.size() == 1, "SH degree and vertex count"
        );
        Check(
            Near(gaussian.position.x, 1) && Near(gaussian.position.y, 2) && Near(gaussian.position.z, 3),
            "position fields by name"
        );
        Check(
            Near(gaussian.scale_opacity.x, 1) && Near(gaussian.scale_opacity.w, 0.5f) &&
                Near(gaussian.rotation.w, 1),
            "exp / sigmoid / quaternion normalization"
        );
        Check(
            Near(gaussian.sh[1].x, 1) && Near(gaussian.sh[1].y, 4) && Near(gaussian.sh[1].z, 7) &&
                Near(gaussian.sh[3].z, 9),
            "channel-major source SH converted to coefficient-major RGB"
        );
        Check(
            loaded.asset->bounds.IsValid() && Near(loaded.asset->bounds.min.x, -2),
            "conservative support bounds"
        );

        const auto binary_path = directory / "binary.ply";
        {
            std::ofstream output(binary_path, std::ios::binary);
            output << Header(true);
            const float raw[] = {0, 3, 1, 2, 0.3f, 0, 0, 2, 0.1f, 0, 0, 0, 0, 0.2f, 123};
            output.write(reinterpret_cast<const char*>(raw), sizeof(raw));
        }
        auto binary_result = LoadGaussianPly(binary_path);
        Check(
            bool(binary_result) && Near(binary_result.asset->gaussians[0].position.z, 3),
            "binary little-endian loads"
        );
        for (int degree = 0; degree <= 3; ++degree) {
            const int   rest_coefficient_count = 3 * ((degree + 1) * (degree + 1) - 1);
            std::string row                    = values;
            for (int coefficient_id = 0; coefficient_id < rest_coefficient_count; ++coefficient_id)
                row += " 0";
            Write(directory / "degree.ply", Header(false, rest_coefficient_count) + row + "\n");
            auto result = LoadGaussianPly(directory / "degree.ply");
            Check(bool(result) && result.asset->sh_degree == degree, "all supported SH degrees load");
        }
        Write(directory / "bad.ply", Header(false, 1) + values + " 0\n");
        Check(!LoadGaussianPly(directory / "bad.ply"), "invalid SH degree rejected");
        Write(directory / "bad.ply", Header(false) + "0 3 1");
        Check(!LoadGaussianPly(directory / "bad.ply"), "truncated ASCII rejected");
        Write(directory / "bad.ply", Header(true));
        Check(!LoadGaussianPly(directory / "bad.ply"), "truncated binary rejected before allocation");
        Write(
            directory / "bad.ply",
            "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n0\n"
        );
        Check(!LoadGaussianPly(directory / "bad.ply"), "ordinary point cloud is not a Gaussian asset");
        Write(directory / "bad.ply", Header(false) + "0 3 1 2 0.3 0 0 0 0.1 0 0 0 0 0.2 0\n");
        Check(!LoadGaussianPly(directory / "bad.ply"), "zero quaternion rejected");
        Write(directory / "bad.ply", Header(false) + "0 3 1 2 0.3 0 1000 2 0.1 0 0 0 0 0.2 0\n");
        Check(!LoadGaussianPly(directory / "bad.ply"), "scale overflow rejected");
        Write(directory / "empty.ply", Header(false, 0, 0));
        auto empty = LoadGaussianPly(directory / "empty.ply");
        Check(bool(empty) && empty.asset->gaussians.empty(), "empty Gaussian asset is valid and inert");

        Scene scene;
        scene.LoadSceneFromFile(ascii_path);
        Check(
            scene.IsReady() && scene.GetGaussianSceneContent() == EGaussianSceneContent::GaussianOnly,
            "pure PLY creates a ready scene without Mesh"
        );
        auto first = scene.PrepareUpdateBatch(false, false);
        Check(
            first.initial_gpu_update.has_value() && first.update_gpu_update &&
                first.update_gpu_update->gaussian_update,
            "first update includes independent Gaussian snapshot"
        );
        auto snapshot = first.update_gpu_update->gaussian_update;
        Check(snapshot->instances.size() == 1, "one imported instance");
        const auto entity = static_cast<entt::entity>(snapshot->instances[0].key);
        auto       stable = scene.PrepareUpdateBatch(false, false);
        Check(
            !stable.update_gpu_update || !stable.update_gpu_update->HasWork(), "unchanged scene emits no work"
        );
        Check(scene.SetNodeTranslation(entity, float3(4, 5, 6)), "Gaussian node uses public transform API");
        auto moved = scene.PrepareUpdateBatch(false, false);
        Check(
            moved.update_gpu_update && moved.update_gpu_update->gaussian_update &&
                !moved.update_gpu_update->HasMeshWork(),
            "Gaussian leaf transform does not update Mesh/TLAS"
        );
        Check(
            Near(snapshot->instances[0].world_transform.GetColumn(3).x, 0),
            "previous immutable snapshot survives edits"
        );
        Check(
            Near(moved.update_gpu_update->gaussian_update->instances[0].world_transform.GetColumn(3).x, 4),
            "new snapshot captures world translation"
        );

        const auto               duplicate_entt =
            scene.DuplicateGaussianInstance(entity);
        Check(duplicate_entt != entt::null && scene.GetGaussianAsset(duplicate_entt) == scene.GetGaussianAsset(entity), "duplicate shares immutable asset");
        Check(Near(scene.TryGetNodeLocalTransform(duplicate_entt)->translation.x, 4), "duplicate preserves local transform");
        auto       duplicated = scene.PrepareUpdateBatch(false, false);
        const auto packed = Render::BuildGaussianGpuUpload(*duplicated.update_gpu_update->gaussian_update);
        Check(
            packed.gaussians.size() == 1 && packed.instances.size() == 2 && packed.assets.size() == 1,
            "shared asset geometry uploaded once for two instances"
        );
        auto settings               = *scene.GetGaussianInstanceSettings(entity);
        settings.opacity_multiplier = std::numeric_limits<float>::infinity();
        Check(!scene.SetGaussianInstanceSettings(entity, settings), "invalid instance edit rejected");
        settings.opacity_multiplier = 0.25f;
        Check(scene.SetGaussianInstanceSettings(entity, settings), "valid appearance edit");
        Check(scene.SetNodeVisibleInGame(entity, false), "Gaussian visibility edit");
        auto hidden = scene.PrepareUpdateBatch(false, false);
        Check(
            hidden.update_gpu_update && !hidden.update_gpu_update->HasMeshWork(),
            "Gaussian visibility does not rebuild Mesh"
        );
        Check(
            snapshot->instances[0].is_visible_in_game &&
                Near(snapshot->instances[0].settings.opacity_multiplier, 1),
            "old snapshot remains immutable after settings change"
        );
        auto failed = scene.ImportGaussianPly(directory / "bad.ply");
        Check(!failed && scene.GetGaussianAsset(entity) != nullptr, "failed import leaves scene intact");
        const auto retained_asset = scene.GetGaussianAsset(entity);
        scene.LoadSceneFromFile(directory / "bad.ply");
        Check(
            scene.IsReady() && scene.GetGaussianAsset(entity) == retained_asset,
            "failed full PLY load also preserves the previous scene"
        );
        Check(!scene.SaveStateCache(), "Mesh-only cache cannot silently drop Gaussian data");
        std::string document_error;
        GaussianViewSettings view_settings;
        view_settings.background = float3(.15f,.2f,.25f);
        view_settings.display_mode = EGaussianDisplayMode::Engine;
        view_settings.exposure_ev = 1.25f;
        Check(scene.SetGaussianViewSettings(view_settings), "Gaussian display settings accepted");
        auto invalid_view_settings = view_settings;
        invalid_view_settings.tile_pair_capacity = 0;
        Check(!scene.SetGaussianViewSettings(invalid_view_settings), "zero pair capacity rejected");
        invalid_view_settings = view_settings; invalid_view_settings.background.x = std::numeric_limits<float>::infinity();
        Check(!scene.SetGaussianViewSettings(invalid_view_settings), "nonfinite background rejected");
        const auto document_path = directory / "scene.moergs";
        Check(scene.SaveGaussianDocument(document_path, document_error), document_error.c_str());
        Check(scene.SaveGaussianDocument(document_path, document_error), "atomic replacement of existing document");
        Scene restored;
        restored.LoadSceneFromFile(document_path);
        restored.PrepareUpdateBatch(false, false);
        const auto restored_snapshot = restored.GetGaussianSnapshot();
        Check(restored.IsReady() && restored_snapshot && restored_snapshot->instances.size() == 2, "document restores instances and hierarchy");
        Check(Near(restored.GetGaussianViewSettings().background.x,.15f) && Near(restored.GetGaussianViewSettings().exposure_ev,1.25f), "document restores Gaussian display settings");
        if (restored_snapshot && restored_snapshot->instances.size() == 2) {
            const auto& instances = restored_snapshot->instances;
            Check(instances[0].asset == instances[1].asset, "document restores shared asset geometry");
            Check(Near(instances[0].world_transform.GetColumn(3).x, 4), "document restores transform");
            Check(!instances[0].is_visible_in_game && Near(instances[0].settings.opacity_multiplier, .25f), "document restores hidden instance settings");
        }
        Check(!scene.SaveGaussianDocument(directory / "do-not-overwrite.ply", document_error), "document cannot overwrite a source PLY");
        std::ifstream document_input(document_path);
        nlohmann::json document; document_input >> document;
        Check(document.at("assets").size() == 1 && std::filesystem::path(document["assets"][0].get<std::string>()).is_relative(), "portable relative source references");
        const auto corrupt_path = directory / "broken.moergs";
        const auto test_invalid_document = [&](nlohmann::json value, const char* message) {
            { std::ofstream output(corrupt_path); output << value; }
            const auto asset_before = restored_snapshot->instances[0].asset;
            restored.LoadSceneFromFile(corrupt_path);
            Check(restored.IsReady() && restored.GetGaussianSnapshot()->instances[0].asset == asset_before, message);
        };
        auto invalid_document = document;
        invalid_document["nodes"][1]["parent"] = 1;
        test_invalid_document(invalid_document, "cyclic document preserves active scene");
        invalid_document = document; invalid_document["assets"][0] = "missing.ply";
        test_invalid_document(invalid_document, "missing asset preserves active scene");
        invalid_document = document; invalid_document["version"] = 999;
        test_invalid_document(invalid_document, "unsupported document version preserves active scene");
        invalid_document = document; invalid_document["nodes"][1]["gaussian"]["opacity"] = -1;
        test_invalid_document(invalid_document, "invalid multiplier preserves active scene");
        Check(
            scene.DestroyEntity(duplicate_entt) && scene.DestroyGaussianInstance(entity),
            "generic and dedicated deletion supported"
        );
        auto removed = scene.PrepareUpdateBatch(false, false);
        Check(
            removed.update_gpu_update && !removed.update_gpu_update->HasMeshWork() &&
                removed.update_gpu_update->gaussian_update->instances.empty(),
            "last removal emits an empty snapshot without Mesh rebuild"
        );
        Check(
            scene.GetGaussianSceneContent() == EGaussianSceneContent::Empty,
            "no Mesh or Gaussian after deletion"
        );
        Check(
            snapshot->instances[0].asset->gaussians.size() == 1,
            "asset lifetime extends through old frame snapshots"
        );
        auto after = scene.PrepareUpdateBatch(false, false);
        Check(
            !after.update_gpu_update || !after.update_gpu_update->HasWork(),
            "empty scene has no repeating Gaussian update"
        );
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    std::filesystem::remove_all(directory); // Test-owned unique directory only.
    std::cout << "Gaussian scene data failures: " << failure_count << '\n';
    return failure_count ? 1 : 0;
}
