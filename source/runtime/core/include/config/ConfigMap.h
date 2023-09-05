#ifndef CONFIG_MAP_H
#define CONFIG_MAP_H
#include <map>
#include <string>
namespace __ENGINE_NAME__ {
    struct ConfigValue {

    };
    class ConfigMap : public std::map<std::string, ConfigValue>{

    };
}
#endif// !CONFIG_MAP_H
