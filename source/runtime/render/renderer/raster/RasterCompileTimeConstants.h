#pragma once

/**
 * 这个文件专门存一些编译期的常量
 * 
 * 本来想写到config里的，但想了想，一些常量需要在编译期决定（比如用于设置Shader参数的一些值），所以不适合写在config里
 * 
 * 目前我不知道这些常量写在哪里比较合适，所以暂时写在这里。如果后续找到了更合适的地方，欢迎帮忙修改一下
 */

#include "misc/STL.h"
#include "misc/Traits.h"

namespace Moer {

// 如果要修改此参数，请同步修改 shaderheaders\shared\raster\lighting_pass\ShaderParameters.h 中的 LightingData 结构体
static constexpr uint CSM_MAX_CASCADES = 4;

}