#pragma once

#include <chrono>
#include <cstdint>

// M0c.2 / M0c.3: residency of one rendering mode's exclusive GPU resources (Legacy lighting; Path Tracing). Renderer owns
// the shared policy: a recording that renders the mode marks it used, its confirmed submission records the last-use fence,
// and a recording of another mode may retire it after the grace period (or under DXGI memory pressure) once that fence
// has completed. The next recording of the mode recreates the resources before anything reads them.
struct ModeResourceResidency
{
    // False after retirement: the mode's exclusive resources and their descriptors do not exist until it renders again.
    bool resident = true;
    // Set while recording a frame that uses the resources; resolved when that frame's submission is confirmed.
    bool usedThisRecording = false;
    // A recording that used the resources was not confirmed submitted: they are never retired afterwards, since the last
    // fence that may reference them is unknown.
    bool submissionUncertain = false;
    // The submission fence of the last frame that used the resources.
    std::uint64_t lastUseFence = 0;
    // When a recording last used the resources (the grace period runs from here).
    std::chrono::steady_clock::time_point lastActive{};
    std::uint64_t retirements = 0;
    // Retirements before the grace period ended, because DXGI memory was under pressure.
    std::uint64_t pressureRetirements = 0;
    // Frames on which retirement was due but the last use had not completed on the GPU yet.
    std::uint64_t deferrals = 0;
    std::uint64_t recreations = 0;
    // CPU time of the most recent recreation (the resize that brought the resources back), in milliseconds.
    double lastRecreationMilliseconds = 0.0;
};
