#ifndef MOREENGINE_ENGINE_H
#define MOREENGINE_ENGINE_H

#include <filesystem>

struct EngineInitInfo{

    std::filesystem::path config_path;

};

class Engine final{
    public:
    void Init(const EngineInitInfo& _init_info);
    
    void PostInit();

    void Run();

    void Quit();

    private:
    void Tick();
};

#endif