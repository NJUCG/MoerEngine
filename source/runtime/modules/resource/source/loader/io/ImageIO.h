#pragma once
#include "PixelFormat.h"
#include "rhi/RHICommon.h"

#include <filesystem>
#include <functional>
namespace Moer {

    struct ImageReadDesc {
        uint32_t                   width{0}, height{0}, layers{1}, mips{1}, channal{4}, data_size{0};
        EPixelFormat               format{PF_UNDEFINED};
        void*                      data{nullptr};
        std::function<void(void*)> data_callback{free};
        //size = layer * mips
        Array<uint32_t> mip_offsets = {0};
        Array<Extent3D> mip_extents;
        bool            IsValid();
    };

    class ImageIO {
    public:
        static ImageReadDesc ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal = 4, EPixelFormat _fmt = PF_R8G8B8A8_UNORM);
        static ImageReadDesc ReadFromMemory(const unsigned char* memory_data, size_t len, uint32_t desired_channal = 4);
    };
}// namespace Moer
