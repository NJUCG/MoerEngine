#pragma once
#include "RenderAPI.h"
#include <filesystem>

namespace Moer { namespace MiXml {
struct Object;

class RENDER_API Parser {
public:
    Parser() noexcept;
    ~Parser() noexcept;

    /**
         * @param file_path the absolute file path of mitsuba-style scene file
         * @return the root xml obj of the target scene(tree struction)
        */
    Object* LoadFromFile(std::filesystem::path file_path) noexcept;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
}} // namespace Moer::MiXml