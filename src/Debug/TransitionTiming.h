#pragma once

#include "Core/Log.h"

#include <windows.h>

#include <chrono>
#include <string>
#include <string_view>

// ER.1 diagnostics: nested CPU wall-clock scopes around rendering-feature transitions. Off unless the process starts
// with ENGINE_ER1_TIMING set; disabled, a scope is one cached flag test. Enabled, each scope at or above the threshold
// logs "[ER1] <thread> <depth> <name> <ms>" when it ends, so inner scopes print before the scope that contains them.
namespace TransitionTiming
{
    inline bool Enabled()
    {
        static const bool enabled = GetEnvironmentVariableA("ENGINE_ER1_TIMING", nullptr, 0) != 0;
        return enabled;
    }

    inline int& Depth()
    {
        thread_local int depth = 0;
        return depth;
    }

    class Scope
    {
        public:
        // Scopes shorter than minimumMilliseconds are not logged (per-frame scopes would otherwise flood the log).
        explicit Scope(std::string_view name, double minimumMilliseconds = 0.1)
        {
            if(!Enabled())
            {
                return;
            }

            m_active = true;
            m_name = name;
            m_minimum = minimumMilliseconds;
            m_depth = Depth()++;
            m_start = std::chrono::steady_clock::now();
        }

        ~Scope()
        {
            if(!m_active)
            {
                return;
            }

            const double milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
            --Depth();

            if(milliseconds >= m_minimum)
            {
                Log::Info("[ER1] t" + std::to_string(GetCurrentThreadId()) + " d" + std::to_string(m_depth) + " " +
                    m_name + " " + std::to_string(milliseconds) + " ms");
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        private:
        bool m_active = false;
        int m_depth = 0;
        double m_minimum = 0.0;
        std::string m_name;
        std::chrono::steady_clock::time_point m_start{};
    };
}
