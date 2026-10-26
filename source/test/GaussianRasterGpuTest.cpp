#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "renderer/raster/GaussianImageExport.h"
#include "renderer/raster/GaussianSplatPass.h"
#include "renderer/raster/GaussianCompositePass.h"
#include "renderer/raster/GaussianShadowPass.h"
#include "scene/RenderScene.h"
#include "scene/Scene.h"
#include "rhi/RHIExecutor.h"
#include "scene/loader/GaussianPlyLoader.h"
#include "taskgraph/TaskSystem.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stb/stb_image.h>
#include <stdexcept>
#include <thread>
using namespace Moer;
using namespace Moer::Render;
namespace {
class GaussianShadowQueryPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianShadowQueryPipeline);
    DEFINE_SHADER_CONSTANT(uint4, param);
    DEFINE_SHADER_BUFFER(typed_views);
    DEFINE_SHADER_BUFFER(output);
    DEFINE_SHADER_BUFFER(queries);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(param, typed_views, output, queries, bdls);
};
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::shared_ptr<GaussianAsset> Synthetic(uint count) {
    auto asset = std::make_shared<GaussianAsset>();
    for (uint gaussian_id = 0; gaussian_id < count; ++gaussian_id) {
        GGaussian gaussian{};
        gaussian.position      = float4(0, 0, -3.f - float((count - gaussian_id) / 2) * 0.001f, 1);
        gaussian.rotation      = float4(0, 0, 0, 1);
        gaussian.scale_opacity = float4(0.08f, 0.08f, 0.08f, count > 2 && count != 32 ? 0.008f : 0.5f);
        const float3 color     = gaussian_id % 2 ? float3(0, 1, 0) : float3(1, 0, 0);
        gaussian.sh[0]         = float4(
            (color.x - .5f) / .2820947918f, (color.y - .5f) / .2820947918f, (color.z - .5f) / .2820947918f, 0
        );
        asset->gaussians.push_back(gaussian);
    }
    return asset;
}
void Run(
    ShaderManager&               manager,
    GaussianAssetRef             asset,
    uint                         count,
    const std::filesystem::path& output                = {},
    bool                         should_test_instances = false,
    bool                         should_test_overflow  = false,
    uint2                        resolution            = uint2(64, 64),
    uint                         pair_count_limit      = 0
) {
    const uint width = output.empty() ? resolution.x : 256;
    const uint height = output.empty() ? resolution.y : 256;
    Camera     camera;
    camera.Initialize(float3(0), -90, 0, 60, float(width) / height, 0.01f, 100.f);
    if (!output.empty()) {
        const auto  bounds = asset->bounds;
        const auto  extent = bounds.max - bounds.min;
        const float radius = std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z) * .5f;
        camera.Initialize(bounds.GetCenter() + float3(0, 0, radius * 2.4f), -90, 0, 60, 1, 0.001f, 100.f);
    }
    GaussianGpuScene scene;
    auto             snapshot = std::make_shared<GaussianSceneSnapshot>();
    snapshot->revision        = 1;
    GaussianInstanceSnapshot instance;
    instance.asset = asset;
    snapshot->instances.push_back(instance);
    if (should_test_instances) {
        instance.world_transform[0][0] = instance.world_transform[1][1] = instance.world_transform[2][2] =
            0.7f;
        instance.world_transform[0][3]       = 0.4f;
        instance.world_transform[2][3]       = -0.5f;
        instance.settings.opacity_multiplier = .6f;
        snapshot->instances.push_back(instance);
    }
    CommandList commands(EQueueType::Graphics);
    scene.ApplyUpdate(snapshot, commands);
    Raster::GaussianSplatPass pass(
        manager, {.tile_pair_capacity = should_test_overflow ? 128u : 4u * 1024u * 1024u}
    );
    const auto display        = pass.Record(commands, &scene, camera, uint2(width, height), false);
    auto       display_future = commands.Readback(display->GetView(), "Gaussian display RGB");
    auto       color_future   = commands.Readback(pass.GetColor()->GetView(), "Gaussian test RGB");
    auto transmittance_future = commands.Readback(pass.GetTransmittance()->GetView(), "Gaussian test T");
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const auto color         = color_future.Get().CopyAs<float>();
    const auto transmittance = transmittance_future.Get().CopyAs<float>();
    Require(color && color->size() == width * height * 4, "RGB readback invalid");
    Require(transmittance && transmittance->size() == width * height, "T readback invalid");
    const auto display_pixels = display_future.Get().CopyAs<float>();
    Require(display_pixels && display_pixels->size() == color->size(), "Display readback invalid");
    for (uint pixel_id = 0; pixel_id < width * height; ++pixel_id) {
        for (uint channel_id = 0; channel_id < 3; ++channel_id) {
            const uint   value_id = pixel_id * 4 + channel_id;
            const double encoded  = std::clamp(double((*color)[value_id]), 0., 1.);
            const double linear =
                encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
            Require(std::abs(linear - (*display_pixels)[value_id]) < 0.00002, "Display sRGB decode differs");
        }
    }
    {
        const auto directory = std::filesystem::temp_directory_path() /
                               ("moer-gaussian-export-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        const auto path = output.empty() ? directory / std::filesystem::path(u8"高斯.png") :
                                           std::filesystem::path(output.string() + ".export.png");
        if (should_test_overflow) {
            std::ofstream existing(path, std::ios::binary);
            existing << "keep-existing";
        }
        Raster::GaussianImageExport exporter;
        CommandList                 export_commands(EQueueType::Graphics);
        exporter.Request(
            export_commands, display->GetView(), pass.GetStatusBuffer(), uint2(width, height), path, 1
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics, export_commands.Submit(), ERHIExecSubmitFlags::FlushGPU
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (exporter.GetStatus().is_busy && std::chrono::steady_clock::now() < deadline) {
            exporter.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!exporter.GetStatus().is_busy, "PNG export timed out");
        if (should_test_overflow) {
            Require(!exporter.GetStatus().is_successful, "Invalid overflow frame was exported");
            std::ifstream existing(path, std::ios::binary);
            std::string   content((std::istreambuf_iterator<char>(existing)), {});
            Require(content == "keep-existing", "Invalid PNG export changed existing destination");
        } else {
            Require(exporter.GetStatus().is_successful, exporter.GetStatus().message.c_str());
            std::ifstream              input(path, std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
            int                        decoded_width = 0, decoded_height = 0, component_count = 0;
            auto*                      pixels = stbi_load_from_memory(
                bytes.data(), int(bytes.size()), &decoded_width, &decoded_height, &component_count, 3
            );
            Require(
                pixels && decoded_width == int(width) && decoded_height == int(height),
                "PNG dimensions mismatch"
            );
            for (uint pixel_id = 0; pixel_id < width * height; ++pixel_id)
                for (uint channel_id = 0; channel_id < 3; ++channel_id) {
                    const float expected = std::clamp((*color)[pixel_id * 4 + channel_id], 0.f, 1.f) * 255.f;
                    Require(
                        std::abs(float(pixels[pixel_id * 3 + channel_id]) - expected) < 1.1f,
                        "PNG RGB / sRGB encoding mismatch"
                    );
                }
            stbi_image_free(pixels);
        }
        std::filesystem::remove_all(directory); // Unique, test-owned temporary directory only.
    }
    if (!output.empty()) {
        std::ofstream raw(output.string() + ".rgba", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(color->data()), color->size() * sizeof(float));
        std::ofstream camera_file(output.string() + ".camera.txt");
        camera_file.precision(10);
        const auto view = camera.GetViewMatrix(), projection = camera.GetProjectionMatrix();
        for (uint row_id = 0; row_id < 4; ++row_id)
            for (uint column_id = 0; column_id < 4; ++column_id)
                camera_file << view[row_id][column_id] << " ";
        camera_file << "\n";
        for (uint row_id = 0; row_id < 4; ++row_id)
            for (uint column_id = 0; column_id < 4; ++column_id)
                camera_file << projection[row_id][column_id] << " ";
        const auto position = camera.GetPosition();
        camera_file << "\n" << position.x << " " << position.y << " " << position.z << "\n";
        std::ofstream image(output, std::ios::binary);
        image << "P6\n" << width << " " << height << "\n255\n";
        for (uint pixel_id = 0; pixel_id < width * height; ++pixel_id)
            for (uint channel_id = 0; channel_id < 3; ++channel_id) {
                const float value = (*color)[pixel_id * 4 + channel_id];
                Require(std::isfinite(value), "Nonfinite real asset pixel");
                image.put(static_cast<char>(std::clamp(value, 0.f, 1.f) * 255.f + .5f));
            }
        return;
    }
    if (should_test_overflow) {
        for (uint pixel_id = 0; pixel_id < width * height; ++pixel_id) {
            Require(
                (*color)[pixel_id * 4] == 1 && (*color)[pixel_id * 4 + 1] == 0 &&
                    (*color)[pixel_id * 4 + 2] == 1,
                "Overflow was not diagnosed as magenta"
            );
            Require((*transmittance)[pixel_id] == 1, "Overflow changed transmittance");
        }
        std::cout << "overflow diagnostic passed" << std::endl;
        return;
    }
    struct ReferenceSplat {
        double depth, x, y, scale_x, scale_y, scale_z, cos_angle, sin_angle, opacity;
        uint   gaussian_id;
    };
    std::vector<ReferenceSplat> order;
    for (const auto& item : snapshot->instances)
        for (uint gaussian_id = 0; gaussian_id < count; ++gaussian_id) {
            const auto&  gaussian = asset->gaussians[gaussian_id];
            const auto&  matrix   = item.world_transform;
            const double scale    = matrix[0][0];
            order.push_back(
                {-(gaussian.position.z * scale + matrix[2][3]),
                 matrix[0][3],
                 matrix[1][3],
                 gaussian.scale_opacity.x * scale,
                 gaussian.scale_opacity.y * scale,
                 gaussian.scale_opacity.z * scale,
                 1. - 2. * gaussian.rotation.z * gaussian.rotation.z,
                 2. * gaussian.rotation.w * gaussian.rotation.z,
                 gaussian.scale_opacity.w * item.settings.opacity_multiplier,
                 gaussian_id}
            );
        }
    std::stable_sort(order.begin(), order.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.depth < rhs.depth;
    });
    double max_error = 0;
    for (uint y = 0; y < height; ++y)
        for (uint x = 0; x < width; ++x) {
            double expected[3] = {}, remaining = 1;
            for (const auto& splat : order) {
                const double depth = splat.depth, focal = height / (2 * std::tan(3.141592653589793 / 6));
                // Independent pinhole covariance for the tests' Z-axis rotations.
                const double sx = splat.scale_x * splat.scale_x;
                const double sy = splat.scale_y * splat.scale_y;
                const double sz = splat.scale_z * splat.scale_z;
                const double cs = splat.cos_angle, sn = splat.sin_angle;
                const double factor = focal * focal / (depth * depth);
                const double a = factor * (cs * cs * sx + sn * sn * sy +
                    splat.x * splat.x * sz / (depth * depth)) + .3;
                const double b = -factor * (cs * sn * (sx - sy) + splat.x * splat.y * sz / (depth * depth));
                const double c = factor * (sn * sn * sx + cs * cs * sy +
                    splat.y * splat.y * sz / (depth * depth)) + .3;
                const double dx     = double(x) - (.5 * (width - 1) + focal * splat.x / depth),
                             dy     = double(y) - (.5 * (height - 1) - focal * splat.y / depth);
                const double alpha  = std::min(
                    .99,
                    splat.opacity *
                        std::exp(-.5 * (c * dx * dx - 2 * b * dx * dy + a * dy * dy) / (a * c - b * b))
                );
                if (alpha < 1. / 255)
                    continue;
                if (remaining * (1 - alpha) < .0001)
                    break;
                expected[splat.gaussian_id % 2] += remaining * alpha;
                remaining *= 1 - alpha;
            }
            const uint pixel_id = x + y * width;
            for (uint channel_id = 0; channel_id < 3; ++channel_id)
                max_error =
                    std::max(max_error, std::abs(expected[channel_id] - (*color)[pixel_id * 4 + channel_id]));
            max_error = std::max(max_error, std::abs(remaining - (*transmittance)[pixel_id]));
        }
    std::cout << "count=" << count << " max_error=" << max_error << std::endl;
    Require(max_error < 0.0002, "Gaussian pixels disagree with double precision CPU reference");
    if (pair_count_limit != 0) {
        const auto status = pass.GetStatus();
        Require(status.has_completed_counts && status.tile_pair_count < pair_count_limit,
            "Conservative culling did not reduce the rotated splat's tile rectangle");
    }
    // Shrink to empty and reuse the same workspace: no previous tile/range contents may leak.
    CommandList empty_commands(EQueueType::Graphics);
    pass.Record(empty_commands, nullptr, camera, uint2(32, 48), false);
    auto empty_future = empty_commands.Readback(pass.GetColor()->GetView(), "Gaussian empty RGB");
    RHIExecutor::Get().Submit(EQueueType::Graphics, empty_commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    const auto empty = empty_future.Get().CopyAs<float>();
    Require(
        empty && std::all_of(
                     empty->begin(),
                     empty->end(),
                     [](float value) {
                         return value == 0;
                     }
                 ),
        "Empty resized frame retained color"
    );
}
void RunViewSettings(ShaderManager& manager) {
    GaussianGpuScene         scene;
    auto                     snapshot = std::make_shared<GaussianSceneSnapshot>();
    GaussianInstanceSnapshot instance;
    instance.asset = Synthetic(1);
    snapshot->instances.push_back(instance);
    CommandList upload(EQueueType::Graphics);
    scene.ApplyUpdate(snapshot, upload);
    RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit(), ERHIExecSubmitFlags::FlushGPU);
    Raster::GaussianSplatPass pass(manager, {.tile_pair_capacity = 129});
    Camera                    camera;
    camera.Initialize(float3(0), -90, 0, 60, 1, .01f, 100.f);
    for (uint mode_id = 0; mode_id < 4; ++mode_id) {
        GaussianViewSettings settings;
        settings.background = float3(.2f, .3f, .4f);
        if (mode_id == 1) {
            settings.display_mode = EGaussianDisplayMode::Engine;
            settings.exposure_ev  = 2;
        }
        if (mode_id == 2)
            settings.debug_view = EGaussianDebugView::Transmittance;
        if (mode_id == 3)
            settings.debug_view = EGaussianDebugView::TileCoverage;
        CommandList commands(EQueueType::Graphics);
        const auto  display    = pass.Record(commands, &scene, camera, uint2(64, 64), false, false, settings);
        auto        rgb_future = commands.Readback(pass.GetColor()->GetView(), "view raw");
        auto        transmittance_future = commands.Readback(pass.GetTransmittance()->GetView(), "view T");
        auto        display_future       = commands.Readback(display->GetView(), "view display");
        RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
        const auto rgb           = rgb_future.Get().CopyAs<float>();
        const auto transmittance = transmittance_future.Get().CopyAs<float>();
        const auto pixels        = display_future.Get().CopyAs<float>();
        Require(rgb && transmittance && pixels, "Display setting readback failed");
        if (mode_id < 3)
            for (uint pixel_id = 0; pixel_id < 64 * 64; ++pixel_id)
                for (uint channel_id = 0; channel_id < 3; ++channel_id) {
                    double encoded = (*rgb)[pixel_id * 4 + channel_id] +
                                     (*transmittance)[pixel_id] * settings.background[channel_id];
                    if (mode_id == 0)
                        encoded = std::clamp(encoded, 0., 1.);
                    double expected =
                        encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
                    if (mode_id == 1)
                        expected = expected * 4 / (1 + expected * 4);
                    if (mode_id == 2)
                        expected = (*transmittance)[pixel_id];
                    Require(
                        std::abs(expected - (*pixels)[pixel_id * 4 + channel_id]) < 0.00002,
                        "Display setting pixel mismatch"
                    );
                }
        if (mode_id == 3) {
            Require((*pixels)[0] == 0 && (*pixels)[2] == 1, "Empty tile coverage mismatch");
            Require(
                std::abs((*pixels)[(32 + 32 * 64) * 4] - 1.f / 12.f) < 1e-6, "Covered tile density mismatch"
            );
        }
        const auto status = pass.GetStatus();
        Require(
            status.has_completed_counts && !status.has_overflow && status.visible_count == 1 &&
                status.tile_pair_count == 4,
            "GPU telemetry count mismatch"
        );
    }
    std::cout << "background, engine preview, debug views and GPU counters passed" << std::endl;
}

void RunInFlightResize(ShaderManager& manager) {
    GaussianGpuScene         scene;
    auto                     snapshot = std::make_shared<GaussianSceneSnapshot>();
    GaussianInstanceSnapshot instance;
    instance.asset = Synthetic(2);
    snapshot->instances.push_back(instance);
    CommandList upload(EQueueType::Graphics);
    scene.ApplyUpdate(snapshot, upload);
    RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit(), ERHIExecSubmitFlags::None);
    Raster::GaussianSplatPass     pass(manager);
    auto                          bindless = RenderDevice::Get().CreateBindlessArray();
    Raster::GaussianCompositePass composite(manager, bindless);
    Camera                        camera;
    camera.Initialize(float3(0), -90, 0, 60, 1, .01f, 100.f);
    std::vector<ReadbackFuture> results;
    for (uint frame_id = 0; frame_id < 8; ++frame_id) {
        pass.SetPairCapacity(frame_id % 2 ? 1024u : 4096u);
        CommandList commands(EQueueType::Graphics);
        const uint2 size(64 + frame_id * 8, 64 + frame_id * 4);
        if (frame_id % 2 == 0) {
            pass.Record(commands, &scene, camera, size, false);
            results.push_back(
                commands.Readback(pass.GetColor()->GetView(), "Gaussian reference in-flight resize")
            );
        } else {
            auto parameters = pass.Prepare(&scene, camera, size, false, {}, TextureRef{}, frame_id % 3 == 0);
            pass.Record(commands, parameters);
            auto background = RenderDevice::Get().CreateTexture(
                "In-flight scene background",
                Extent3D(size.x, size.y, 1),
                PF_R32G32B32A32_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
                    ETextureUsageFlags::TRANSFER_DST
            );
            commands.ClearResource(background->GetView(), float4(.1f, .2f, .3f, 1));
            auto parameters_composite = composite.Prepare(
                TextureWithHandle{.tex = background},
                pass.GetColor(),
                pass.GetTransmittance(),
                BufferRef(pass.GetStatusBuffer().GetBuffer()),
                pass.GetRanges()
            );
            composite.Record(commands, parameters_composite);
            results.push_back(commands.Readback(
                parameters_composite.output.tex->GetView(), "Gaussian common in-flight resize"
            ));
        }
        RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::None);
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    for (auto& future : results) {
        const auto pixels = future.Get().CopyAs<float>();
        Require(
            pixels && std::all_of(
                          pixels->begin(),
                          pixels->end(),
                          [](float value) {
                              return std::isfinite(value);
                          }
                      ),
            "In-flight resize returned invalid pixels"
        );
        Require(
            std::any_of(
                pixels->begin(),
                pixels->end(),
                [](float value) {
                    return value > 0;
                }
            ),
            "In-flight resize lost color"
        );
    }
    std::cout << "in-flight resize passed" << std::endl;
}

