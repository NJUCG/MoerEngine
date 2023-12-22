#pragma once

#include <string>
#include <memory>
#include <filesystem>

#include <scene/Scene.h>

namespace Moer::Resource::Gltf {

        class  Parser {
        public:
            Parser() noexcept;
            ~Parser() noexcept;
            
            static CORE_API std::unique_ptr<Scene> LoadSceneFromFile(const std::filesystem::path &  file_path) noexcept;

        private:
            struct Impl;
            Impl* m_impl = nullptr;
        };
}