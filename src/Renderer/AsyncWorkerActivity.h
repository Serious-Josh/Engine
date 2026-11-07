#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// A coherent, read-only view of one owned DXR worker, taken under the worker's own mutex. The counters are monotonic
// for the worker object's lifetime, and every accepted job is in exactly one place:
//     submitted == queued + (active ? 1 : 0) + finished + dropped
//     finished  == untaken + taken + discarded
// `finished` counts completions, successful or failed. `taken` counts results the owner moved out, which is when the
// owner accepts or rejects them. `dropped` counts queued jobs removed at shutdown, `discarded` untaken results.
// Rejected submissions (duplicates, a stopped worker) are not counted anywhere.
struct AsyncWorkerActivity
{
    std::uint64_t submitted = 0;
    std::uint64_t finished = 0;
    std::uint64_t taken = 0;
    std::uint64_t dropped = 0;
    std::uint64_t discarded = 0;
    std::size_t queued = 0;
    std::size_t untaken = 0;
    bool active = false;
    // The key of the active job; empty when idle. Assigned into the caller's string, so polling reuses its capacity.
    std::string activeJob;

    bool IsBusy() const noexcept
    {
        return queued > 0 || active;
    }
};

// What the Renderer's DXR preparation gates concluded during one Render call: whether any of them is still waiting for
// background work, and the first required failure each phase reported. Optional variants that fall back are Ready by
// then, and a stale result is resubmitted (waiting) rather than reported, so neither appears as a failure.
struct DxrPreparationFrameReport
{
    bool waiting = false;
    // First diagnostic line only; the full text is logged where it always was.
    std::string libraryFailure;
    std::string pipelineFailure;
};
