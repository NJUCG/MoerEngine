#ifndef CONFIG_MAP_H
#define CONFIG_MAP_H
#include <map>
#include <string>
namespace Moer {
    struct ConfigValue {
    };
    class ConfigMap : public std::map<std::string, ConfigValue> {
    };
}// namespace Moer
#endif// !CONFIG_MAP_H
