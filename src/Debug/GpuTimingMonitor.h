#pragma once

#include "Renderer/GpuTimestampProfiler.h"

#include <algorithm>
#include <array>
#include <cmath>

// CPU-only diagnostics for completed timestamp results; never accesses GPU resources.
struct GpuTimingMonitor
{
    static constexpr unsigned int RegionCount = static_cast<unsigned int>(GpuTimestampProfiler::Region::Count);
    static constexpr unsigned int HistoryCapacity = 120;
    static constexpr unsigned int BaselineSampleCount = 30;
    static constexpr unsigned int MinimumBaselineSamples = 10;
    static constexpr unsigned int EventCapacity = 20;

    struct History
    {
        std::array<double, HistoryCapacity> samples{};
        unsigned int count = 0;
        unsigned int next = 0;
        bool currentValid = false;
        bool excursion = false;

        int excursionDirection = 0;

        double excursionBaseline = 0.0;
        double current = 0.0;
    };

    struct Event
    {
        unsigned int region = 0;
        double baseline = 0.0;
        double current = 0.0;
        double delta = 0.0;

        uint64_t frame = 0;
    };

    struct FrameSample
    {
        uint64_t frame = 0;

        uint32_t validTimings = 0;
        uint32_t validDeltas = 0;

        std::array<double, RegionCount> timings{};
        std::array<double, RegionCount> deltas{};
    };

    struct GraphHistory
    {
        std::array<FrameSample, HistoryCapacity> frames{};
        unsigned int count = 0;
        unsigned int next = 0;

        const FrameSample& GetSample(unsigned int chronologicalIndex) const
        {
            const unsigned int oldest = (next + HistoryCapacity - count) % HistoryCapacity;
            return frames[(oldest + chronologicalIndex) % HistoryCapacity];
        }
    };

    std::array<History, RegionCount> histories{};

    GraphHistory graphHistory;

    std::array<Event, EventCapacity> events{};
    unsigned int eventCount = 0;
    unsigned int nextEvent = 0;
    uint64_t lastFrame = 0;
    uint64_t sampledFrame = 0;

    bool hasFrame = false;
    bool enabled = true;
    bool frozen = false;

    float threshold = 0.20f;

    void Clear()
    {
        histories = {};
        graphHistory = {};
        events = {};
        eventCount = 0;

        nextEvent = 0;
        // Preserve the consumed frame marker so clear cannot replay a result.
    }

    void ResetExcursions()
    {
        for (History& history : histories)
        {
            history.excursion = false;
        }
    }

    static double CalculateBaseline(const History& history)
    {
        std::array<double, BaselineSampleCount> previousSamples{};

        const unsigned int sampleCount = (std::min)(history.count, BaselineSampleCount);

        // 'next' is the next write slot, so walk backward from the latest sample.

        for (unsigned int i = 0; i < sampleCount; ++i)
        {
            const unsigned int sampleIndex = (history.next + HistoryCapacity - 1u - i) % HistoryCapacity;
            previousSamples[i] = history.samples[sampleIndex];
        }

        std::sort(previousSamples.begin(), previousSamples.begin() + sampleCount);

        const unsigned int middle = sampleCount / 2u;

        if (sampleCount % 2u != 0u)
        {
            return previousSamples[middle];
        }

        return (previousSamples[middle - 1u] + previousSamples[middle]) * 0.5;
    }

    void DetectEvent(

        unsigned int region,
        double current,
        uint64_t frame)
    {
        History& history = histories[region];
        const double baseline = CalculateBaseline(history);
        const double delta = current - baseline;
        const double distanceFromEventBaseline = current - history.excursionBaseline;
        const double returnTolerance = threshold * 0.5;

        // Keep the baseline from the event until timing returns near it. This
        // prevents a sustained plateau from generating an event every frame.

        if (history.excursion && std::abs(distanceFromEventBaseline) <= returnTolerance)
        {
            history.excursion = false;
        }

        // Also allow a direct transition from a spike to a drop, or vice versa.
        const bool oppositeExcursion =
            history.excursion && distanceFromEventBaseline * history.excursionDirection <= -threshold;

        const bool canRecordEvent = !history.excursion || oppositeExcursion;
        const bool exceedsThreshold = std::abs(delta) >= static_cast<double>(threshold);

        if (canRecordEvent && exceedsThreshold)
        {
            events[nextEvent] = {region, baseline, current, delta, frame};

            nextEvent = (nextEvent + 1u) % EventCapacity;
            eventCount = (std::min)(eventCount + 1u, EventCapacity);

            history.excursion = true;
            history.excursionDirection = delta > 0.0 ? 1 : -1;
            history.excursionBaseline = baseline;
        }
    }

    void Update(const GpuTimestampProfiler::Results& result)
    {
        const bool alreadyConsumed = hasFrame && result.frameSerial <= lastFrame;

        if (result.validRegions == 0 || alreadyConsumed)
        {
            return;
        }

        hasFrame = true;

        lastFrame = result.frameSerial;

        // Freeze pauses monitor sampling only; the timestamp profiler keeps running.

        if (!enabled || frozen)
        {
            return;
        }

        sampledFrame = result.frameSerial;

        // All regions occupy the same frame slot, including unavailable regions.
        FrameSample& graphSample = graphHistory.frames[graphHistory.next];

        graphSample = {};
        graphSample.frame = result.frameSerial;

        for (unsigned int region = 0; region < RegionCount; ++region)
        {
            History& history = histories[region];

            const double current = result.milliseconds[region];
            const bool regionAvailable = (result.validRegions & (1u << region)) != 0;
            history.currentValid = regionAvailable && std::isfinite(current) && current > 0.0;

            if (!history.currentValid)
            {
                continue;
            }

            history.current = current;
            const uint32_t regionBit = 1u << region;

            graphSample.validTimings |= regionBit;
            graphSample.timings[region] = current;

            if (history.count > 0)
            {
                graphSample.validDeltas |= regionBit;
                graphSample.deltas[region] = current - CalculateBaseline(history);
            }

            // Detect before inserting the current sample: it must not affect its baseline.

            if (history.count >= MinimumBaselineSamples)
            {
                DetectEvent(region, current, result.frameSerial);
            }

            history.samples[history.next] = current;

            history.next = (history.next + 1u) % HistoryCapacity;
            history.count = (std::min)(history.count + 1u, HistoryCapacity);
        }

        graphHistory.next = (graphHistory.next + 1u) % HistoryCapacity;
        graphHistory.count = (std::min)(graphHistory.count + 1u, HistoryCapacity);
    }
};
