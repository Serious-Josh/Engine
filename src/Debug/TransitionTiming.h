#pragma once

#include "Core/Log.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// ER.1 diagnostics: nested CPU wall-clock scopes around rendering-feature transitions. Off unless the process starts
// with ENGINE_ER1_TIMING set; disabled, a scope is one cached flag test. Enabled, each scope at or above the threshold
// logs "[ER1] <thread> <depth> <name> <ms>" when it ends, so inner scopes print before the scope that contains them.
//
// M0c.5 Stage 5.0: the same scopes serve startup measurement on a second channel. Off unless the process starts with
// ENGINE_STARTUP_TIMING set. A scope in the startup channel logs
// "[STARTUP] <thread> <depth> <name> <ms> ms [<start> s .. <end> s]", where the times are seconds since the process was
// created, so phases can be placed on one timeline and the time no scope covers can be computed. Milestones mark points
// (first Project Browser frame, first lit frame, …). Scopes default to the ER.1 channel only, so ER.1 output is unchanged.
namespace TransitionTiming
{
    enum Channel : unsigned int
    {
        Transitions = 1u,
        Startup = 2u,
        TransitionsAndStartup = Transitions | Startup,
    };

    inline bool Enabled()
    {
        static const bool enabled = GetEnvironmentVariableA("ENGINE_ER1_TIMING", nullptr, 0) != 0;
        return enabled;
    }

    inline bool StartupEnabled()
    {
        static const bool enabled = GetEnvironmentVariableA("ENGINE_STARTUP_TIMING", nullptr, 0) != 0;
        return enabled;
    }

    inline int& Depth()
    {
        thread_local int depth = 0;
        return depth;
    }

    // The process creation time on the steady clock, computed once from the system clock's distance to it.
    inline std::chrono::steady_clock::time_point ProcessStart()
    {
        static const std::chrono::steady_clock::time_point start = []()
        {
            FILETIME creation{};
            FILETIME exitTime{};
            FILETIME kernel{};
            FILETIME user{};
            FILETIME now{};
            GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
            GetSystemTimePreciseAsFileTime(&now);
            const auto ticks = [](FILETIME time)
            {
                return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            };
            const std::uint64_t sinceCreation = ticks(now) - ticks(creation);
            return std::chrono::steady_clock::now() - std::chrono::microseconds(sinceCreation / 10u);
        }();
        return start;
    }

    inline double SecondsSinceProcessStart(std::chrono::steady_clock::time_point time)
    {
        return std::chrono::duration<double>(time - ProcessStart()).count();
    }

    // A point on the startup timeline (startup channel only).
    inline void StartupMilestone(std::string_view name)
    {
        if(!StartupEnabled())
        {
            return;
        }

        Log::Info("[STARTUP] t" + std::to_string(GetCurrentThreadId()) + " milestone " + std::string(name) + " at " +
            std::to_string(SecondsSinceProcessStart(std::chrono::steady_clock::now())) + " s");
    }

    class Scope
    {
        public:
        // Scopes shorter than minimumMilliseconds are not logged (per-frame scopes would otherwise flood the log).
        explicit Scope(std::string_view name, double minimumMilliseconds = 0.1, unsigned int channels = Transitions)
        {
            m_startup = (channels & Startup) != 0u && StartupEnabled();
            const bool transitions = (channels & Transitions) != 0u && Enabled();

            if(!m_startup && !transitions)
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

            const auto end = std::chrono::steady_clock::now();
            const double milliseconds = std::chrono::duration<double, std::milli>(end - m_start).count();
            --Depth();

            if(milliseconds < m_minimum)
            {
                return;
            }

            const std::string thread = "t" + std::to_string(GetCurrentThreadId()) + " d" + std::to_string(m_depth) + " ";

            if(m_startup)
            {
                Log::Info("[STARTUP] " + thread + m_name + " " + std::to_string(milliseconds) + " ms [" +
                    std::to_string(SecondsSinceProcessStart(m_start)) + " s .. " + std::to_string(SecondsSinceProcessStart(end)) +
                    " s]");
            }
            else
            {
                Log::Info("[ER1] " + thread + m_name + " " + std::to_string(milliseconds) + " ms");
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        // M0c.5 Stage 5.4: appended to the logged name, for a result known only before the scope ends (a cache hit).
        void AppendName(std::string_view suffix)
        {
            if(m_active)
            {
                m_name += suffix;
            }
        }

        private:
        bool m_active = false;
        bool m_startup = false;
        int m_depth = 0;
        double m_minimum = 0.0;
        std::string m_name;
        std::chrono::steady_clock::time_point m_start{};
    };
}
