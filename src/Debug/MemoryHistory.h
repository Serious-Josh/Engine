#pragma once

#include "Debug/MemoryMonitor.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// M0b.1 P3: the Memory window's model, free of ImGui so it can be tested directly. Only the Editor's
// MemoryProfilerWindow.cpp instantiates it; packaged code never references this header.
//
// Fed with MemoryMonitor snapshots (each of which already carries the P1 registry snapshot taken at the same moment).
// A snapshot is consumed once, by sequence number: it appends one history sample (unless history is paused) and updates
// the observed peaks (always). Storage is fixed; nothing allocates per sample.

// The plotted measurements of one monitor sample, with their validity.
struct MemoryHistorySample
{
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point time{};
    bool processValid = false;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t privateBytes = 0;
    bool localValid = false;
    std::uint64_t localUsageBytes = 0;
    std::uint64_t localBudgetBytes = 0;
    bool nonLocalValid = false;
    std::uint64_t nonLocalUsageBytes = 0;
    std::uint64_t nonLocalBudgetBytes = 0;
    bool trackedValid = false;
    std::uint64_t trackedBytes = 0;
};

// The largest value seen since the last reset; invalid until a valid measurement has been observed.
struct ObservedPeak
{
    bool valid = false;
    std::uint64_t bytes = 0;

    void Observe(bool measurementValid, std::uint64_t measurement)
    {
        if(!measurementValid)
        {
            return;
        }

        if(!valid || measurement > bytes)
        {
            bytes = measurement;
        }

        valid = true;
    }

    void Restart(bool measurementValid, std::uint64_t measurement)
    {
        valid = measurementValid;
        bytes = measurementValid ? measurement : 0;
    }
};

// Observed peaks since the last reset. These are separate from P1's lifetime peaks and the OS's process peaks, which
// cannot be reset; resetting here changes nothing outside this model.
struct MemoryObservedPeaks
{
    ObservedPeak workingSet;
    ObservedPeak privateBytes;
    ObservedPeak localUsage;
    ObservedPeak nonLocalUsage;
    ObservedPeak tracked;
    std::array<ObservedPeak, MemoryCategoryCount> categories{};
};

class MemoryProfilerModel
{
    public:
    static constexpr std::size_t HistoryCapacity = 240;
    // Samples further apart than this are not joined by a line (minimized Editor, stalls, paused history).
    static constexpr std::chrono::milliseconds GapThreshold{1500};
    // The latest sample is stale when older than this.
    static constexpr std::chrono::milliseconds StaleAge{1500};

    // Consumes a snapshot once. Returns true when it was new (whether or not history was paused).
    bool Observe(const MemoryMonitorSnapshot& snapshot)
    {
        // Monitor sequences only increase (they survive Stop/Start), so anything not newer was already consumed.
        if(!snapshot.available || snapshot.sequence <= m_lastSequence)
        {
            return false;
        }

        m_lastSequence = snapshot.sequence;
        m_latest = snapshot;
        m_hasLatest = true;
        const MemoryHistorySample sample = ToSample(snapshot);
        ObservePeaks(snapshot, sample, false);

        if(!m_paused)
        {
            Append(sample);
        }

        return true;
    }

    // Observed peaks restart from the latest measurements; lifetime and process peaks are untouched.
    void ResetObservedPeaks()
    {
        m_peaks = {};

        if(m_hasLatest)
        {
            ObservePeaks(m_latest, ToSample(m_latest), true);
        }
    }

    void ClearHistory()
    {
        m_head = 0;
        m_count = 0;
    }

    void SetPaused(bool paused)
    {
        m_paused = paused;
    }

    bool IsPaused() const
    {
        return m_paused;
    }

    std::size_t Count() const
    {
        return m_count;
    }

    // 0 is the oldest retained sample.
    const MemoryHistorySample& Sample(std::size_t index) const
    {
        return m_samples[(m_head + HistoryCapacity - m_count + index) % HistoryCapacity];
    }

    bool HasLatest() const
    {
        return m_hasLatest;
    }

    const MemoryMonitorSnapshot& Latest() const
    {
        return m_latest;
    }

    const MemoryObservedPeaks& ObservedPeaks() const
    {
        return m_peaks;
    }

    bool IsStale(std::chrono::steady_clock::time_point now) const
    {
        return !m_hasLatest || now - m_latest.sampleTime > StaleAge;
    }

    static MemoryHistorySample ToSample(const MemoryMonitorSnapshot& snapshot)
    {
        MemoryHistorySample sample;
        sample.sequence = snapshot.sequence;
        sample.time = snapshot.sampleTime;
        sample.processValid = snapshot.process.valid;
        sample.workingSetBytes = snapshot.process.workingSetBytes;
        sample.privateBytes = snapshot.process.privateBytes;
        sample.localValid = snapshot.video.adapterValid && snapshot.video.local.valid;
        sample.localUsageBytes = snapshot.video.local.usageBytes;
        sample.localBudgetBytes = snapshot.video.local.budgetBytes;
        sample.nonLocalValid = snapshot.video.adapterValid && snapshot.video.nonLocal.valid;
        sample.nonLocalUsageBytes = snapshot.video.nonLocal.usageBytes;
        sample.nonLocalBudgetBytes = snapshot.video.nonLocal.budgetBytes;
        sample.trackedValid = snapshot.gpuAllocations.available;
        sample.trackedBytes = snapshot.gpuAllocations.total.currentBytes;

        return sample;
    }

    private:
    void Append(const MemoryHistorySample& sample)
    {
        m_samples[m_head] = sample;
        m_head = (m_head + 1) % HistoryCapacity;

        if(m_count < HistoryCapacity)
        {
            ++m_count;
        }
    }

    void ObservePeaks(const MemoryMonitorSnapshot& snapshot, const MemoryHistorySample& sample, bool restart)
    {
        const auto apply = [restart](ObservedPeak& peak, bool valid, std::uint64_t bytes)
        {
            if(restart)
            {
                peak.Restart(valid, bytes);
            }
            else
            {
                peak.Observe(valid, bytes);
            }
        };

        apply(m_peaks.workingSet, sample.processValid, sample.workingSetBytes);
        apply(m_peaks.privateBytes, sample.processValid, sample.privateBytes);
        apply(m_peaks.localUsage, sample.localValid, sample.localUsageBytes);
        apply(m_peaks.nonLocalUsage, sample.nonLocalValid, sample.nonLocalUsageBytes);
        apply(m_peaks.tracked, sample.trackedValid, sample.trackedBytes);

        for(std::size_t index = 0; index < MemoryCategoryCount; ++index)
        {
            apply(m_peaks.categories[index], sample.trackedValid, snapshot.gpuAllocations.categories[index].currentBytes);
        }
    }

    std::array<MemoryHistorySample, HistoryCapacity> m_samples{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    bool m_paused = false;
    std::uint64_t m_lastSequence = 0;
    bool m_hasLatest = false;
    MemoryMonitorSnapshot m_latest;
    MemoryObservedPeaks m_peaks;
};
