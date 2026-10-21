#include "GaussianSceneFile.h"
#include "scene/LogicalScene.h"
#include "scene/loader/GaussianPlyLoader.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
namespace Moer {
namespace {
using Json                               = nlohmann::json;
constexpr size_t    k_max_node_count     = 1000000;
constexpr uint64    k_max_document_bytes = 64u * 1024u * 1024u;
std::atomic<uint64> g_save_sequence{0};
std::string         Utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path Path(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
float Number(const Json& value) {
    if (!value.is_number())
        throw std::runtime_error("Expected a finite number");
    const float number = value.get<float>();
    if (!std::isfinite(number))
        throw std::runtime_error("Nonfinite scene value");
    return number;
}
float3 VectorValue(const Json& value) {
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error("Expected a 3-vector");
    return float3(Number(value[0]), Number(value[1]), Number(value[2]));
}
Json VectorValue(float3 value) {
    return Json::array({value.x, value.y, value.z});
}
Quaternion Rotation(const Json& value) {
    if (!value.is_array() || value.size() != 4)
        throw std::runtime_error("Expected a wxyz quaternion");
    const double w = Number(value[0]), x = Number(value[1]), y = Number(value[2]), z = Number(value[3]);
    const double length = std::sqrt(w * w + x * x + y * y + z * z);
    if (length < 1e-12)
        throw std::runtime_error("Zero quaternion");
    if (std::abs(length - 1.0) < 1e-5)
        return Quaternion(float(w), float(x), float(y), float(z));
    return Quaternion(float(w / length), float(x / length), float(y / length), float(z / length));
}
Json CameraData(const Camera& camera) {
    return {
        {"position", VectorValue(camera.GetPosition())},
        {"yaw", camera.GetYaw()},
        {"pitch", camera.GetPitch()},
        {"fov", camera.GetFov()},
        {"aspect", camera.GetAspectRatio()},
        {"near", camera.GetNearClip()},
        {"far", camera.GetFarClip()}
    };
}
void ReadCamera(const Json& value, Camera& out_camera) {
    const float fov = Number(value.at("fov")), aspect = Number(value.at("aspect"));
    const float near_clip = Number(value.at("near")), far_clip = Number(value.at("far"));
    if (fov <= 0 || fov >= 180 || aspect <= 0 || near_clip <= 0 || far_clip <= near_clip)
        throw std::runtime_error("Invalid camera projection");
    out_camera.Initialize(
        VectorValue(value.at("position")),
        Number(value.at("yaw")),
        Number(value.at("pitch")),
        fov,
        aspect,
        near_clip,
        far_clip
    );
}
void CheckDocument(const Json& document) {
    if (document.at("format") != "MoerGaussianScene" || document.at("version") != 1)
        throw std::runtime_error("Unsupported Gaussian scene document/version");
    const auto& nodes = document.at("nodes");
    if (!nodes.is_array() || nodes.empty() || nodes.size() > k_max_node_count)
        throw std::runtime_error("Invalid Gaussian scene node count");
    if (!document.at("assets").is_array() || document.at("assets").size() > k_max_node_count)
        throw std::runtime_error("Invalid asset table");
}
} // namespace
bool SaveGaussianSceneFile(
    const std::filesystem::path& path,
    const ecs::LogicalScene&     scene,
    std::string&                 out_error
) {
    std::filesystem::path temporary_path;
    out_error.clear();
    try {
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        if (extension != ".moergs")
            throw std::runtime_error("Gaussian scenes must use the .moergs extension");
        const auto& registry = scene.r();
        if (!registry.view<const ecs::CRenderable>().empty() ||
            !registry.view<const ecs::CPrimitive>().empty() || !registry.view<const ecs::CMesh>().empty() ||
            !registry.view<const ecs::CTexture>().empty())
            throw std::runtime_error(
                "This document format supports Gaussian scenes only; Mesh/resources must not be discarded"
            );
        const auto roots = registry.view<const ecs::CTagRootNode, const ecs::CNode>();
        if (roots.size_hint() != 1)
            throw std::runtime_error("Expected one root node");
        const auto                               root_entity = *roots.begin();
        std::vector<entt::entity>                nodes{root_entity};
        std::unordered_map<entt::entity, size_t> node_ids{{root_entity, 0}};
        for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
            auto child_entity = registry.get<ecs::CNode>(nodes[node_id]).first_child_entt;
            while (child_entity != entt::null) {
                if (nodes.size() >= k_max_node_count || !node_ids.emplace(child_entity, nodes.size()).second)
                    throw std::runtime_error("Invalid/cyclic scene hierarchy");
                nodes.push_back(child_entity);
                child_entity = registry.get<ecs::CNode>(child_entity).next_sibling_entt;
            }
        }
        if (nodes.size() != registry.view<const ecs::CNode>().size())
            throw std::runtime_error("Detached nodes cannot be omitted");
        Json document = {
            {"format", "MoerGaussianScene"},
            {"version", 1},
            {"assets", Json::array()},
            {"nodes", Json::array()}
        };
        std::unordered_map<const GaussianAsset*, size_t> asset_ids;
        const auto directory = std::filesystem::absolute(path).parent_path();
        for (const auto entity : nodes) {
            const auto& node       = registry.get<ecs::CNode>(entity);
            const auto* visibility = registry.try_get<ecs::CVisibility>(entity);
            Json        item       = {
                {"parent", entity == root_entity ? -1 : int64(node_ids.at(node.parent_entt))},
                {"name", node.name},
                {"translation", VectorValue(node.translation)},
                {"scale", VectorValue(node.scale)},
                {"rotation",
                 Json::array(
                     {node.rotation.vec.w, node.rotation.vec.x, node.rotation.vec.y, node.rotation.vec.z}
                 )},
                {"visible_game", !visibility || visibility->visible_in_game},
                {"visible_editor", !visibility || visibility->visible_in_editor}
            };
            if (const auto* instance = registry.try_get<ecs::CGaussianInstance>(entity)) {
                if (!instance->asset || instance->asset->source_path.empty() ||
                    !std::filesystem::is_regular_file(instance->asset->source_path))
                    throw std::runtime_error("Every Gaussian asset needs an existing source PLY");
                auto [it, is_new] = asset_ids.emplace(instance->asset.get(), asset_ids.size());
                if (is_new) {
                    std::error_code error;
                    auto source = std::filesystem::relative(instance->asset->source_path, directory, error);
                    if (error || source.empty())
                        source = instance->asset->source_path;
                    document["assets"].push_back(Utf8(source));
                }
                item["gaussian"] = {
                    {"asset", it->second},
                    {"enabled", instance->settings.is_enabled},
                    {"opacity", instance->settings.opacity_multiplier},
                    {"color", VectorValue(instance->settings.color_multiplier)}
                };
            }
            if (const auto* camera = registry.try_get<ecs::CCamera>(entity)) {
                item["camera"]      = CameraData(camera->camera);
                item["main_camera"] = registry.all_of<ecs::CTagMainCamera>(entity);
            }
            if (registry.all_of<ecs::CLight>(entity)) {
                const auto write_light = [&](const auto& light, const char* kind) {
                    item["light"] = {
                        {"kind", kind},
                        {"color", VectorValue(light.color)},
                        {"intensity", light.intensity},
                        {"main", registry.all_of<ecs::CTagMainLight>(entity)}
                    };
                };
                if (const auto* light = registry.try_get<ecs::CLightDirectional>(entity))
                    write_light(*light, "directional");
                else if (const auto* light = registry.try_get<ecs::CLightPoint>(entity))
                    write_light(*light, "point");
                else if (const auto* light = registry.try_get<ecs::CLightAmbient>(entity))
                    write_light(*light, "ambient");
                else
                    throw std::runtime_error("Unsupported light type in Gaussian document");
            }
            document["nodes"].push_back(std::move(item));
        }
        const auto settings = registry.ctx().contains<ecs::CtxGaussianViewSettings>() ?
                                  registry.ctx().get<const ecs::CtxGaussianViewSettings>().value :
                                  GaussianViewSettings{};
        document["view"]    = {
            {"background", VectorValue(settings.background)},
            {"exposure_ev", settings.exposure_ev},
            {"display_mode", static_cast<uint>(settings.display_mode)},
            {"debug_view", static_cast<uint>(settings.debug_view)},
            {"tile_pair_capacity", settings.tile_pair_capacity}
        };
        const auto text = document.dump(2);
        if (text.size() > k_max_document_bytes)
            throw std::runtime_error("Scene document is too large");
        temporary_path = path;
        temporary_path += ".tmp-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                          std::to_string(g_save_sequence++);
        {
            std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.write(text.data(), text.size());
            output.close();
        }
#if PLATFORM_WINDOWS
        if (!::MoveFileExW(
                temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            ))
            throw std::runtime_error(
                "Cannot publish Gaussian scene file (Windows error " + std::to_string(::GetLastError()) + ")"
            );
#else
        std::filesystem::rename(temporary_path, path);
#endif
        return true;
    } catch (const std::exception& error) {
        out_error = error.what();
        if (!temporary_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
        }
        return false;
    }
}
bool LoadGaussianSceneFile(
    const std::filesystem::path& path,
    ecs::LogicalScene&           out_scene,
    std::string&                 out_error
) {
    out_error.clear();
    try {
        if (std::filesystem::file_size(path) > k_max_document_bytes)
            throw std::runtime_error("Scene document is too large");
        std::ifstream input(path, std::ios::binary);
        const auto    document = Json::parse(input);
        CheckDocument(document);
        std::vector<GaussianAssetRef>                     assets;
        std::unordered_map<std::string, GaussianAssetRef> shared_assets;
        for (const auto& entry : document.at("assets")) {
            const auto source = std::filesystem::weakly_canonical(
                std::filesystem::absolute(path).parent_path() / Path(entry.get<std::string>())
            );
            const auto key   = Utf8(source);
            auto&      asset = shared_assets[key];
            if (!asset) {
                auto loaded = LoadGaussianPly(source);
                if (!loaded)
                    throw std::runtime_error(loaded.error_message);
                asset = std::move(loaded.asset);
            }
            assets.push_back(asset);
        }
        ecs::LogicalScene staged;
        auto&             registry = staged.r();
        registry.ctx().emplace<ecs::CtxMegaBuffers>();
        auto& settings = registry.ctx().emplace<ecs::CtxGaussianViewSettings>().value;
        if (document.contains("view")) {
            const auto& value          = document.at("view");
            settings.background        = VectorValue(value.at("background"));
            settings.exposure_ev       = Number(value.at("exposure_ev"));
            const auto bounded_integer = [](const Json& number, uint64 maximum) -> uint {
                if (!number.is_number_integer() || number.get<int64>() < 0 || number.get<uint64>() > maximum)
                    throw std::runtime_error("Invalid Gaussian view integer");
                return static_cast<uint>(number.get<uint64>());
            };
            settings.display_mode =
                static_cast<EGaussianDisplayMode>(bounded_integer(value.at("display_mode"), 1));
            settings.debug_view = static_cast<EGaussianDebugView>(bounded_integer(value.at("debug_view"), 2));
            settings.tile_pair_capacity = bounded_integer(value.at("tile_pair_capacity"), 1u << 24);
            if (!IsValidGaussianViewSettings(settings))
                throw std::runtime_error("Invalid Gaussian view settings");
        }
        std::vector<entt::entity> entities;
        uint                      main_camera_count = 0, main_light_count = 0;
        for (const auto& item : document.at("nodes")) {
            const auto  node_id = entities.size();
            const auto& parent  = item.at("parent");
            if (!parent.is_number_integer())
                throw std::runtime_error("Node parent must be an integer");
            const auto parent_id = parent.get<int64>();
            if ((node_id == 0 && parent_id != -1) ||
                (node_id > 0 && (parent_id < 0 || uint64(parent_id) >= node_id)))
                throw std::runtime_error("Parents must precede children; cycles/detached nodes are invalid");
            EntityWithNodeCreateInfo info;
            info.name        = item.at("name").get<std::string>();
            info.translation = VectorValue(item.at("translation"));
            info.scale       = VectorValue(item.at("scale"));
            info.rotation    = Rotation(item.at("rotation"));
            entt::entity entity;
            if (node_id == 0) {
                entity = registry.create();
                registry.emplace<ecs::CTagRootNode>(entity);
                auto& node       = registry.emplace<ecs::CNode>(entity);
                node.name        = info.name;
                node.translation = info.translation;
                node.rotation    = info.rotation;
                node.scale       = info.scale;
            } else {
                info.parent_node_entt = entities[size_t(parent_id)];
                entity                = staged.UCreateEntityWithNode(info);
                if (entity == entt::null)
                    throw std::runtime_error("Cannot construct node hierarchy");
            }
            entities.push_back(entity);
            registry.emplace_or_replace<ecs::CVisibility>(
                entity, item.at("visible_editor").get<bool>(), item.at("visible_game").get<bool>()
            );
            if (item.contains("gaussian")) {
                const auto& gaussian    = item.at("gaussian");
                const auto& asset_value = gaussian.at("asset");
                if (!asset_value.is_number_unsigned() && !asset_value.is_number_integer())
                    throw std::runtime_error("Invalid asset ID");
                const auto asset_id = asset_value.get<int64>();
                if (asset_id < 0 || uint64(asset_id) >= assets.size())
                    throw std::runtime_error("Gaussian asset ID out of range");
                GaussianInstanceSettings settings;
                settings.is_enabled         = gaussian.at("enabled").get<bool>();
                settings.opacity_multiplier = Number(gaussian.at("opacity"));
                settings.color_multiplier   = VectorValue(gaussian.at("color"));
                if (settings.opacity_multiplier < 0 || settings.color_multiplier.x < 0 ||
                    settings.color_multiplier.y < 0 || settings.color_multiplier.z < 0)
                    throw std::runtime_error("Negative Gaussian multiplier");
                registry.emplace<ecs::CGaussianInstance>(entity, assets[size_t(asset_id)], settings);
            }
            if (item.contains("camera")) {
                ReadCamera(item.at("camera"), registry.emplace<ecs::CCamera>(entity).camera);
                if (item.at("main_camera").get<bool>()) {
                    registry.emplace<ecs::CTagMainCamera>(entity);
                    ++main_camera_count;
                }
            }
            if (item.contains("light")) {
                const auto& light     = item.at("light");
                const auto  kind      = light.at("kind").get<std::string>();
                const auto  color     = VectorValue(light.at("color"));
                const float intensity = Number(light.at("intensity"));
                if (intensity < 0 || color.x < 0 || color.y < 0 || color.z < 0)
                    throw std::runtime_error("Invalid light value");
                ELightType type;
                if (kind == "directional") {
                    type            = ELightType::Directional;
                    auto& value     = registry.emplace<ecs::CLightDirectional>(entity);
                    value.color     = color;
                    value.intensity = intensity;
                } else if (kind == "point") {
                    type            = ELightType::Point;
                    auto& value     = registry.emplace<ecs::CLightPoint>(entity);
                    value.color     = color;
                    value.intensity = intensity;
                } else if (kind == "ambient") {
                    type            = ELightType::Ambient;
                    auto& value     = registry.emplace<ecs::CLightAmbient>(entity);
                    value.color     = color;
                    value.intensity = intensity;
                } else
                    throw std::runtime_error("Unknown light type");
                registry.emplace<ecs::CLight>(entity, type);
                if (light.at("main").get<bool>()) {
                    registry.emplace<ecs::CTagMainLight>(entity);
                    ++main_light_count;
                }
            }
        }
        if (main_camera_count > 1 || main_light_count > 1)
            throw std::runtime_error("Duplicate main camera/light");
        if (main_camera_count == 0)
            staged.UCreateDefaultCamera();
        if (main_light_count == 0)
            staged.UCreateDefaultLights();
        staged.Update();
        out_scene.r() = std::move(registry);
        return true;
    } catch (const std::exception& error) {
        out_error = error.what();
        return false;
    }
}
} // namespace Moer
