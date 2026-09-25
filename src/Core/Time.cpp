#include "Time.h"

void Time::Initialize(){
    m_previousTime = Clock::now();
}

void Time::Update(){
    const Clock::time_point currentTime = Clock::now();

    m_deltaTime = std::chrono::duration<double>(currentTime - m_previousTime).count();

    m_previousTime = currentTime;
    m_elapsedTime += m_deltaTime;
}

double Time::GetDeltaTime() const{
    return m_deltaTime;
}

double Time::GetElapsedTime() const{
    return m_elapsedTime;
}