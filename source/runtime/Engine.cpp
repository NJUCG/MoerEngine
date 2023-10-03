#include "Engine.h"
#include <spdlog/spdlog.h>

void Engine::Init(const EngineInitInfo& _info){
    SPDLOG_INFO("Engine Begin Initilization");


    SPDLOG_INFO("Engine Initilization Finished");
}

void Engine::PostInit(){
    SPDLOG_INFO("Engine Begin Post Init");

    SPDLOG_INFO("Engine Post Init Finished");
}

void Engine::Run(){
    SPDLOG_INFO("Engine Start Running");
    for (; ; ) {
        //todo: currently not functions yet
        // Tick();
        break;
    }
    SPDLOG_INFO("Engine Stop Running");
}

void Engine::Tick(){

}

void Engine::Quit(){
    SPDLOG_INFO("Engine Quit");
}