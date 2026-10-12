#pragma once

#include "CpuFrameTiming.h"
#include "Renderer/GpuTimestampProfiler.h"
#include <array>

// CPU anchors use steady_clock only. GPU timestamps are never compared to CPU clocks.
class FramePacingMonitor
{
    public:
    enum class RecordState
    {
        Pending,
        Available,
        Missing
    };

    struct CpuRecord
    {
        RecordState state = RecordState::Pending;

        CpuTiming::FrameSample sample{};
    };

    struct Event
    {
        GpuTimestampProfiler::FrameGap gap{};

        CpuRecord previous;
        CpuRecord current;
    };

    void RecordCpu(const CpuTiming::FrameSample& sample)
    {
        if (!sample.submitted || (m_hasCpuRecord && sample.frameSerial <= m_latestCpuSerial))
        {
            return;
        }

        m_cpuRecords[m_nextCpuRecord] = sample;

        m_nextCpuRecord = (m_nextCpuRecord + 1u) % m_cpuRecords.size();

        if (m_cpuRecordCount < m_cpuRecords.size())
        {
            ++m_cpuRecordCount;
        }

        m_latestCpuSerial = sample.frameSerial;

        m_hasCpuRecord = true;
        RefreshPendingEvents();
    }

    void RecordGaps(const GpuTimestampProfiler::Results& results)
    {
        for (unsigned int i = 0; i < results.resolvedGapCount; ++i)
        {
            const auto& gap = results.resolvedGaps[i];

            if (m_hasGpuRecord && gap.currentSerial <= m_latestGpuSerial)
            {
                continue;
            }

            m_latestGpuSerial = gap.currentSerial;
            m_hasGpuRecord = true;

            if (gap.milliseconds <= 10.0)
            {
                continue;
            }

            Event& event = m_events[m_nextEvent];
            event = {};

            event.gap = gap;
            Resolve(event.previous, gap.previousSerial);
            Resolve(event.current, gap.currentSerial);
            m_nextEvent = (m_nextEvent + 1u) % m_events.size();

            if (m_eventCount < m_events.size())
            {
                ++m_eventCount;
            }
        }
    }

    unsigned int EventCount() const
    {
        return m_eventCount;
    }

    const Event& NewestEvent(unsigned int offset) const
    {
        return m_events[(m_nextEvent + m_events.size() - 1u - offset) % m_events.size()];
    }

    private:
    void Resolve(
      CpuRecord& record,
      uint64_t serial)
    {
        if (record.state != RecordState::Pending)
        {
            return;
        }

        for (unsigned int i = 0; i < m_cpuRecordCount; ++i)
        {
            if (m_cpuRecords[i].frameSerial == serial)
            {
                record.sample = m_cpuRecords[i];
                record.state = RecordState::Available;
                return;
            }
        }

        // CPU records are published in serial order. An absent older record cannot arrive later.

        if (m_hasCpuRecord && serial <= m_latestCpuSerial)
        {
            record.state = RecordState::Missing;
        }
    }

    void RefreshPendingEvents()
    {
        for (unsigned int i = 0; i < m_eventCount; ++i)
        {
            Resolve(m_events[i].previous, m_events[i].gap.previousSerial);

            Resolve(m_events[i].current, m_events[i].gap.currentSerial);
        }
    }

    std::array<CpuTiming::FrameSample, 120> m_cpuRecords{};

    std::array<Event, 16> m_events{};
    unsigned int m_cpuRecordCount = 0;

    unsigned int m_nextCpuRecord = 0;
    unsigned int m_eventCount = 0;
    unsigned int m_nextEvent = 0;
    uint64_t m_latestCpuSerial = 0;
    uint64_t m_latestGpuSerial = 0;

    bool m_hasCpuRecord = false;
    bool m_hasGpuRecord = false;
};
