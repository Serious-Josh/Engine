#include "Engine.h"

#include <iostream>

void Engine::Initialize(){
    if(!m_window.Initialize()){
        std::cerr << "Failed to initialize window.\n";
    }
}

void Engine::Run(){
    while(!m_window.ShouldClose()){
        m_window.ProcessMessages();
    }
}

void Engine::Shutdown(){
    m_window.Shutdown();
}