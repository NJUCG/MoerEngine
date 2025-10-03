#ifndef MOER_ENGINE_SHADER_COMPILE_CACHE_H
#define MOER_ENGINE_SHADER_COMPILE_CACHE_H
#include <string>
namespace Moer {
struct ShaderCompiledEntry {
    std::string shader_path;

    uint64_t last_write_time;
};
} // namespace Moer

#endif //MOER_ENGINE_SHADER_COMPILE_CACHE_H