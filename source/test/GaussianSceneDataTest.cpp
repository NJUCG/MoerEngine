#include "scene/Scene.h"
#include "config/ConfigManager.h"
#include "scene/cache/SceneCache.h"
#include "scene/cache/SceneCacheSerializer.h"
#include "scene/loader/LoaderInterface.h"
#include "scene/LogicalScene.h"
#include "scene/gaussian/GaussianGpuScene.h"
#include "scene/loader/GaussianPlyLoader.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cstring>

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
void CheckMoca(Scene& scene, const std::filesystem::path& directory) {
    Check(scene.SaveStateCache(), "Gaussian state saves through existing cache API");
    SceneCacheSourceIdentity identity;
    Check(SceneCache::BuildSourceIdentity(scene.GetSourceFilePath(), identity), "cache source identity");
    const auto path = SceneCache::GetCachePath(identity, ESceneCacheKind::State);
    ecs::LogicalScene restored;
    Check(SceneCacheSerializer::LoadLogicalScene(path, restored), "v6 cache loads");
    const auto instances = restored.r().view<ecs::CGaussianInstance>();
    Check(instances.size() == 2, "cache retains all Gaussian instances");
    GaussianAssetRef shared_asset;
    for (auto entity : instances) {
        const auto& instance = instances.get<ecs::CGaussianInstance>(entity);
        if (!shared_asset) shared_asset = instance.asset;
        Check(instance.asset == shared_asset, "cache restores shared asset identity");
    }
    const auto original = scene.GetGaussianSnapshot()->instances[0].asset;
    Check(shared_asset && shared_asset->sh_degree == original->sh_degree &&
        shared_asset->gaussians.size() == original->gaussians.size() &&
        std::memcmp(shared_asset->gaussians.data(), original->gaussians.data(),
            original->gaussians.size() * sizeof(Render::GGaussian)) == 0,
        "canonical Gaussian CPU data survives cache exactly");
    Scene reopened;
    reopened.LoadSceneFromFile(scene.GetSourceFilePath());
    reopened.PrepareUpdateBatch(false, false);
    Check(reopened.WasLoadedFromStateCache(), "PLY reload uses state cache instead of parser");
    Check(Near(reopened.GetGaussianViewSettings().background.x, .15f) &&
        Near(reopened.GetGaussianViewSettings().exposure_ev, 1.25f), "cache restores view settings");
    const auto* hidden = reopened.GetGaussianSnapshot().get();
    Check(hidden && hidden->instances.size() == 2 &&
        std::count_if(hidden->instances.begin(), hidden->instances.end(), [](const auto& instance) {
            return !instance.is_visible_in_game && Near(instance.settings.opacity_multiplier, .25f) &&
                   Near(instance.world_transform.GetColumn(3).x, 4);
        }) == 1, "cache restores visibility, opacity and transform");

    // Include Mesh resources in the same v6 registry, including their buffer payload.
    const auto mesh_entity = restored.r().create();
    restored.r().emplace<ecs::CMesh>(mesh_entity);
    restored.r().ctx().get<ecs::CtxMegaBuffers>().position.push_back(float3(1,2,3));
    const auto mixed_path = directory / "mixed.moca";
    Check(SceneCacheSerializer::SaveLogicalScene(mixed_path, {}, restored), "mixed registry saves");
    ecs::LogicalScene mixed;
    Check(SceneCacheSerializer::LoadLogicalScene(mixed_path, mixed) &&
        mixed.r().view<ecs::CGaussianInstance>().size() == 2 && mixed.r().view<ecs::CMesh>().size() == 1 &&
        mixed.r().ctx().get<ecs::CtxMegaBuffers>().position.size() == 1,
        "v6 keeps Mesh resources and Gaussian assets together");

    // Failure must not replace the caller's existing registry with a partial scene.
    const auto corrupt_path = directory / "truncated.moca";
    std::filesystem::copy_file(mixed_path, corrupt_path);
    std::filesystem::resize_file(corrupt_path, std::filesystem::file_size(corrupt_path) - 1);
    Check(!SceneCacheSerializer::LoadLogicalScene(corrupt_path, mixed) &&
        mixed.r().view<ecs::CGaussianInstance>().size() == 2, "truncated cache preserves active registry");
    std::filesystem::copy_file(mixed_path, corrupt_path, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream corrupt(corrupt_path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(-4, std::ios::end);
        const uint32 invalid_capacity = 0;
        corrupt.write(reinterpret_cast<const char*>(&invalid_capacity), sizeof(invalid_capacity));
    }
    Check(!SceneCacheSerializer::LoadLogicalScene(corrupt_path, mixed) &&
        mixed.r().view<ecs::CGaussianInstance>().size() == 2, "invalid v6 tail preserves active registry");

    // An independent, fixed v5 layout: root node plus an empty Mesh resource.
    // This deliberately does not use the production scene serializer to create the legacy fixture.
    const auto legacy_path = directory / "legacy-v5.moca";
    {
        SceneCacheBinaryWriter writer(legacy_path);
        SceneCacheHeader header;
        writer.WritePod(header);
        writer.WritePod(uint32(5)); writer.WritePod(uint32(0)); writer.WritePod(uint64(2));
        for (int reserved_id = 0; reserved_id < 4; ++reserved_id) writer.WritePod(uint64(0));
        writer.WritePod(uint64(0)); writer.WritePod(uint64((1u<<0) | (1u<<1) | (1u<<14)));
        writer.WriteString("legacy-root");
        for (int reference_id = 0; reference_id < 5; ++reference_id)
            writer.WritePod(std::numeric_limits<uint64>::max());
        writer.WritePod(uint32(0)); writer.WritePod(uint32(0));
        const float3 translation(1,2,3), scale(1);
        const Quaternion rotation;
        writer.WriteBytes(&translation, sizeof(translation));
        writer.WriteBytes(&rotation, sizeof(rotation)); writer.WriteBytes(&scale, sizeof(scale));
        writer.WritePod(uint64(1)); writer.WritePod(uint64(1u<<12));
        writer.WritePod(uint64(0)); writer.WritePod(uint32(0)); writer.WritePod(uint64(0));
        header.payload_size = writer.Tell() - sizeof(header);
        writer.Seek(0); writer.WritePod(header); Check(writer.Flush(), "legacy fixture writes");
    }
    ecs::LogicalScene legacy;
    Check(SceneCacheSerializer::LoadLogicalScene(legacy_path, legacy) &&
        legacy.r().view<ecs::CNode>().size() == 1 && legacy.r().view<ecs::CMesh>().size() == 1 &&
        legacy.r().view<ecs::CGaussianInstance>().empty(), "pre-extension v5 Mesh cache remains readable");
    const auto mesh_path = directory / "mesh-v5.moca";
    Check(SceneCacheSerializer::SaveLogicalScene(mesh_path, {}, legacy), "Mesh-only cache still saves");
    SceneCacheBinaryReader mesh_reader(mesh_path);
    SceneCacheHeader mesh_header;
    uint32 version = 0;
    Check(mesh_reader.ReadPod(mesh_header) && mesh_reader.ReadPod(version) && version == 5,
        "Mesh-only writer retains v5 format");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("moer-gaussian-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        ConfigManager::GetInstance().Init(std::filesystem::current_path());
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
        Check(scene.SaveStateCache(), "unified state cache saves Gaussian data");
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
        CheckMoca(scene, directory);
        const auto document_path = directory / "scene.moergs";
        // Authored legacy fixture, independent of the removed .moergs writer.
        nlohmann::json document = {
            {"format", "MoerGaussianScene"}, {"version", 1},
            {"assets", {ascii_path.filename().string()}}, {"nodes", nlohmann::json::array()}
        };
        for (int node_id = 0; node_id < 3; ++node_id) {
            nlohmann::json node = {
                {"parent", node_id == 0 ? -1 : 0}, {"name", "legacy-" + std::to_string(node_id)},
                {"translation", node_id ? nlohmann::json{4,5,6} : nlohmann::json{0,0,0}},
                {"scale", {1,1,1}}, {"rotation", {1,0,0,0}},
                {"visible_game", node_id != 1}, {"visible_editor", true}
            };
            if (node_id) node["gaussian"] = {
                {"asset", 0}, {"enabled", true}, {"opacity", node_id == 1 ? .25f : 1.f}, {"color", {1,1,1}}
            };
            document["nodes"].push_back(node);
        }
        document["view"] = {{"background", {.15f,.2f,.25f}}, {"exposure_ev", 1.25f},
            {"display_mode", 1}, {"debug_view", 0}, {"tile_pair_capacity", 4194304}};
        Write(document_path, document.dump());
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
        Check(scene.SaveStateCache(), "empty Gaussian state can be saved");
        Scene empty_restored; empty_restored.LoadSceneFromFile(ascii_path);
        Check(empty_restored.IsReady() && empty_restored.HasGaussianSceneData() &&
            empty_restored.GetGaussianSceneContent()==EGaussianSceneContent::Empty,
            "empty cache retains Gaussian routing metadata");
        Check(!scene.RequestGaussianImageExport(directory/"bad.ply",document_error),"PNG request rejects a PLY destination");
        Check(scene.RequestGaussianImageExport(directory/"image.png",document_error),"PNG request accepted");
        const auto pending_export=scene.TakeGaussianImageExportRequest();
        Check(pending_export && !scene.TakeGaussianImageExportRequest(),"PNG request consumed exactly once");
        Check(!scene.RequestGaussianImageExport(directory/"other.png",document_error),"busy export cannot be replaced");
        const auto current_status=scene.GetGaussianImageExportStatus();
        auto stale_status=current_status;stale_status.request_id++;stale_status.is_busy=false;
        scene.ApplyGaussianImageExportStatus(stale_status);
        Check(scene.GetGaussianImageExportStatus().is_busy,"stale export completion ignored");
        scene.ApplyGaussianImageExportStatus({current_status.request_id,false,true,"saved"});
        Check(scene.GetGaussianImageExportStatus().is_successful,"current export completion applied");
        auto after = scene.PrepareUpdateBatch(false, false);
        Check(
            !after.update_gpu_update || !after.update_gpu_update->HasWork(),
            "empty scene has no repeating Gaussian update"
        );
        if (argc > 1) {
            ecs::LogicalScene existing_mesh;
            Check(SceneCacheSerializer::LoadLogicalScene(argv[1], existing_mesh) &&
                !existing_mesh.r().view<ecs::CMesh>().empty(), "existing pre-change Mesh cache loads");
            std::cout << "Existing Mesh cache loaded: " << existing_mesh.r().view<ecs::CMesh>().size()
                      << " meshes\n";
        }
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".ply" && entry.path().extension() != ".moergs") continue;
        SceneCacheSourceIdentity identity;
        if (SceneCache::BuildSourceIdentity(entry.path(), identity))
            for (auto kind : {ESceneCacheKind::Origin, ESceneCacheKind::State})
                std::filesystem::remove(SceneCache::GetCachePath(identity, kind));
    }
    std::filesystem::remove_all(directory); // Test-owned unique directory only.
    std::cout << "Gaussian scene data failures: " << failure_count << '\n';
    return failure_count ? 1 : 0;
}
