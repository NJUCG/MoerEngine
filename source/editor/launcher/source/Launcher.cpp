#include "Launcher.h"
#include <filesystem>
#include "Engine.h"

Launcher& Launcher::GetInstance(){
    static Launcher launcher;
    return launcher;
}

Launcher::Launcher(){}

void Launcher::Init(const std::filesystem::path& _work_space_path){
    EngineInitInfo info{_work_space_path};
    engine = new Engine();
    engine->Init(info);

    engine->PostInit();
}

void Launcher::Run(){

    engine->Run();
    
    engine->Quit();
}

void Launcher::Quit(){
    
}