#include "Engine.h"

#include <iostream>

void Engine::Initialize(){
    std::cout << "Engine initialized.\n";
}

void Engine::Run(){
    std::cout << "Engine running.\n";
}

void Engine::Shutdown(){
    std::cout << "Engine shutting down.\n";
}