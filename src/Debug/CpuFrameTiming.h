#pragma once

#include <chrono>
#include <cstdint>

namespace CpuTiming
{
    using Clock = std::chrono::steady_clock;

    inline double MillisecondsSince(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    struct FrameSample
    {
        uint64_t frameSerial = 0;
        bool valid = false;
        bool submitted = false;
        bool presentationReturned = false;
        Clock::time_point frameStart{};
        Clock::time_point executeStart{};
        Clock::time_point presentReturn{};
        bool frameIntervalValid = false;
        bool executeIntervalValid = false;
        bool fenceWaited = false;
        double frameInterval = 0.0;
        double frameExecution = 0.0;
        double render = 0.0;
        double begin = 0.0;
        double fenceWait = 0.0;
        double commandReset = 0.0;
        double recording = 0.0;
        double end = 0.0;
        double execute = 0.0;
        double executeInterval = 0.0;
        double present = 0.0;
    };

    // Includes all function exits and local cleanup in the enclosing duration.
    class ScopedDuration
    {
    public:
        explicit ScopedDuration(double& milliseconds)
            : m_milliseconds(milliseconds), m_start(Clock::now())
        {
        }

        ~ScopedDuration()
        {
            m_milliseconds = MillisecondsSince(m_start);
        }

    private:
        double& m_milliseconds;
        Clock::time_point m_start;
    };
}
