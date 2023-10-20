#pragma once

#include <string>
#include <memory>
#include <filesystem>

namespace Moer::Resource {
namespace MiXml {
    struct Object;

    class Parser {
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
}
}// namespace Moer::Resource::MiXml