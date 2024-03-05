#pragma once
#include "PixelFormat.h"

#include <filesystem>
#include <functional>
namespace Moer {

    struct ImageReadDesc {
        uint32_t                   width{0}, height{0}, layers{1}, mips{1}, channal{4}, data_size{0};
        EPixelFormat               format{PF_UNDEFINED};
        void*                      data{nullptr};
        std::function<void(void*)> data_callback{free};
        //size = layer * mips
        std::vector<uint32_t> offsets = {0};
        void CheckValid();
    };

    class ImageIO {
    public:
        static ImageReadDesc ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal = 4);
        static ImageReadDesc ReadFromMemory(const unsigned char* memory_data, size_t len, uint32_t desired_channal = 4);
    };
}// namespace Moer
