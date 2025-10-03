#include "loader\mixml\Tag.h"

namespace Moer::Resource::MiXml {
    const Moer::UnorderedMap<std::string, ETag> S_TAG_MAP{
        {"", ETag::UNKNOWN},
        {"scene", ETag::SCENE},
        {"default", ETag::DEFAULT},
        {"bsdf", ETag::BSDF},
        {"emitter", ETag::EMITTER},
        {"film", ETag::FILM},
        {"integrator", ETag::INTEGRATOR},
        {"sensor", ETag::SENSOR},
        {"shape", ETag::SHAPE},
        {"texture", ETag::TEXTURE},
        {"lookat", ETag::LOOKAT},
        {"transform", ETag::TRANSFORM},
        {"integer", ETag::INTEGER},
        {"string", ETag::STRING},
        {"float", ETag::FLOAT},
        {"vector", ETag::VECTOR},
        {"rgb", ETag::RGB},
        {"point", ETag::POINT},
        {"matrix", ETag::MATRIX},
        {"scale", ETag::SCALE},
        {"rotate", ETag::ROTATE},
        {"translate", ETag::TRANSLATE},
        {"boolean", ETag::BOOLEAN},
        {"ref", ETag::REF},
    };
}