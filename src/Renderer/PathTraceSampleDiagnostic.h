#pragma once

#include "../../assets/Shaders/PathTraceSampleRecord.h"
#include "Renderer/Buffer.h"
#include "Renderer/DescriptorHeap.h"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

struct PathTraceSampleDiagnosticSettings
{
    bool enabled = false;
    int x = 0, y = 0;
    float threshold = 100.0f; // Capture detail only; never changes radiance.
    std::uint32_t clearRequest = 0;
    // R.9I.1D automatic capture: independent of the selected pixel above.
    bool autoEnabled = false;
    float autoThreshold = 10.0f; // Detection filter only; never changes radiance.
    std::uint32_t autoClearRequest = 0;
};

struct PathTraceAutoCandidate
{
    PathTraceSampleRecord record{};
    unsigned x = 0, y = 0;
    std::uint64_t run = 0;
    bool bucketMaximum = true; // false: a stronger event in the same bucket and frame lost the write lock.
    bool meanAvailable = false;
};

struct PathTraceAutoCaptureStatus
{
    std::uint64_t run = 0;
    float threshold = 0.0f;
    std::uint64_t framesCaptured = 0, framesSkipped = 0, crossings = 0, bucketEvents = 0, maximaLost = 0,
        lockFailures = 0, meanLockFailures = 0, meanUnavailable = 0, invalid = 0, notRetained = 0;
    std::string state = "Disabled";
    std::vector<PathTraceAutoCandidate> candidates; // Strongest indirect luminance first.
};

struct PathTraceSampleDiagnosticStatus
{
    unsigned width = 0, height = 0, currentSamples = 0;
    std::uint64_t run = 0, captures = 0, dropped = 0, overwritten = 0;
    std::string state = "Disabled";
    PathTraceSampleRecord peak{};
    bool havePeak = false;
    std::vector<PathTraceSampleRecord> history;
    PathTraceAutoCaptureStatus automatic;
};

inline bool CreatePathTraceDiagnosticReadback(ID3D12Device* device, unsigned bytes,
    Microsoft::WRL::ComPtr<ID3D12Resource>& readback)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
}

// R.9I.1D: per dispatch, each pixel's qualifying samples compete for one of PathTraceAutoBucketCount hashed
// buckets (strongest wins). Completed bucket winners merge on the CPU into a bounded strongest-first list.
// Same two-slot, never-wait, quarantine-on-abandon lifetime as the selected-pixel slots.
class PathTraceAutoCapture
{
public:
    static constexpr unsigned CandidateCapacity = 32;
    static constexpr unsigned SlotCount = 2;
    static constexpr unsigned Bytes = PathTraceAutoBucketCount * PathTraceAutoBucketStride;

    void Configure(const PathTraceSampleDiagnosticSettings& settings, unsigned width, unsigned height,
        std::uint64_t run, std::uint64_t completedFence, PathTraceAutoCaptureStatus& status)
    {
        const bool changed = settings.autoEnabled != m_settings.autoEnabled ||
            settings.autoThreshold != m_settings.autoThreshold ||
            settings.autoClearRequest != m_settings.autoClearRequest ||
            width != m_width ||
            height != m_height ||
            run != status.run;

        if(changed)
        {
            ++m_generation;
            status = {};
        }

        m_settings = settings;
        m_width = width;
        m_height = height;
        status.run = run;
        status.threshold = settings.autoThreshold;
        AbandonRecording();

        // GetCompletedValue's device-removal sentinel: never treat every fence as complete.
        m_completionUnknown = completedFence == UINT64_MAX;

        if(m_completionUnknown)
        {
            status.state = "Fence completion unavailable (device removed); capture suspended";
            return;
        }

        // Consume completed slots in fence order; Map only after completion is proven.
        std::array<unsigned, SlotCount> order{0, 1};

        if(m_slots[0].fence > m_slots[1].fence)
        {
            std::swap(order[0], order[1]);
        }

        for(unsigned slotIndex : order)
        {
            auto& slot = m_slots[slotIndex];

            if(!slot.fence || slot.fence > completedFence)
            {
                continue;
            }

            if(slot.generation == m_generation && settings.autoEnabled)
            {
                void* mapped = nullptr;
                const D3D12_RANGE read{0, Bytes};

                if(SUCCEEDED(slot.readback->Map(0, &read, &mapped)))
                {
                    Merge(static_cast<const std::uint8_t*>(mapped), slot, run, status);
                    const D3D12_RANGE written{0, 0};
                    slot.readback->Unmap(0, &written);
                    ++status.framesCaptured;
                }
                else
                {
                    ++status.framesSkipped;
                }
            }

            slot.fence = 0;
        }

        status.state = settings.autoEnabled ? "Armed; waiting for a fresh path batch" : "Disabled";
    }

