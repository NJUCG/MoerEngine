#ifndef MOER_ENGINE_CONFIG_MANAGER_H
#define MOER_ENGINE_CONFIG_MANAGER_H
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

class ConfigManager {
public:
    ConfigManager(const std::filesystem::path& configPath) {
        LoadConfig(configPath);
    }

    std::string GetValue(const std::string& key) const {
        auto it = m_config_map.find(key);
        if (it != m_config_map.end()) {
            return it->second;
        }
        return "";
    }

private:
    void LoadConfig(const std::filesystem::path& configPath) {
        std::ifstream config_file(configPath);
        if (!config_file.is_open()) {
            throw std::runtime_error("Failed to open config file");
        }

        std::string line;
        while (std::getline(config_file, line)) {
            // Ignore comments and empty lines
            if (line.empty() || line[0] == '#') {
                continue;
            }

            // Split line into key and value
            size_t equals_pos = line.find('=');
            if (equals_pos == std::string::npos) {
                throw std::runtime_error("Invalid config line: " + line);
            }
            std::string key   = line.substr(0, equals_pos);
            std::string value = line.substr(equals_pos + 1);

            // Add key-value pair to map
            m_config_map[key] = value;
        }
    }

    std::unordered_map<std::string, std::string> m_config_map;
};
#endif//MOER_ENGINE_CONFIG_MANAGER_H