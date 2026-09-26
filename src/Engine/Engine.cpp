#include "Engine.h"
#include "Core/InputCodes.h"
#include "Core/Log.h"

bool Engine::Initialize(){
    if(!m_window.Initialize()){
        Log::Error("Failed to initialize window.");
        return false;
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
                    m_input.SetKeyState(event.code, true);
                    break;
                case EventType::KeyReleased:
                    m_input.SetKeyState(event.code, false);
                    break;
                case EventType::MouseButtonPressed:
                    m_input.SetMouseButtonState(event.code, true);
                    break;
                case EventType::MouseButtonReleased:
                    m_input.SetMouseButtonState(event.code, false);
                    break;
                case EventType::MouseMoved:
                    m_input.SetMousePosition(event.mouseX, event.mouseY);
                    break;
                case EventType::MouseScrolled:
                    m_input.SetScroll(event.scrollX, event.scrollY);
                    break;
            }
        }
    );

    Log::Info("Engine initialized.");
    return true;
}

void Engine::Run(){
    while(!m_window.ShouldClose()){
        Frame();
    }
}

void Engine::Frame(){
    m_time.Update();

    m_input.Update();
    m_window.ProcessMessages();
}

void Engine::Shutdown(){
    m_window.Shutdown();

    Log::Info("Engine shutting down.");
}

Input& Engine::GetInput(){
    return m_input;
}