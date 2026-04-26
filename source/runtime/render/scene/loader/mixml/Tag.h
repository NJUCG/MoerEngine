#pragma once

#include "misc/STL.h"

#include <string>

namespace Moer::MiXml {
enum class ETag : unsigned int {
    UNKNOWN = 0,
    // xml objects
    SCENE,
    DEFAULT,
    BSDF,
    EMITTER,
    FILM,
    INTEGRATOR,
    SENSOR,
    SHAPE,
    TEXTURE,
    LOOKAT,
    TRANSFORM,
    // properties
    INTEGER,
    STRING,
    FLOAT,
    VECTOR,
    RGB,
    POINT,
    MATRIX,
    SCALE,
    ROTATE,
    TRANSLATE,
    BOOLEAN,
    // reference
    REF,
    NUM_COUNT
};

extern const Moer::UnorderedMap<std::string, ETag> S_TAG_MAP;
} // namespace Moer::MiXml