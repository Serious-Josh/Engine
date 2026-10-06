#pragma once

#include <chrono>

class Time{
    public:
        void Initialize();
        void Update();

        double GetDeltaTime() const;

    private:
        using Clock = std::chrono::steady_clock;

        Clock::time_point m_previousTime;
        double m_deltaTime = 0.0;
};