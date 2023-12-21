#ifndef CONFIG_MAP_H
#define CONFIG_MAP_H

#include "misc/STL.h"

namespace Moer {
    struct ConfigValue {
    };
    class ConfigMap : public Moer::Map<std::string, ConfigValue> {
    };
}// namespace Moer
#endif// !CONFIG_MAP_H