    unsigned Begin(ID3D12Device* device, DescriptorHeap& heap, ID3D12GraphicsCommandList* commands,
        unsigned frameSlot, unsigned samples, unsigned firstSample, unsigned frame, PathTraceAutoCaptureStatus& status)
    {
        m_recordedSlot = -1;

        if(!m_settings.autoEnabled || m_completionUnknown)
        {
            return UINT_MAX;
        }

        auto& slot = m_slots[frameSlot % SlotCount];

        // A skipped frame still renders normally; it is only unobserved, and counted as such.
        if(slot.fence || slot.quarantined || slot.recording)
        {
            ++status.framesSkipped;
            status.state = "Readback busy/quarantined; frame not observed";
            return UINT_MAX;
        }

        if(!m_zero && !CreateZero(device))
        {
            ++status.framesSkipped;
            status.state = "Diagnostic clear-source allocation failed";
            return UINT_MAX;
        }

        if(!slot.gpu.GetResource())
        {
            if(!slot.gpu.InitializeGPUWritable(device, Bytes) || !slot.gpu.CreateRawUAV(device, heap))
            {
                slot.gpu.Shutdown();
                ++status.framesSkipped;
                status.state = "Diagnostic allocation failed";
                return UINT_MAX;
            }
        }

        if(!slot.readback && !CreatePathTraceDiagnosticReadback(device, Bytes, slot.readback))
        {
            ++status.framesSkipped;
            status.state = "Diagnostic readback allocation failed";
            return UINT_MAX;
        }

        // Buckets hold only this dispatch's events; the CPU list carries retention across frames.
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyBufferRegion(slot.gpu.GetResource(), 0, m_zero.Get(), 0, Bytes);
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        slot.firstSample = firstSample;
        slot.frame = frame;
        slot.batch = samples;
        slot.generation = m_generation;
        slot.recording = true;
        m_recordedSlot = int(frameSlot % SlotCount);
        status.state = "Capturing fresh individual samples";
        return slot.gpu.GetUAVDescriptorIndex();
    }

    void Copy(ID3D12GraphicsCommandList* commands)
    {
        if(m_recordedSlot < 0)
        {
            return;
        }

        auto& slot = m_slots[m_recordedSlot];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = slot.gpu.GetResource();
        commands->ResourceBarrier(1, &barrier);
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(slot.readback.Get(), 0, slot.gpu.GetResource(), 0, Bytes);
        // Matches buffer decay at ExecuteCommandLists completion, as for the selected-pixel slots.
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_COMMON);
    }

    void Commit(std::uint64_t fence)
    {
        if(m_recordedSlot < 0)
        {
            return;
        }

        auto& slot = m_slots[m_recordedSlot];
        slot.recording = false;
        slot.fence = fence;
        m_recordedSlot = -1;
    }

    // An unsubmitted recording may still be referenced by discarded commands; never reuse or free it early.
    void AbandonRecording()
    {
        for(auto& slot : m_slots)
        {
            if(slot.recording)
            {
                slot.recording = false;
                slot.quarantined = true;
            }
        }

        m_recordedSlot = -1;
    }

    void Invalidate(PathTraceAutoCaptureStatus& status)
    {
        ++m_generation;
        // Like the selected-pixel status, run identity survives.
        const std::uint64_t run = status.run;
        status = {};
        status.run = run;
        status.threshold = m_settings.autoThreshold;
        status.state = "Scene history invalidated; waiting for a new PT run";
    }

    void Shutdown() // Caller has drained the renderer queue.
    {
        for(auto& slot : m_slots)
        {
            slot.gpu.Shutdown();
            slot.readback.Reset();
            slot.fence = 0;
            slot.recording = false;
            slot.quarantined = false;
        }

        m_zero.Reset();
        m_recordedSlot = -1;
        m_completionUnknown = false;
        ++m_generation;
    }

    // Strongest first; an equal later candidate ranks after the earlier one.
    static void Retain(const PathTraceAutoCandidate& candidate, PathTraceAutoCaptureStatus& status)
    {
        auto& list = status.candidates;
        const auto strongerThan = [](float luminance, const PathTraceAutoCandidate& other)
        {
            return luminance > other.record.indirect[3];
        };
        if(list.size() == CandidateCapacity)
        {
            ++status.notRetained;

            // Not strictly stronger than the weakest: an equal later candidate ranks after it, so it would be evicted.
            if(!strongerThan(candidate.record.indirect[3], list.back()))
            {
                return;
            }

            list.pop_back();
        }

        // Searched after any eviction: pop_back invalidates an iterator to the former last element.
        const auto position = std::upper_bound(list.begin(), list.end(), candidate.record.indirect[3], strongerThan);
        list.insert(position, candidate);
    }

