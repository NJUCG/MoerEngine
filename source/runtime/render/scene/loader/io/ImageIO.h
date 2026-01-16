#pragma once
#include "PixelFormat.h"
#include "rhi/RHICommon.h"

#include <filesystem>
#include <functional>
namespace Moer {

struct ImageReadDesc {
    uint32                     width{0}, height{0}, layers{1}, mips{1}, channel{4}, data_size{0};
    EPixelFormat               format{PF_UNDEFINED};
    void*                      data{nullptr};
    std::function<void(void*)> data_callback = [](void* _ptr) {
        delete[] static_cast<uint8*>(_ptr);
    };
    // size = face * layer * mips
    uint8 faces = 1;
    bool  IsValid();
};

class ImageIO {
public:
    static ImageReadDesc ReadFromFile(
        const std::filesystem::path& _path,
        uint32                       _desired_channal     = 4,
        EPixelFormat                 _fmt                 = PF_R8G8B8A8_UNORM,
        bool                         _is_generate_mipmaps = true
    );
    static ImageReadDesc ReadFromMemory(
        const unsigned char* _memory_data,
        size_t               _len,
        uint32_t             _desired_channal     = 4,
        bool                 _is_generate_mipmaps = true
    );
};
} // namespace Moer
