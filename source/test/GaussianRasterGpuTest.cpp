#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "renderer/raster/GaussianImageExport.h"
#include "renderer/raster/GaussianSplatPass.h"
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
    Raster::GaussianSplatPass pass(manager);
    Camera                    camera;
    camera.Initialize(float3(0), -90, 0, 60, 1, .01f, 100.f);
    std::vector<ReadbackFuture> results;
    for (uint frame_id = 0; frame_id < 8; ++frame_id) {
        pass.SetPairCapacity(frame_id % 2 ? 1024u : 4096u);
        CommandList commands(EQueueType::Graphics);
        pass.Record(commands, &scene, camera, uint2(64 + frame_id * 8, 64 + frame_id * 4), false);
        results.push_back(commands.Readback(pass.GetColor()->GetView(), "Gaussian in-flight resize"));
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

} // namespace
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