private:
    struct Slot
    {
        Buffer gpu;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        std::uint64_t fence = 0, generation = 0;
        unsigned firstSample = 0, frame = 0, batch = 0;
        bool recording = false, quarantined = false;
    };

    // Shared all-zero copy source that clears a slot's buckets before each observed dispatch.
    bool CreateZero(ID3D12Device* device)
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = Bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        const bool created = SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zero)));

        if(!created || FAILED(m_zero->Map(0, &noRead, &mapped)))
        {
            m_zero.Reset();
            return false;
        }

        std::memset(mapped, 0, Bytes);
        m_zero->Unmap(0, nullptr);
        return true;
    }

    void Merge(const std::uint8_t* bytes, const Slot& slot, std::uint64_t run, PathTraceAutoCaptureStatus& status)
    {
        for(unsigned bucket = 0; bucket < PathTraceAutoBucketCount; ++bucket)
        {
            const std::uint8_t* base = bytes + bucket * PathTraceAutoBucketStride;
            std::uint32_t header[8];
            std::memcpy(header, base, sizeof(header));
            status.crossings += header[3];
            status.lockFailures += header[4];
            status.meanLockFailures += header[5];

            if(!header[0])
            {
                continue;
            }

            ++status.bucketEvents;
            PathTraceAutoCandidate candidate;
            std::memcpy(&candidate.record, base + PathTraceAutoHeaderBytes, sizeof(PathTraceSampleRecord));
            const auto& record = candidate.record;
            candidate.x = header[2] & 0xFFFFu;
            candidate.y = header[2] >> 16;
            candidate.run = run;

            // Never publish bytes this dispatch did not write. A bucket whose every maximum lost the lock has
            // no record from this dispatch.
            const bool identityMatches = record.meta[0] == slot.frame &&
                record.meta[2] == slot.firstSample &&
                record.meta[3] == slot.batch &&
                record.meta[1] - slot.firstSample < slot.batch &&
                candidate.x < m_width &&
                candidate.y < m_height &&
                record.indirect[3] >= m_settings.autoThreshold;

            if(!identityMatches)
            {
                if(header[4])
                {
                    ++status.maximaLost;
                }
                else
                {
                    ++status.invalid;
                }

                continue;
            }

            std::uint32_t storedKey;
            std::memcpy(&storedKey, &record.indirect[3], sizeof(storedKey));
            candidate.bucketMaximum = storedKey == header[0];

            if(!candidate.bucketMaximum)
            {
                ++status.maximaLost;
            }

            candidate.meanAvailable = record.mean[3] >= 0.0f;

            if(!candidate.meanAvailable)
            {
                ++status.meanUnavailable;
            }

            Retain(candidate, status);
        }
    }

    std::array<Slot, SlotCount> m_slots;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_zero;
    PathTraceSampleDiagnosticSettings m_settings;
    unsigned m_width = 0, m_height = 0;
    std::uint64_t m_generation = 0;
    int m_recordedSlot = -1;
    bool m_completionUnknown = false;
};

