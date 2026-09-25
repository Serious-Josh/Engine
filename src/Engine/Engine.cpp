#include "Engine.h"
#include "Core/InputCodes.h"

#include <iostream>

void Engine::Initialize(){
    if(!m_window.Initialize()){
        std::cerr << "Failed to initialize window.\n";
    }

    m_time.Initialize();

    m_window.SetEventCallback(
        [this](const Event& event)
        {
            switch(event.type){
                case EventType::WindowClose:
                    m_window.Close();
                    break;
                case EventType::KeyPressed:
                    m_input.SetKeyState(event.keyCode, true);
                    break;
                case EventType::KeyReleased:
                    m_input.SetKeyState(event.keyCode, false);
                    break;
            }
        }
    );
}

void Engine::Run(){
    while(!m_window.ShouldClose()){
        Frame();
    }
}

void Engine::Frame(){
    m_time.Update();

    m_window.ProcessMessages();
}

void Engine::Shutdown(){
    m_window.Shutdown();
}

Input& Engine::GetInput(){
    return m_input;
}