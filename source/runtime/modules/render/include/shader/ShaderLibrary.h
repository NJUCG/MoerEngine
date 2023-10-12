#ifndef MOERENGINE_SHADER_LIBRARY_H
#define MOERENGINE_SHADER_LIBRARY_H
#include "shader/ShaderCommon.h"
#include <array>
class ShaderResourceMap {
};
class GlobalShaderMap {

private:
    std::array<ShaderResourceMap, EShaderPlatform::SP_Num> resources_maps;
};
#endif