#pragma once
#include "PixelFormat.h"

#include <filesystem>
#include <functional>
namespace Moer {

    struct ImageReadDesc {
        int                        width{}, height{}, depth{1}, channal{4};
        EPixelFormat               format;
        void*                      data;
        std::function<void(void*)> data_callback;
    };

    class ImageIO {
    public:
        static ImageReadDesc ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal = 4);
    };
}// namespace Moer
