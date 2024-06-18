#include <catch2/catch_test_macros.hpp>

#include <span>

#define DDS_USE_STD_FILESYSTEM 1
#include <filesystem>
#include <dds.hpp>

TEST_CASE("Read DDS and write PPM") {
    dds::Image image;
    auto result = dds::readFile(std::filesystem::current_path() / "test.dds", &image);
    REQUIRE(result == dds::ReadResult::Success);
    REQUIRE(image.width > 0);
    REQUIRE(image.height > 0);
    REQUIRE(image.format == DXGI_FORMAT_R8G8B8A8_UNORM);

    auto& firstMipmap = image.mipmaps.front();

    std::ofstream output(std::filesystem::current_path() / "test.ppm", std::ios_base::out | std::ios_base::binary);
    output << "P6"
           << "\n"
           << image.width << " " << image.height << "\n"
           << "255"
           << "\n";

    output << std::setfill('0') << std::setw(3);
    for (auto j = 0u; j < image.height; ++j) {
        for (auto i = 0u; i < image.width; ++i) {
            auto pixel = ((j * image.width) + i) * 4; // 4 channels
            output << firstMipmap.data()[pixel] << firstMipmap.data()[pixel + 1] << firstMipmap.data()[pixel + 2];
        }
    }

    output.close();
}
