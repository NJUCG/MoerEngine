#ifndef MOER_ENGINE_SHADER_COMPILE_CACHE_H
#define MOER_ENGINE_SHADER_COMPILE_CACHE_H

#include "misc/Types.h"
#include <string>
namespace Moer {
struct ShaderCompiledEntry {
    std::string shader_path;

    uint64 last_write_time;
};
} // namespace Moer

#endif //MOER_ENGINE_SHADER_COMPILE_CACHE_H