void RunViewportPng() {
    auto&      device = RenderDevice::Get();
    const auto path  = std::filesystem::temp_directory_path() /
                       ("moer-viewport-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
    auto       image = device.CreateTexture(
        "Viewport PNG contract",
        Extent3D(2, 1, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
    );
    auto status = device.CreateBuffer<uint>(
        "Viewport PNG status",
        4,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
    );
    unsigned char pixels[] = {0, 128, 255, 255, 64, 0, 0, 255};
    CommandList   commands(EQueueType::Graphics);
    commands.CopyFrom(std::span<byte>(reinterpret_cast<byte*>(pixels), sizeof(pixels)), image->GetView());
    commands.ClearResource(status->GetView(), 0u);
    Raster::GaussianImageExport exporter;
    exporter.Request(commands, image->GetView(), status->GetView(), uint2(2, 1), path, 1);
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
    exporter.Finish();
    Require(exporter.GetStatus().is_successful, "Viewport RGBA8 PNG export failed");
    std::ifstream              file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    int                        width = 0, height = 0, channels = 0;
    auto* decoded = stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 3);
    Require(decoded && width == 2 && height == 1, "Viewport PNG dimensions invalid");
    Require(
        decoded[0] == 0 && std::abs(int(decoded[1]) - 188) <= 1 && decoded[2] == 255 &&
            std::abs(int(decoded[3]) - 137) <= 1,
        "Viewport PNG must encode linear UNORM exactly once"
    );
    stbi_image_free(decoded);
    file.close();
    std::filesystem::remove(path);
    std::cout << "viewport RGBA8 PNG passed" << std::endl;
}

void RunHybrid(
    ShaderManager& manager,
    bool           use_graph,
    float          shadow_strength  = -1,
    bool           cube_shadow      = false,
    bool           debug_visibility = false
) {
    constexpr uint width = 65, height = 49;
    Camera         camera;
    camera.Initialize(float3(0), -90, 0, 60, float(width) / height, .01f, 100.f);
    auto         asset  = Synthetic(2);
    const float3 rgb[2] = {float3(.5f, .25f, .75f), float3(0, 1, 0)};
    for (uint i = 0; i < 2; ++i) {
        auto& gaussian         = asset->gaussians[i];
        gaussian.position      = float4(0, 0, i == 0 ? -2.f : -4.f, 1);
        gaussian.scale_opacity = float4(.35f, .35f, .35f, .5f);
        gaussian.sh[0]         = float4(
            (rgb[i].x - .5f) / .2820947918f,
            (rgb[i].y - .5f) / .2820947918f,
            (rgb[i].z - .5f) / .2820947918f,
            0
        );
    }
    GaussianGpuScene scene;
    auto             snapshot = std::make_shared<GaussianSceneSnapshot>();
    snapshot->revision        = 1;
    GaussianInstanceSnapshot instance;
    instance.asset = asset;
    instance.settings.receive_shadows = shadow_strength >= 0;
    snapshot->instances.push_back(instance);
    auto&      device = RenderDevice::Get();
    const auto usage =
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC;
    auto depth = device.CreateTexture("Hybrid test depth", Extent3D(width, height, 1), PF_R32_SFLOAT, usage);
    auto mesh  = device.CreateTexture(
        "Hybrid test mesh color", Extent3D(width, height, 1), PF_R32G32B32A32_SFLOAT, usage
    );
    std::vector<float> depths(width * height), mesh_pixels(width * height * 4);
    const auto         projection = camera.GetProjectionMatrix();
    for (uint y = 0; y < height; ++y)
        for (uint x = 0; x < width; ++x) {
            float z = y < height / 2 ? (x < width / 2 ? 1.f : 3.f) : (x < width / 2 ? 5.f : 0.f);
            if (y == height / 2)
                z = 2.f;
            depths[y * width + x]                = z > 0 ? (-z * projection[2][2] + projection[2][3]) /
                                                               (-z * projection[3][2] + projection[3][3]) :
                                                           0.f;
            mesh_pixels[(y * width + x) * 4 + 2] = 1;
            mesh_pixels[(y * width + x) * 4 + 3] = 1;
        }
    auto                          bindless = device.CreateBindlessArray();
    Raster::GaussianSplatPass     splats(manager, {.tile_pair_capacity = 1024});
    Raster::GaussianCompositePass composite(manager, bindless);
    CommandList                   commands(EQueueType::Graphics);
    scene.ApplyUpdate(snapshot, commands);
    commands.CopyFrom(
        std::span<byte>(reinterpret_cast<byte*>(depths.data()), depths.size() * sizeof(float)),
        depth->GetView()
    );
    commands.CopyFrom(
        std::span<byte>(reinterpret_cast<byte*>(mesh_pixels.data()), mesh_pixels.size() * sizeof(float)),
        mesh->GetView()
    );
    GaussianViewSettings settings;
    settings.exposure_ev = 1;
    auto parameters      = splats.Prepare(&scene, camera, uint2(width, height), false, settings, depth, true);
    BufferRef  shadow_buffer;
    TextureRef shadow_texture;
    uint       shadow_handle = 0;
    if (shadow_strength >= 0) {
        GaussianShadowData shadow{};
        shadow.config                 = uint4(cube_shadow ? 2u : 1u, 1, 8, 0);
        shadow.world_to_view          = camera.GetViewMatrix();
        shadow.world_to_clip[0]       = float4x4::Identity();
        shadow.world_to_clip[0][2][2] = .1f;
        shadow.world_to_clip[0][2][3] = 1;
        shadow.splits                 = float4(1);
        shadow.clips                  = float4(.01f, 100.f, .01f, 100.f);
        shadow.light                  = cube_shadow ? float4(0, 0, 0, 1) : float4(0, 0, 1, 0);
        shadow.options                = float4(.4f, shadow_strength, 0, 0);
        const auto usage              = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST;
        shadow_texture =
            cube_shadow ?
                TextureRef(
                    device.CreateCubeMap("Mesh shadow cube reference", Extent2D(8, 8), PF_R32_SFLOAT, usage)
                ) :
                device.CreateTexture("Mesh shadow ortho reference", Extent3D(8, 8, 1), PF_R32_SFLOAT, usage);
        float d = cube_shadow ? .01f / (.01f - 100.f) - .01f * 100.f / ((.01f - 100.f) * 2.5f) : .7f;
        std::vector<float> shadow_pixels(8 * 8, d);
        for (uint face = 0; face < (cube_shadow ? 6u : 1u); ++face)
            commands.CopyFrom(
                std::span<byte>(
                    reinterpret_cast<byte*>(shadow_pixels.data()), shadow_pixels.size() * sizeof(float)
                ),
                cube_shadow ? shadow_texture->GetView().Slice(face, 1) : shadow_texture->GetView()
            );
        shadow_handle =
            bindless->AllocateTexture(shadow_texture->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
        shadow.mesh_maps.x = shadow_handle;
        shadow_buffer      = device.CreateBuffer<byte>(
            "Mesh shadow view reference",
            sizeof(shadow),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST
        );
        commands.CopyFrom(
            std::span<byte>(reinterpret_cast<byte*>(&shadow), sizeof(shadow)), shadow_buffer->GetView()
        );
        parameters.shadow_data                      = shadow_buffer;
        parameters.shadow_bindless                  = bindless;
        parameters.settings.shadow_receive_strength = shadow_strength;
        if (debug_visibility)
            parameters.settings.debug_view = EGaussianDebugView::ShadowVisibility;
    }
    auto merge           = composite.Prepare(
        TextureWithHandle{.tex = mesh},
        splats.GetColor(),
        splats.GetTransmittance(),
        BufferRef(splats.GetStatusBuffer().GetBuffer()),
        splats.GetRanges()
    );
    if (use_graph) {
        RenderGraph graph("HybridContract", RenderGraph::QueueTopology::FromRHI());
        auto        resources      = splats.ImportGraphResources(graph);
        auto        depth_resource = graph.ImportTexture("mesh_depth", depth, {});
        auto        mesh_resource  = graph.ImportTexture("mesh_color", mesh, {});
        auto        output         = graph.ImportTexture("composite", merge.output.tex, {});
        splats.AddToGraph(graph, parameters, resources, depth_resource);
        composite.AddToGraph(
            graph,
            merge,
            mesh_resource,
            resources.color,
            resources.transmittance,
            resources.status,
            resources.ranges,
            output
        );
        graph.Export(output);
        if (!graph.Compile())
            throw std::runtime_error("Hybrid graph: " + graph.GetCompileError());
        Require(graph.RecordAndMergeFrontendCommands(commands, true), "Hybrid graph recording failed");
    } else {
        splats.Record(commands, parameters);
        composite.Record(commands, merge);
    }
    auto reference = commands.Readback(splats.GetDisplay()->GetView(), "Hybrid reference display");
    auto colors    = commands.Readback(splats.GetColor()->GetView(), "Hybrid Gaussian color");
    auto transmit  = commands.Readback(splats.GetTransmittance()->GetView(), "Hybrid transmittance");
    auto final     = commands.Readback(merge.output.tex->GetView(), "Hybrid scene color");
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
    auto color = colors.Get().CopyAs<float>(), t = transmit.Get().CopyAs<float>(),
         image            = final.Get().CopyAs<float>();
    auto reference_pixels = reference.Get().CopyAs<float>();
    Require(color && t && image && reference_pixels, "Hybrid readback failed");
    const auto decode = [](float c) {
        return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
    };
    for (uint y = 0; y < height; ++y)
        for (uint x = 0; x < width; ++x) {
            float limit = y < height / 2 ? (x < width / 2 ? 1.f : 3.f) : (x < width / 2 ? 5.f : 1000.f);
            if (y == height / 2)
                limit = 2.f;
            float  expected_t = 1;
            float3 expected(0), expected_reference(0);
            for (uint i = 0; i < 2; ++i) {
                const float z = i == 0 ? 2.f : 4.f;
                if (z > limit)
                    continue;
                float scale    = height * .5f * projection[1][1] * .35f / z;
                float variance = scale * scale + .3f;
                float dx = float(x) - (width * .5f - .5f), dy = float(y) - (height * .5f - .5f);
                float alpha = .5f * std::exp(-.5f * (dx * dx + dy * dy) / variance);
                if (alpha < 1.f / 255.f)
                    continue;
                for (uint c = 0; c < 3; ++c) {
                    expected[c] += (debug_visibility ?
                                        (i == 0 ? 1.f : 0.f) :
                                        decode(rgb[i][c]) * 2.f *
                                            (shadow_strength >= 0 && i == 1 ? 1 - shadow_strength : 1.f)) *
                                   alpha * expected_t;
                    expected_reference[c] += rgb[i][c] * alpha * expected_t;
                }
                expected_t *= 1 - alpha;
            }
            const uint pixel = y * width + x;
            Require(std::abs((*t)[pixel] - expected_t) < .00003f, "Hybrid depth/alpha mismatch");
            for (uint c = 0; c < 3; ++c) {
                Require(
                    std::abs((*reference_pixels)[pixel * 4 + c] - decode(expected_reference[c])) < .00003f,
                    "Reference export domain must remain independent of engine linear accumulation"
                );
                Require(
                    std::abs((*color)[pixel * 4 + c] - expected[c]) < .00003f,
                    "Hybrid linear SH accumulation mismatch"
                );
                Require(
                    std::abs((*image)[pixel * 4 + c] - (expected[c] + (c == 2 ? expected_t : 0))) < .00003f,
                    "Hybrid premultiplied scene composite mismatch"
                );
            }
        }
    std::cout << "hybrid depth/linear-color/composite passed (graph=" << use_graph << ")" << std::endl;
}

} // namespace
void RunGaussianShadows(ShaderManager& manager, bool cube, float opacity_multiplier = 1.f) {
    auto& device = RenderDevice::Get();
    auto bindless = device.CreateBindlessArray();
    CommandList commands(EQueueType::Graphics);
    Camera camera;
    camera.Initialize(float3(0),-90,0,60,1,.01f,100.f);
    auto         asset   = Synthetic(cube ? 7 : 1);
    const float3 axes[6] = {{3,0,0},{-3,0,0},{0,3,0},{0,-3,0},{0,0,3},{0,0,-3}};
    for (uint i=0;i<asset->gaussians.size();++i) {
        asset->gaussians[i].position =
            cube ? float4(i < 6 ? axes[i] : float3(3, 3, 0), 1) : float4(0, 0, -3, 1);
        asset->gaussians[i].scale_opacity = float4(.2f,.2f,.2f,.8f);
        if (cube && i == 6)
            asset->gaussians[i].scale_opacity = float4(.5f, .5f, .5f, .8f);
    }
    auto snapshot = std::make_shared<GaussianSceneSnapshot>(); snapshot->revision = 1;
    GaussianInstanceSnapshot instance;
    instance.asset                       = asset;
    instance.settings.cast_shadows       = true;
    instance.settings.opacity_multiplier = opacity_multiplier;
    snapshot->instances.push_back(instance);
    GaussianGpuScene scene; scene.ApplyUpdate(snapshot,commands);
    Raster::GaussianShadowPass shadows(manager,bindless,PF_D32_SFLOAT);
    Raster::GaussianShadowPass::RecordParameters parameters;
    parameters.bindless = bindless;
    parameters.data_buffer = shadows.GetDataBuffer();
    parameters.gaussians=scene.GetGaussianBuffer(); parameters.instances=scene.GetInstanceBuffer();
    parameters.draws.push_back(scene.GetInstances()[0]); parameters.instance_indices.push_back(0);
    parameters.data.config=uint4(cube?2u:1u,1,64,1);
    parameters.data.options=float4(.4f,.5f,.005f,0);
    if (cube) {
        parameters.maps[0]=device.CreateCubeMap("Shadow cube contract",Extent2D(64,64),PF_D32_SFLOAT,ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT|ETextureUsageFlags::SAMPLED|ETextureUsageFlags::TRANSFER_SRC);
        const float3 dirs[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        const float3 ups[6]={{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
        auto proj=MakePerspectiveMatrixRH(1.57079632679f,1.f,100.f,.01f);
        proj[1][1] *= -1.f;
        for (uint face=0;face<6;++face) parameters.data.world_to_clip[face]=proj*MakeLookatViewMatrixRH(float3(0),dirs[face],ups[face]);
    } else {
        parameters.maps[0]=device.CreateTexture("Shadow ortho contract",Extent3D(64,64,1),PF_D32_SFLOAT,ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT|ETextureUsageFlags::SAMPLED|ETextureUsageFlags::TRANSFER_SRC);
        parameters.data.world_to_clip[0]=float4x4::Identity();
        parameters.data.world_to_clip[0][2][2]=.1f; parameters.data.world_to_clip[0][2][3]=1.f;
    }
    uint map_handle=bindless->AllocateTexture(parameters.maps[0]->GetView(),Sampler(SF_NEAREST,SAM_CLAMP_TO_EDGE));
    parameters.data.gaussian_maps.x=map_handle;
    shadows.Record(commands,parameters);
    TextureRef legacy_cube;
    uint       legacy_handle = 0;
    if (cube) {
        RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        auto legacy = parameters;
        legacy_cube = device.CreateCubeMap(
            "Legacy Mesh cube contract",
            Extent2D(64, 64),
            PF_D32_SFLOAT,
            ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED
        );
        legacy.maps[0] = legacy_cube;
        for (uint face = 0; face < 6; ++face)
            for (uint column = 0; column < 4; ++column)
                legacy.data.world_to_clip[face][1][column] *= -1.f;
        shadows.Record(commands, legacy);
        legacy_handle =
            bindless->AllocateTexture(legacy_cube->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
    }
    auto debug=shadows.RecordDepthDebug(commands,parameters);
    auto readback = commands.Readback(debug->GetView(),"Gaussian shadow depth contract");
    RHIExecutor::Get().Submit(EQueueType::Graphics,commands.Submit(),ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    auto shadow_readback = readback.Get();
    std::cout << "shadow readback status=" << int(shadow_readback.status) << " bytes=" << shadow_readback.ByteSize() << " reason=" << shadow_readback.error_reason << std::endl;
    auto pixels = shadow_readback.CopyAs<float>();
    Require(pixels && pixels->size() == 64*64*(cube?6:4)*4, "Gaussian shadow depth readback size");
    const float expected=cube ? .01f/(.01f-100.f) - .01f*100.f/((.01f-100.f)*3.f) : .7f;
    for (uint face=0;face<(cube?6u:1u);++face) {
        uint columns=cube?3u:2u;
        uint offset=((face/columns*64+32)*columns*64+(face%columns*64+32))*4;
        float center=(*pixels)[offset];
        Require(
            std::abs(center - (opacity_multiplier < .5f ? 0.f : expected)) < 1e-5,
            "Gaussian shadow depth differs from reverse-Z reference"
        );
        Require((*pixels)[((face/columns*64)*columns*64+face%columns*64)*4]==0, "Gaussian shadow map clear or ellipse cutoff incorrect");
    }
    if (!cube)
        for (uint y = 0; y < 64; ++y)
            for (uint x = 0; x < 64; ++x) {
                const float dx = x + .5f - 32.f, dy = y + .5f - 32.f;
                const float alpha =
                    std::min(.99f, .8f * opacity_multiplier * std::exp(-.5f * (dx * dx + dy * dy) / 41.26f));
                if (std::abs(alpha - .4f) < 1e-5f)
                    continue;
                const float actual = (*pixels)[(y * 128 + x) * 4];
                Require(
                    std::abs(actual - (alpha >= .4f ? .7f : 0.f)) < 1e-5f,
                    "Gaussian shadow alpha cutoff differs from CPU reference"
                );
            }
    // Query the map just rasterized through both Mesh raw-buffer and GS typed-buffer paths.
    parameters.data.world_to_view   = camera.GetViewMatrix();
    parameters.data.clips           = float4(.01f, 100.f, .01f, 100.f);
    parameters.data.splits          = float4(1);
    parameters.data.light           = cube ? float4(0, 0, 0, 1) : float4(0, 0, 1, 0);
    parameters.data.options.z       = 0;
    parameters.data.mesh_maps.x     = cube ? legacy_handle : map_handle;
    parameters.data.light_indices.z = cube ? 1u : 0u;
    if (!cube)
        for (uint view = 1; view < 6; ++view) {
            parameters.data.world_to_clip[view]       = float4x4::Identity();
            parameters.data.world_to_clip[view][0][3] = float(view) * .3f;
            parameters.data.world_to_clip[view][1][2] = float(view) * .2f;
        }
    Array<float4> queries;
    for (uint face = 0; face < (cube ? 6u : 1u); ++face) {
        const auto direction = cube ? axes[face] / 3.f : float3(0, 0, -1);
        queries.push_back(float4(direction * 2.f, 1));
        queries.push_back(float4(direction * 4.f, 1));
    }
    if (cube)
        for (const auto direction : {float3(1, .99f, 0), float3(.99f, 1, 0)}) {
            queries.push_back(float4(direction * 2.f, 1));
            queries.push_back(float4(direction * 4.f, 1));
        }
    auto query_input = device.CreateBuffer<float4>(
        "Shadow query points",
        queries.size(),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST
    );
    auto query_output = device.CreateBuffer<float4>(
        "Shadow query results",
        queries.size() * 3,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC
    );
    commands.CopyFrom(
        std::span<const byte>(reinterpret_cast<const byte*>(&parameters.data), sizeof(parameters.data)),
        parameters.data_buffer->GetView()
    );
    commands.CopyFrom(
        std::span<const byte>(reinterpret_cast<const byte*>(queries.data()), queries.size() * sizeof(float4)),
        query_input->GetView()
    );
    const uint data_handle = bindless->AllocateBuffer(parameters.data_buffer->GetView());
    commands.UpdateBindlessArray(bindless);
    auto query_pipeline = manager.Compute<GaussianShadowQueryPipeline>("test/GaussianShadowQuery.comp.hlsl");
    commands
        .Compute(
            query_pipeline,
            uint4(data_handle, uint(queries.size()), 0, 0),
            parameters.data_buffer->GetView(),
            query_output->GetView(),
            query_input->GetView(),
            bindless
        )
        .Dispatch(uint3(uint(queries.size()), 1, 1), "Gaussian shadow shared query contract");
    auto query_readback = commands.Readback(query_output->GetView(), "Gaussian shadow shared query");
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    auto query_pixels = query_readback.Get().CopyAs<float>();
    Require(query_pixels && query_pixels->size() == queries.size() * 12, "Shadow query readback failed");
    for (uint i = 0; i < queries.size(); ++i) {
        const auto expected_clip = parameters.data.world_to_clip[i % 6] * queries[i];
        const auto expected_view = parameters.data.world_to_view * queries[i];
        for (uint channel = 0; channel < 4; ++channel) {
            Require(
                std::abs((*query_pixels)[i * 12 + channel] - expected_clip[channel]) < 1e-5f,
                "Raw shadow matrix differs from CPU layout"
            );
            Require(
                std::abs((*query_pixels)[i * 12 + 4 + channel] - expected_view[channel]) < 1e-5f,
                "Raw view matrix differs from CPU layout"
            );
        }
        if (cube && i >= 12) {
            const auto raw   = (*query_pixels)[i * 12 + 8];
            const auto typed = (*query_pixels)[i * 12 + 9];
            std::cout << "Legacy seam query " << i << " raw=" << raw
                      << " adapted=" << (*query_pixels)[i * 12 + 10] << std::endl;
            Require(
                std::abs(raw - (*query_pixels)[i * 12 + 10]) < .112f,
                "Legacy Mesh cube adapter differs at seam"
            );
            std::cout << "Cube seam query " << i << " visibility=" << raw << "," << typed << std::endl;
            Require(std::abs(raw - typed) < 1e-5f, "Cube seam differs between raw and typed queries");
            Require(
                i % 2 == 0 ? std::abs(raw - 1.f) < 1e-5f : raw < .5f, "Cube seam loses the Gaussian occluder"
            );
            if (i == 15)
                Require(
                    std::abs(raw - (*query_pixels)[13 * 12 + 8]) < .112f, "Cube seam PCF is discontinuous"
                );
            continue;
        }
        const float expected_visibility = (i % 2 == 0 || opacity_multiplier < .5f) ? 1.f : 0.f;
        Require(
            std::abs((*query_pixels)[i * 12 + 8] - expected_visibility) < 1e-5f,
            "Mesh raw-buffer shadow query failed"
        );
        Require(
            std::abs((*query_pixels)[i * 12 + 9] - expected_visibility) < 1e-5f,
            "GS typed-buffer shadow query failed"
        );
    }
    if (cube)
        bindless->UnbindTexture(legacy_handle);
    bindless->UnbindBuffer(data_handle);
    bindless->UnbindTexture(map_handle);
    std::cout << "Gaussian shadow depth " << (cube ? "six cube faces" : "orthographic") << " passed\n";
}

int main(int argc, char** argv) {
    bool has_device = false, has_tasks = false, has_shaders = false;
    try {
        LogSystem::Init();
        ConfigManager::GetInstance().Init(std::filesystem::current_path());
        TaskSystem::Init();
        has_tasks = true;
        RenderDevice::Init(
            DeviceInitInfo{
                .rhi_type        = ERHIType::Vulkan,
                .name            = "GaussianRasterGpuTest",
                .rhi_api_version = "1.3",
                .rhi_thread      = false,
                .rhi_bypass      = true
            }
        );
        has_device    = true;
        auto& manager = ShaderManager::Get();
        has_shaders   = true;
        Run(manager, Synthetic(1), 1);
        Run(manager, Synthetic(2), 2);
        Run(manager, Synthetic(600), 600);
        Run(manager, Synthetic(32), 32);
        // Partial edge tiles must keep out-of-bounds/finished lanes in batch barriers.
        // A long, broad stack makes some pixels finish in the first batch while others
        // still need later batches, including tiles with only one in-bounds pixel.
        auto edge_asset = Synthetic(513);
        for (auto& gaussian : edge_asset->gaussians)
            gaussian.scale_opacity = float4(1.2f, 1.2f, 1.2f, .1f);
        Run(manager, edge_asset, 513, {}, false, false, uint2(65, 49));
        Run(manager, Synthetic(2), 2, {}, false, false, uint2(1, 1));
        auto rotated_asset = Synthetic(1);
        rotated_asset->gaussians[0].rotation = float4(0, 0, std::sin(.35f), std::cos(.35f));
        for (float opacity : {.75f, .0039217f}) {
            rotated_asset->gaussians[0].scale_opacity = float4(1.2f, .0001f, .01f, opacity);
            Run(manager, rotated_asset, 1, {}, false, false, uint2(65, 49), 20);
        }
        RunGaussianShadows(manager, false);
        RunGaussianShadows(manager, true);
        RunGaussianShadows(manager, false, .2f);
        RunGaussianShadows(manager, false, 2.f);
        RunViewportPng();
        RunHybrid(manager, false);
        RunHybrid(manager, true);
        for (float strength : {0.f, .5f, 1.f}) {
            RunHybrid(manager, false, strength, false);
            RunHybrid(manager, true, strength, false);
            RunHybrid(manager, false, strength, true);
            RunHybrid(manager, true, strength, true);
        }
        RunHybrid(manager, false, 0.f, false, true);
        RunHybrid(manager, true, .5f, false, true);
        RunHybrid(manager, false, 1.f, true, true);
        RunHybrid(manager, true, .5f, true, true);
        RunInFlightResize(manager);
        RunViewSettings(manager);
        Run(manager, Synthetic(2), 2, {}, true);
        Run(manager, Synthetic(600), 600, {}, false, true);
        if (argc > 2) {
            auto loaded = LoadGaussianPly(argv[1]);
            Require(bool(loaded), loaded.error_message.c_str());
            Run(manager, loaded.asset, 0, argv[2]);
        }
        ShaderManager::ShutDown();
        has_shaders = false;
        RenderDevice::Dispose();
        has_device = false;
        TaskSystem::ShutDown();
        has_tasks = false;
        std::cout << "Gaussian raster GPU contracts passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        if (has_shaders)
            ShaderManager::ShutDown();
        if (has_device)
            RenderDevice::Dispose();
        if (has_tasks)
            TaskSystem::ShutDown();
        return 1;
    }
}