// Two fixed slots, independent of image extent. Never wait for a fence. A recording not
// associated with a successful submission is quarantined until renderer shutdown/drain.
class PathTraceSampleDiagnostic
{
public:
    static constexpr unsigned Capacity = 64;
    static constexpr unsigned HistoryCapacity = 128;
    static constexpr unsigned SlotCount = 2;
    static constexpr unsigned Bytes = Capacity * sizeof(PathTraceSampleRecord);

    void Configure(const PathTraceSampleDiagnosticSettings& settings, unsigned width, unsigned height,
        unsigned samples, std::uint64_t run, std::uint64_t completedFence)
    {
        const bool changed = settings.enabled != m_settings.enabled || settings.x != m_settings.x ||
            settings.y != m_settings.y || settings.clearRequest != m_settings.clearRequest ||
            settings.threshold != m_settings.threshold || width != m_status.width || height != m_status.height ||
            run != m_status.run;
        if(changed)
        {
            ++m_generation;
            m_status.history.clear();
            m_status.captures = m_status.dropped = m_status.overwritten = 0;
            m_status.peak = {};
            m_status.havePeak = false;
        }
        m_settings = settings;
        m_status.width = width;
        m_status.height = height;
        m_status.currentSamples = samples;
        m_status.run = run;
        AbandonRecording();
        m_completionUnknown = completedFence == UINT64_MAX; // GetCompletedValue device removal sentinel.
        if(m_completionUnknown)
        {
            m_status.state = "Fence completion unavailable (device removed); capture suspended";
            m_automatic.Configure(settings, width, height, run, completedFence, m_status.automatic);
            return;
        }
        std::array<unsigned, SlotCount> order{0, 1};
        if(m_slots[0].fence > m_slots[1].fence) std::swap(order[0], order[1]);
        for(unsigned slotIndex : order)
        {
            auto& slot = m_slots[slotIndex];
            if(!slot.fence || slot.fence > completedFence) continue;
            if(slot.generation == m_generation && settings.enabled)
            {
                void* mapped = nullptr;
                const D3D12_RANGE read{0, slot.count * sizeof(PathTraceSampleRecord)};
                if(SUCCEEDED(slot.readback->Map(0, &read, &mapped)))
                {
                    const auto* records = static_cast<const PathTraceSampleRecord*>(mapped);
                    for(unsigned i = 0; i < slot.count; ++i)
                    {
                        if(records[i].meta[0] != slot.frame || records[i].meta[1] != slot.firstSample + i ||
                            records[i].meta[2] != slot.firstSample || records[i].meta[3] != slot.batch)
                        {
                            ++m_status.dropped;
                            continue; // No matching shader write: never publish old/uninitialized bytes.
                        }
                        ++m_status.captures;
                        if(m_status.history.size() == HistoryCapacity)
                        {
                            m_status.history.erase(m_status.history.begin());
                            ++m_status.overwritten;
                        }
                        m_status.history.push_back(records[i]);
                        if(std::isfinite(records[i].indirect[3]) &&
                            (!m_status.havePeak || records[i].indirect[3] > m_status.peak.indirect[3]))
                        {
                            m_status.peak = records[i];
                            m_status.havePeak = true;
                        }
                    }
                    const D3D12_RANGE written{0, 0};
                    slot.readback->Unmap(0, &written);
                }
                else m_status.dropped += slot.count;
            }
            slot.fence = 0;
        }
        m_status.state = settings.enabled ? "Armed; waiting for a fresh path batch" : "Disabled";
        m_automatic.Configure(settings, width, height, run, completedFence, m_status.automatic);
    }

    unsigned Begin(ID3D12Device* device, DescriptorHeap& heap, ID3D12GraphicsCommandList* commands,
        unsigned frameSlot, unsigned samples, unsigned firstSample, unsigned frame)
    {
        m_recordedSlot = -1;
        if(!m_settings.enabled) return UINT_MAX;
        if(m_completionUnknown) return UINT_MAX;
        if(m_settings.x < 0 || m_settings.y < 0 || unsigned(m_settings.x) >= m_status.width ||
            unsigned(m_settings.y) >= m_status.height)
        {
            m_status.state = "Selected coordinate outside render target";
            return UINT_MAX;
        }
        auto& slot = m_slots[frameSlot % SlotCount];
        if(slot.fence || slot.quarantined || slot.recording)
        {
            m_status.dropped += samples;
            m_status.state = "Readback busy/quarantined; batch dropped";
            return UINT_MAX;
        }
        if(!slot.gpu.GetResource())
        {
            if(!slot.gpu.InitializeGPUWritable(device, Bytes) || !slot.gpu.CreateRawUAV(device, heap))
            {
                slot.gpu.Shutdown();
                m_status.state = "Diagnostic allocation failed";
                m_status.dropped += samples;
                return UINT_MAX;
            }
        }
        if(!slot.readback)
        {
            if(!CreatePathTraceDiagnosticReadback(device, Bytes, slot.readback))
            {
                m_status.state = "Diagnostic readback allocation failed";
                m_status.dropped += samples;
                return UINT_MAX;
            }
        }
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.count = (std::min)(samples, Capacity);
        slot.firstSample = firstSample;
        slot.frame = frame;
        slot.batch = samples;
        m_status.dropped += samples - slot.count;
        slot.generation = m_generation;
        slot.recording = true;
        m_recordedSlot = int(frameSlot % SlotCount);
        m_status.state = "Capturing fresh individual samples";
        return slot.gpu.GetUAVDescriptorIndex();
    }

    unsigned BeginAutomatic(ID3D12Device* device, DescriptorHeap& heap, ID3D12GraphicsCommandList* commands,
        unsigned frameSlot, unsigned samples, unsigned firstSample, unsigned frame)
    {
        return m_automatic.Begin(device, heap, commands, frameSlot, samples, firstSample, frame, m_status.automatic);
    }

    void Copy(ID3D12GraphicsCommandList* commands)
    {
        m_automatic.Copy(commands);
        if(m_recordedSlot < 0) return;
        auto& slot = m_slots[m_recordedSlot];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = slot.gpu.GetResource();
        commands->ResourceBarrier(1, &barrier);
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(slot.readback.Get(), 0, slot.gpu.GetResource(), 0,
            slot.count * sizeof(PathTraceSampleRecord));
        // Buffers decay to COMMON at ExecuteCommandLists completion. End explicitly
        // in COMMON so Buffer's CPU tracking also matches the next slot reuse.
        slot.gpu.Transition(commands, D3D12_RESOURCE_STATE_COMMON);
    }

    void Commit(std::uint64_t fence)
    {
        m_automatic.Commit(fence);
        if(m_recordedSlot < 0) return;
        auto& slot = m_slots[m_recordedSlot];
        slot.recording = false;
        slot.fence = fence;
        m_recordedSlot = -1;
    }

    void AbandonRecording()
    {
        m_automatic.AbandonRecording();
        for(auto& slot : m_slots)
            if(slot.recording) { slot.recording = false; slot.quarantined = true; }
        m_recordedSlot = -1;
    }

    void Invalidate()
    {
        m_automatic.Invalidate(m_status.automatic);
        ++m_generation;
        m_status.history.clear();
        m_status.havePeak = false;
        m_status.captures = m_status.dropped = m_status.overwritten = 0;
        m_status.currentSamples = 0;
        m_status.state = "Scene history invalidated; waiting for a new PT run";
    }

    void Shutdown() // Caller has drained the renderer queue.
    {
        m_automatic.Shutdown();
        for(auto& slot : m_slots)
        {
            slot.gpu.Shutdown();
            slot.readback.Reset();
            slot.fence = 0;
            slot.recording = slot.quarantined = false;
        }
        m_recordedSlot = -1;
        m_status = {};
        m_completionUnknown = false;
        ++m_generation;
    }
    const PathTraceSampleDiagnosticStatus& Status() const { return m_status; }

private:
    struct Slot
    {
        Buffer gpu;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        std::uint64_t fence = 0, generation = 0;
        unsigned count = 0;
        unsigned firstSample = 0, frame = 0, batch = 0;
        bool recording = false, quarantined = false;
    };
    std::array<Slot, SlotCount> m_slots;
    PathTraceAutoCapture m_automatic;
    PathTraceSampleDiagnosticSettings m_settings;
    PathTraceSampleDiagnosticStatus m_status;
    std::uint64_t m_generation = 0;
    int m_recordedSlot = -1;
    bool m_completionUnknown = false;
};
