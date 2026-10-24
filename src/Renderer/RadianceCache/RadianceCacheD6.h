#pragma once

#include "Core/Log.h"
#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RadianceCache/RadianceCacheD6Basis.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/SceneLightingEpoch.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <bit>
#include <climits>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <wrl/client.h>

// SHARC.4C/4D production D6 radiance cache: GPU resources, generation clear, retirement, and (4D) the learning binding
// and resolve. The path tracer's cache library inserts samples (learning only); nothing looks the cache up yet.
// SHARC.4D-Q (experimental, Insertion::Deferred): the learning library stores training records instead, and
// RecordDeferredInsertion inserts them before resolve; the record buffer exists only while deferred frames learn.
// Independent of SHARC.2's RadianceCache.
// Header-only, so the build needs no new translation unit. Layout and header: assets/Shaders/RadianceCacheD6.hlsli.
//
// Generation contract (build/sharc-4c/REPORT.md section 6). The cache's contents belong to the scene-lighting epoch
// named in its GPU header. RecordGeneration, called in the frame's command list after the epoch latch and before any
// cache access, clears every entry buffer and then writes the header when the latched epoch differs from the contents'
// epoch. All frame work runs on one queue, so earlier frames' accesses complete before the clear and later ones see
// it. The clear counts only once its list is submitted (CommitSubmission); an unsubmitted or uncertain submission
// forces the next frame to clear again. Consumers must check the header (RadianceCacheD6HeaderAccepts) against their
// frame's epoch and identity, and must not use the cache in a frame where IsUsable() is false.
class RadianceCacheD6
{
public:
    struct Settings
    {
        // A power of two in [MinimumSlotCount, MaximumSlotCount].
        std::uint32_t slotCount = 1u << 18;
        // World-space cell edge in metres (the key the entries will use).
        float cellSize = 0.1f;
        // SHARC.5F (experimental): distance-adaptive levels of cellSize * 2^level around the spatial anchor
        // (RadianceCacheD6MakeBindingKey). A different key encoding, so a change reallocates and clears the cache.
        bool adaptive = false;

        bool operator==(const Settings&) const = default;
    };

    static constexpr std::uint32_t MinimumSlotCount = 1u << 10;
    static constexpr std::uint32_t MaximumSlotCount = 1u << 20;
    static constexpr std::uint32_t BucketSlots = 8;
    static constexpr std::uint32_t AccumulationWords = 40;
    static constexpr std::uint32_t PublishedWords = 24;
    static constexpr std::uint32_t StatisticWords = 16;
    static constexpr std::uint32_t HeaderWords = 4;
    // Versions of this implementation, its D6 layout and its key encoding: any change makes old contents meaningless.
    static constexpr std::uint32_t ImplementationVersion = 1;
    static constexpr std::uint32_t LayoutVersion = 1;
    static constexpr std::uint32_t KeyVersion = 1;
    static constexpr std::uint32_t AdaptiveKeyVersion = 2;
    // Resolve: the history weight cap (past it, the published value is a moving average), the staleness horizon in
    // resolves, and the relative-variance blend (the validated SHARC.3a-V2/V4 values).
    static constexpr float HistoryCap = 4096.0f;
    static constexpr std::uint32_t StaleFrames = 300;
    static constexpr float VarianceBlend = 0.1f;

    // Must match RadianceCacheD6Binding (RadianceCacheD6.hlsli).
    struct Binding
    {
        std::uint32_t tagsIndex = 0;
        std::uint32_t keysIndex = 0;
        std::uint32_t accumulationIndex = 0;
        std::uint32_t publishedIndex = 0;

        std::uint32_t bucketFlagsIndex = 0;
        std::uint32_t statisticsIndex = 0;
        std::uint32_t headerIndex = 0;
        std::uint32_t basisIndex = 0;

        std::uint32_t bucketCount = 0;
        std::uint32_t frameIndex = 0;
        std::uint32_t epochLow = 0;
        std::uint32_t epochHigh = 0;

        std::uint32_t configurationIdentity = 0;
        float inverseCellSize = 0.0f;
        float maximumSampleCount = 0.0f;
        float varianceBlend = 0.0f;

        std::uint32_t staleFrames = 0;
        std::uint32_t learning = 0;
        // SHARC.4D-P profiling controls (diagnostics; 0 = production behaviour), see RadianceCacheD6Binding.
        std::uint32_t learningThinning = 0;
        std::uint32_t harvestStop = 0;

        // SHARC.4D-Q deferred insertion (0 for inline insertion and resolve).
        std::uint32_t recordsIndex = 0;
        std::uint32_t recordCapacity = 0;
        std::uint32_t recordSequence = 0;
        std::uint32_t deferred = 0;

        // SHARC.4D-R (diagnostics), see RadianceCacheD6Binding.
        std::uint32_t queryMode = 0;
        std::uint32_t queryStatisticsIndex = 0;
        std::uint32_t trainingTile = 0;
        std::uint32_t trainingTilesX = 0;

        // SHARC.4D-S (diagnostics), see RadianceCacheD6Binding.
        float queryThreshold = 0.0f;
        std::uint32_t queryCounters = 0;
        std::uint32_t queryPadding[2]{};

        // SHARC.5F, see RadianceCacheD6Binding.
        float anchor[3]{};
        std::uint32_t spatialMode = 0;

        // SHARC.6A history prior, see RadianceCacheD6Binding.
        std::uint32_t primaryRecordOffset = 0;
        std::uint32_t priorOutputIndex = 0;
        float priorWeight = 0.0f;
        std::uint32_t priorCountersIndex = 0;
    };

    static_assert(sizeof(Binding) == 10 * 16);

    // SHARC.4D-Q: where a learning frame's samples are inserted (the default is the validated SHARC.4D path).
    enum class Insertion
    {
        // At the end of each path, in the ray generation.
        Inline,
        // The ray generation stores one training record per path; CSInsertRecords inserts them before resolve.
        Deferred,
        // SHARC.4D-R (diagnostics): the image renders without learning, and a separate training dispatch traces one
        // pixel per SetTrainingTile tile, storing compact records that CSInsertRecords inserts.
        Sparse
    };

    // SHARC.4D-R query coverage counters (RADIANCE_CACHE_D6_QUERY_*).
    static constexpr std::uint32_t QueryWords = 16;
    static constexpr std::uint32_t QueryEligible = 0;
    static constexpr std::uint32_t QueryFound = 1;
    static constexpr std::uint32_t QueryCount64 = 2;
    static constexpr std::uint32_t QueryCount256 = 3;
    static constexpr std::uint32_t QueryCount1024 = 4;
    static constexpr std::uint32_t QueryCount4096 = 5;
    static constexpr std::uint32_t QueryAge1 = 6;
    static constexpr std::uint32_t QueryAge16 = 7;
    static constexpr std::uint32_t QueryAge64 = 8;
    static constexpr std::uint32_t QueryHeaderRejected = 9;
    static constexpr std::uint32_t QueryCount16 = 10;
    static constexpr std::uint32_t QueryTerminated = 11;
    static constexpr std::uint32_t QueryClamped = 12;
    static constexpr std::uint32_t QueryMissed = 13;
    static constexpr std::uint32_t QueryAbsentFull = 14;
    static constexpr std::uint32_t QueryAbsentFree = 15;
    // SHARC.6A history-prior counters (RADIANCE_CACHE_D6_PRIOR_*; words 10-14 are PriorRejectedReason + code - 1).
    static constexpr std::uint32_t PriorWords = 16;
    static constexpr std::uint32_t PriorSurfaces = 0;
    static constexpr std::uint32_t PriorAvailable = 1;
    static constexpr std::uint32_t PriorUnsupported = 2;
    static constexpr std::uint32_t PriorAbsent = 3;
    static constexpr std::uint32_t PriorLowConfidence = 4;
    static constexpr std::uint32_t PriorCoarse = 5;
    static constexpr std::uint32_t PriorNonfinite = 6;
    static constexpr std::uint32_t PriorRejectedHistory = 7;
    static constexpr std::uint32_t PriorUsed = 8;
    static constexpr std::uint32_t PriorWeightSum = 9;
    static constexpr std::uint32_t PriorRejectedReason = 10;
    // The prior's full weight in history frames (a fine cell; 0.4 m cells get half, 0.8 m cells none).
    static constexpr float PriorWeight = 2.0f;
    // SHARC.4E statistics readback: per frame slot, the query counters then the cache statistics.
    static constexpr std::uint32_t ReadbackSlots = 2;
    static constexpr std::uint32_t ReadbackWords = QueryWords + StatisticWords;

    // A training record (RADIANCE_CACHE_D6_RECORD_WORDS words) and its stamp word.
    static constexpr std::uint32_t RecordWords = 12;
    static constexpr std::uint64_t RecordBytes = (RecordWords + 1) * 4;
    // Stamps hold the sequence in their upper 31 bits.
    static constexpr std::uint32_t MaximumRecordSequence = 0x7FFFFFFFu;

    // Statistics words (RADIANCE_CACHE_D6_STATISTIC_*), cleared at the start of every learning frame.
    static constexpr std::uint32_t StatisticSampleAttempts = 0;
    static constexpr std::uint32_t StatisticRejectedSamples = 1;
    static constexpr std::uint32_t StatisticClampedSamples = 2;
    static constexpr std::uint32_t StatisticStoredSamples = 3;
    static constexpr std::uint32_t StatisticClaimedSlots = 4;
    static constexpr std::uint32_t StatisticHeaderRejected = 5;
    static constexpr std::uint32_t StatisticBucketFull = 6;
    static constexpr std::uint32_t StatisticZeroSamples = 7;
    static constexpr std::uint32_t StatisticOccupied = 8;
    static constexpr std::uint32_t StatisticPublished = 9;
    static constexpr std::uint32_t StatisticEvicted = 10;
    static constexpr std::uint32_t StatisticChecksumRejected = 11;
    static constexpr std::uint32_t StatisticOverflowDiscarded = 12;
    static constexpr std::uint32_t StatisticMaximumSlotSamples = 13;
    static constexpr std::uint32_t StatisticResolvedSamples = 14;
    static constexpr std::uint32_t StatisticOverflowedBuckets = 15;

    struct Resources
    {
        Buffer tags;
        Buffer keys;
        Buffer accumulation;
        Buffer published;
        Buffer bucketFlags;
        Buffer statistics;
        Buffer header;
        // The D6 radial basis (RadianceCacheD6Basis.h), read-only.
        Buffer basis;
    };

    struct Statistics
    {
        std::uint64_t allocations = 0;
        std::uint64_t allocationFailures = 0;
        std::uint64_t retirements = 0;
        std::uint64_t generationsRecorded = 0;
        std::uint64_t generationsCommitted = 0;
        std::uint64_t uncertainSubmissions = 0;
        // SHARC.4D-Q record buffers.
        std::uint64_t recordAllocations = 0;
        std::uint64_t recordAllocationFailures = 0;
        std::uint64_t recordRetirements = 0;
        std::uint64_t deferredInsertions = 0;
    };

    static bool IsValidSettings(const Settings& settings)
    {
        return std::has_single_bit(settings.slotCount) && settings.slotCount >= MinimumSlotCount &&
            settings.slotCount <= MaximumSlotCount && settings.cellSize > 0.0f && settings.cellSize < 1.0e6f;
    }

    static std::uint64_t ResourceBytes(const Settings& settings)
    {
        const std::uint64_t slots = settings.slotCount;
        return slots * 4 + slots * 8 + slots * 4 * AccumulationWords + slots * 4 * PublishedWords +
            slots / BucketSlots * 4 + 4 * StatisticWords + 4 * HeaderWords;
    }

    static RadianceCacheConfigurationIdentity IdentityOf(const Settings& settings)
    {
        RadianceCacheConfigurationIdentity identity;
        identity.implementationVersion = ImplementationVersion;
        identity.layoutVersion = LayoutVersion;
        identity.cellSize = settings.cellSize;
        identity.keyVersion = settings.adaptive ? AdaptiveKeyVersion : KeyVersion;
        identity.slotCount = settings.slotCount;
        identity.bucketSlots = BucketSlots;
        return identity;
    }

    // The 32-bit identity word of the GPU header (FNV-1a over the identity's fields).
    static std::uint32_t HeaderIdentity(const RadianceCacheConfigurationIdentity& identity)
    {
        const std::uint32_t words[] = {identity.implementationVersion, identity.layoutVersion,
            std::bit_cast<std::uint32_t>(identity.cellSize), identity.keyVersion, identity.slotCount, identity.bucketSlots};
        std::uint32_t hash = 2166136261u;

        for(std::uint32_t word : words)
        {
            for(int byte = 0; byte < 4; ++byte)
            {
                hash = (hash ^ ((word >> (byte * 8)) & 0xFFu)) * 16777619u;
            }
        }

        return hash;
    }

    bool HasResources() const
    {
        return m_resources.has_value();
    }

    // The identity of the current resources; all zero without resources.
    RadianceCacheConfigurationIdentity GetIdentity() const
    {
        return m_resources ? IdentityOf(m_settings) : RadianceCacheConfigurationIdentity{};
    }

    // Allocates resources for settings, or keeps the current ones if they already match. Replaced resources are
    // retired until lastSubmissionFence completes. A new allocation's contents belong to no epoch (header state 0,
    // contents uncleared) until a generation is recorded and committed. failForTesting: diagnostics fault injection.
    bool EnsureResources(ID3D12Device* device, ResourceUploader& uploader, ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap, const Settings& settings, std::uint64_t lastSubmissionFence, bool failForTesting)
    {
        if(m_resources && m_settings == settings)
        {
            return true;
        }

        // The previous configuration is no longer the one requested: it is retired even if the new one cannot exist,
        // so an unsupported request leaves no cache rather than the old one.
        Retire(lastSubmissionFence);

        if(!device || !IsValidSettings(settings) || !EnsurePipelines(device, shaderLibrary))
        {
            ++m_statistics.allocationFailures;
            return false;
        }
        Resources resources;
        const std::uint64_t slots = settings.slotCount;
        const bool created = !failForTesting &&
            CreateRawBuffer(device, descriptorHeap, resources.tags, slots * 4, L"SHARC D6 tags") &&
            CreateRawBuffer(device, descriptorHeap, resources.keys, slots * 8, L"SHARC D6 keys") &&
            CreateRawBuffer(device, descriptorHeap, resources.accumulation, slots * 4 * AccumulationWords,
                L"SHARC D6 accumulation") &&
            CreateRawBuffer(device, descriptorHeap, resources.published, slots * 4 * PublishedWords,
                L"SHARC D6 published") &&
            CreateRawBuffer(device, descriptorHeap, resources.bucketFlags, slots / BucketSlots * 4,
                L"SHARC D6 bucket flags") &&
            CreateRawBuffer(device, descriptorHeap, resources.statistics, 4 * StatisticWords, L"SHARC D6 statistics") &&
            CreateRawBuffer(device, descriptorHeap, resources.header, 4 * HeaderWords, L"SHARC D6 header") &&
            resources.basis.Initialize(device, uploader, RadianceCacheD6BasisBits, sizeof(RadianceCacheD6BasisBits),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
            resources.basis.CreateRawSRV(device, descriptorHeap);

        if(!created)
        {
            // Nothing was submitted with the partial set, so it is released at once.
            ReleaseResources(resources);
            ++m_statistics.allocationFailures;
            Log::Error("The D6 radiance cache resources could not be created; the reference renderer is unaffected.");
            return false;
        }

        m_resources = std::move(resources);
        m_settings = settings;
        m_committedEpoch.reset();
        m_pendingEpoch.reset();
        ++m_statistics.allocations;
        return true;
    }

    // Moves the resources to the retired list: work submitted up to lastSubmissionFence may still use them through
    // their descriptors, so both stay alive until that fence completes.
    void Retire(std::uint64_t lastSubmissionFence)
    {
        // The record buffer belongs to learning into these resources; a failed record allocation is retried with them.
        RetireRecords(lastSubmissionFence);
        m_recordsFailedCount = 0;

        if(m_priorCounters)
        {
            m_retiredRecords.push_back(RetiredRecords{Records{std::move(*m_priorCounters), 0}, lastSubmissionFence});
            m_priorCounters.reset();
        }

        if(m_queryStatistics)
        {
            m_retiredRecords.push_back(RetiredRecords{Records{std::move(*m_queryStatistics), 0}, lastSubmissionFence});
            m_queryStatistics.reset();
        }

        if(!m_resources)
        {
            return;
        }

        m_retired.push_back(RetiredResources{std::move(*m_resources), lastSubmissionFence});
        m_resources.reset();
        m_committedEpoch.reset();
        m_pendingEpoch.reset();
        ++m_statistics.retirements;
    }

    void ReleaseRetired(std::uint64_t completedFence)
    {
        for(size_t index = 0; index < m_retired.size();)
        {
            if(m_retired[index].lastUseFence <= completedFence)
            {
                ReleaseResources(m_retired[index].resources);
                m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(index));
            }
            else
            {
                ++index;
            }
        }

        for(size_t index = 0; index < m_retiredRecords.size();)
        {
            if(m_retiredRecords[index].lastUseFence <= completedFence)
            {
                m_retiredRecords[index].records.buffer.Shutdown();
                m_retiredRecords.erase(m_retiredRecords.begin() + static_cast<std::ptrdiff_t>(index));
            }
            else
            {
                ++index;
            }
        }
    }

    // After the renderer's final GPU drain only.
    void Shutdown()
    {
        if(m_resources)
        {
            ReleaseResources(*m_resources);
            m_resources.reset();
        }

        for(auto& retired : m_retired)
        {
            ReleaseResources(retired.resources);
        }

        m_retired.clear();

        if(m_records)
        {
            m_records->buffer.Shutdown();
            m_records.reset();
        }

        for(auto& retired : m_retiredRecords)
        {
            retired.records.buffer.Shutdown();
        }

        m_retiredRecords.clear();

        if(m_priorCounters)
        {
            m_priorCounters->Shutdown();
            m_priorCounters.reset();
        }

        if(m_queryStatistics)
        {
            m_queryStatistics->Shutdown();
            m_queryStatistics.reset();
        }

        m_recordsFailedCount = 0;
        m_clearPipeline.Shutdown();
        m_headerPipeline.Shutdown();
        m_resolvePipeline.Shutdown();
        m_insertPipeline.Shutdown();
        m_readback.Reset();
        m_readbackRecorded[0] = false;
        m_readbackRecorded[1] = false;
        m_pipelinesReady = false;
        m_pipelinesFailed = false;
        m_insertPipelineReady = false;
        m_insertPipelineFailed = false;
        m_committedEpoch.reset();
        m_pendingEpoch.reset();
        m_usable = false;
    }

    // A frame recording begins: a generation recorded by an earlier recording that was never submitted did not run.
    void BeginRecording()
    {
        m_pendingEpoch.reset();
        m_usable = false;
        // Nor did a record-buffer clear.
        m_recordsNeedClear = m_recordsNeedClear || m_recordsClearRecorded;
        m_recordsClearRecorded = false;
    }

    // Records, if needed, the clear and header for the frame's latched epoch. Must be called after the latch and before
    // any cache access in the same command list. The cache is usable this frame only if this returns true.
    bool RecordGeneration(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        const SceneLightingEpoch::Status& epoch)
    {
        m_usable = false;

        if(!m_resources || !m_pipelinesReady || !commandList || !epoch.valid)
        {
            return false;
        }

        if((m_committedEpoch && *m_committedEpoch == epoch.epoch) || (m_pendingEpoch && *m_pendingEpoch == epoch.epoch))
        {
            m_usable = true;
            return true;
        }

        Resources& resources = *m_resources;
        Buffer* entryBuffers[] = {&resources.tags, &resources.keys, &resources.accumulation, &resources.published,
            &resources.bucketFlags, &resources.statistics};
        const std::uint64_t slots = m_settings.slotCount;
        const std::uint64_t entryBytes[] = {slots * 4, slots * 8, slots * 4 * AccumulationWords,
            slots * 4 * PublishedWords, slots / BucketSlots * 4, 4 * StatisticWords};

        for(Buffer* buffer : entryBuffers)
        {
            buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        resources.header.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_clearPipeline.Bind(commandList);

        for(size_t index = 0; index < std::size(entryBuffers); ++index)
        {
            ClearConstants constants{};
            constants.targetIndex = entryBuffers[index]->GetUAVDescriptorIndex();
            constants.wordCount = static_cast<std::uint32_t>(entryBytes[index] / 4);

            if(!RecordDispatch(commandList, uploadBuffer, constants,
                (constants.wordCount + ClearWordsPerGroup - 1) / ClearWordsPerGroup))
            {
                return false;
            }
        }

        // Every entry word is zero before the header names the new epoch.
        RecordUAVBarriers(commandList, entryBuffers);
        m_headerPipeline.Bind(commandList);
        ClearConstants header{};
        header.targetIndex = resources.header.GetUAVDescriptorIndex();
        header.epoch[0] = static_cast<std::uint32_t>(epoch.epoch);
        header.epoch[1] = static_cast<std::uint32_t>(epoch.epoch >> 32);
        header.configurationIdentity = HeaderIdentity(IdentityOf(m_settings));
        header.state = HeaderCleared;

        if(!RecordDispatch(commandList, uploadBuffer, header, 1))
        {
            return false;
        }

        Buffer* everything[] = {&resources.tags, &resources.keys, &resources.accumulation, &resources.published,
            &resources.bucketFlags, &resources.statistics, &resources.header};
        RecordUAVBarriers(commandList, everything);
        m_pendingEpoch = epoch.epoch;
        m_trainedFrames = 0;
        ++m_statistics.generationsRecorded;
        m_usable = true;
        return true;
    }

    // The frame's list was submitted: its generation (if any) is now ordered ahead of every later frame.
    void CommitSubmission()
    {
        if(m_pendingEpoch)
        {
            m_committedEpoch = m_pendingEpoch;
            m_pendingEpoch.reset();
            ++m_statistics.generationsCommitted;
        }

        m_recordsClearRecorded = false;
    }

    // Submission failed after it may have reached the GPU: the contents' epoch is unknown, so the next frame clears.
    // So are the record stamps.
    void MarkSubmissionUncertain()
    {
        m_committedEpoch.reset();
        m_pendingEpoch.reset();
        m_usable = false;
        m_recordsNeedClear = m_records.has_value();
        m_recordsClearRecorded = false;
        ++m_statistics.uncertainSubmissions;
    }

    // SHARC.4E: the next RecordGeneration clears the cache and writes a new header for the same epoch (a user reset).
    // Like an uncertain submission, it needs no new scene-lighting epoch, so nothing else restarts.
    void RequestClear()
    {
        m_committedEpoch.reset();
        m_pendingEpoch.reset();
    }

    // Learning frames resolved into the current contents since their generation clear.
    std::uint64_t GetTrainedFrames() const
    {
        return m_trainedFrames;
    }

    // SHARC.4E statistics, after BeginTrackedRecording's wait for frameSlot: the counters the slot's previous frame
    // copied (false if it copied none), then this frame's copy is recorded at the end of its cache work. Never stalls.
    bool ReadStatistics(std::uint32_t frameSlot, std::uint32_t (&words)[ReadbackWords]) const
    {
        if(!m_readback || frameSlot >= ReadbackSlots || !m_readbackRecorded[frameSlot])
        {
            return false;
        }

        const D3D12_RANGE range{frameSlot * ReadbackWords * 4u, (frameSlot + 1) * ReadbackWords * 4u};
        void* mapped = nullptr;

        if(FAILED(m_readback->Map(0, &range, &mapped)))
        {
            return false;
        }

        std::memcpy(words, static_cast<const std::uint8_t*>(mapped) + range.Begin, sizeof(words));
        const D3D12_RANGE written{0, 0};
        m_readback->Unmap(0, &written);
        return true;
    }

    void RecordStatisticsReadback(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, std::uint32_t frameSlot)
    {
        if(!m_resources || !m_queryStatistics || !commandList || frameSlot >= ReadbackSlots)
        {
            return;
        }

        if(!m_readback)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = ReadbackSlots * ReadbackWords * 4;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if(!device || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback))))
            {
                return;
            }

            m_readback->SetName(L"SHARC D6 statistics readback");
        }

        Buffer* sources[] = {&*m_queryStatistics, &m_resources->statistics};
        const std::uint64_t offsets[] = {0, QueryWords * 4};
        const std::uint64_t sizes[] = {QueryWords * 4, StatisticWords * 4};

        for(size_t index = 0; index < std::size(sources); ++index)
        {
            sources[index]->Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->CopyBufferRegion(m_readback.Get(), frameSlot * ReadbackWords * 4 + offsets[index],
                sources[index]->GetResource(), 0, sizes[index]);
            sources[index]->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        m_readbackRecorded[frameSlot] = true;
    }

    // SHARC.5F: the world position adaptive levels are measured from (the frame's camera), for every binding made after
    // it is set. Set once per frame before any learning or query binding, so a frame's training and queries agree.
    void SetSpatialAnchor(float x, float y, float z)
    {
        m_spatialAnchor[0] = x;
        m_spatialAnchor[1] = y;
        m_spatialAnchor[2] = z;
    }

    // Whether the current frame recorded or confirmed a generation for its latched epoch.
    bool IsUsable() const
    {
        return m_usable;
    }

    // SHARC.4D learning, after RecordGeneration in the same list and before the path tracer's cache library runs:
    // clears the frame's statistics and returns the address of the insertion binding's constants (0: no learning this
    // frame). The insertion binding carries no published-entry buffer, so training can never read cached radiance.
    // SHARC.4D-Q: with Insertion::Deferred, the frame stores its samples as records under a new stamp sequence, so
    // EnsureRecords must have provided room for recordCount records (otherwise the frame does not learn).
    D3D12_GPU_VIRTUAL_ADDRESS RecordLearningBegin(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        const SceneLightingEpoch::Status& epoch, std::uint64_t recordCount = 0, std::uint32_t trainingTilesX = 0)
    {
        m_learningBinding = Binding{};
        const bool deferred = m_insertion != Insertion::Inline;

        if(!m_usable || !m_resources || !commandList ||
            (deferred && (!m_records || recordCount == 0 || recordCount > m_records->capacity || !m_insertPipelineReady)))
        {
            return 0;
        }

        Resources& resources = *m_resources;
        m_clearPipeline.Bind(commandList);
        ClearConstants clear{};
        clear.targetIndex = resources.statistics.GetUAVDescriptorIndex();
        clear.wordCount = StatisticWords;

        if(!RecordDispatch(commandList, uploadBuffer, clear, 1))
        {
            return 0;
        }

        Buffer* statistics[] = {&resources.statistics};
        RecordUAVBarriers(commandList, statistics);
        Binding binding = MakeBinding(epoch);
        binding.publishedIndex = UINT_MAX;
        binding.learning = 1;
        binding.learningThinning = m_profilingThinning;
        binding.harvestStop = m_profilingHarvestStop;
        binding.primaryRecordOffset = deferred ? m_primaryRecordOffset : 0u;

        // Profiling stop 6 reads published entries but inserts nothing, so it cannot train on them.
        if(m_profilingHarvestStop == 6)
        {
            binding.publishedIndex = resources.published.GetUAVDescriptorIndex();
        }

        if(deferred && !RecordRecordsBegin(commandList, uploadBuffer, binding))
        {
            return 0;
        }

        if(m_insertion == Insertion::Sparse)
        {
            binding.trainingTile = m_trainingTile;
            binding.trainingTilesX = trainingTilesX;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&binding, sizeof(Binding),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address != 0)
        {
            m_learningBinding = binding;
        }

        return address;
    }

    // SHARC.4D-Q: the learning frames' insertion path; Inline unless a diagnostics run selects Deferred.
    void SetInsertion(Insertion insertion)
    {
        m_insertion = insertion;
    }

    Insertion GetInsertion() const
    {
        return m_insertion;
    }

    // SHARC.4D-Q: before RecordLearningBegin of a deferred learning frame, the record buffer for recordCount records
    // (one per path of the dispatch: pixels x samples per frame). Allocated on first use; a different count replaces
    // it, the old buffer retired until lastSubmissionFence completes. A count the buffer cannot address, or a failed
    // allocation, returns false (the frame renders without learning); a failure is not retried for the same count
    // until the cache is retired. A new buffer's stamps are cleared before its first use. failForTesting:
    // diagnostics fault injection.
    bool EnsureRecords(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap,
        std::uint64_t recordCount, std::uint64_t lastSubmissionFence, bool failForTesting)
    {
        if(m_records && m_records->capacity == recordCount)
        {
            return true;
        }

        if(recordCount == m_recordsFailedCount)
        {
            return false;
        }

        RetireRecords(lastSubmissionFence);
        Records records;
        const std::uint64_t bytes = recordCount * RecordBytes;
        const bool created = !failForTesting && m_resources && device && recordCount > 0 &&
            recordCount <= MaximumRecordCount && EnsureInsertPipeline(device, shaderLibrary) &&
            CreateRawBuffer(device, descriptorHeap, records.buffer, bytes, L"SHARC D6 training records");

        if(!created)
        {
            records.buffer.Shutdown();
            m_recordsFailedCount = recordCount;
            ++m_statistics.recordAllocationFailures;
            Log::Error("The D6 radiance cache training records (" + std::to_string(recordCount) +
                ") could not be created; frames render without learning.");
            return false;
        }

        records.capacity = static_cast<std::uint32_t>(recordCount);
        m_records = std::move(records);
        m_recordsNeedClear = true;
        m_recordsFailedCount = 0;
        ++m_statistics.recordAllocations;
        return true;
    }

    // SHARC.4D-Q: after the deferred learning dispatch in the same list (RecordLearningBegin returned a deferred
    // binding), before RecordResolve. Inserts the frame's records with the learning binding: the header check, D6 values
    // and atomic insertion run here instead of at the end of each path.
    bool RecordDeferredInsertion(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer)
    {
        if(m_learningBinding.deferred == 0 || !m_records || !m_resources || !commandList)
        {
            return false;
        }

        // The ray generation's record stores complete before the insertion reads them.
        Buffer* records[] = {&m_records->buffer};
        RecordUAVBarriers(commandList, records);

        if(m_skipDeferredInsertionForTesting)
        {
            return true;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&m_learningBinding, sizeof(Binding),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            Log::Error("Failed to allocate D6 radiance cache insertion constants.");
            return false;
        }

        m_insertPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        DispatchRecords(commandList, m_learningBinding.recordCapacity);
        ++m_statistics.deferredInsertions;
        // RecordResolve's barrier orders the insertions before resolve.
        return true;
    }

    // Diagnostics only (SHARC.4D-Q): deferred frames store their records but CSInsertRecords does not run (the
    // record-writing cost alone; nothing is inserted).
    void SetSkipDeferredInsertionForTesting(bool skip)
    {
        m_skipDeferredInsertionForTesting = skip;
    }

    // SHARC.4D-R (diagnostics): Insertion::Sparse trains one pixel per tile x tile tile (1 = every pixel); 0 renders
    // without any training dispatch (and without insertion or resolve).
    void SetTrainingTile(std::uint32_t tile)
    {
        m_trainingTile = tile;
    }

    std::uint32_t GetTrainingTile() const
    {
        return m_trainingTile;
    }

    // Training threads per row, and in all, of a width x height image (one per tile).
    std::uint32_t TrainingTilesX(std::uint32_t width) const
    {
        return m_trainingTile == 0 ? 0 : (width + m_trainingTile - 1) / m_trainingTile;
    }

    std::uint64_t TrainingThreads(std::uint32_t width, std::uint32_t height) const
    {
        return m_trainingTile == 0 ? 0 :
            static_cast<std::uint64_t>(TrainingTilesX(width)) * ((height + m_trainingTile - 1) / m_trainingTile);
    }

    // SHARC.4D-R (diagnostics): render Raster Primary frames with the query library (PathTraceRadianceCacheQuery.hlsli)
    // in queryMode (RadianceCacheD6Binding); queryMode 3 needs EnsureQueryStatistics.
    void SetQuery(bool queryLibrary, std::uint32_t queryMode)
    {
        m_queryLibrary = queryLibrary;
        m_queryMode = queryMode;
    }

    bool UsesQueryLibrary() const
    {
        return m_queryLibrary;
    }

    // SHARC.4D-S (diagnostics): render with the query-termination library instead (queryMode 4 or 5), ending paths at
    // vertex 1 on entries with at least `threshold` samples (infinity: never); counters also counts coverage and
    // terminations (needs EnsureQueryStatistics).
    void SetQueryTermination(bool terminationLibrary, float threshold, bool counters)
    {
        m_queryTerminationLibrary = terminationLibrary;
        m_queryThreshold = threshold;
        m_queryCounters = counters;
    }

    bool UsesQueryTerminationLibrary() const
    {
        return m_queryLibrary && m_queryTerminationLibrary;
    }

    // SHARC.4E: render with the cached-radiance library (queryMode 1 substitutes, 0 never does), completing paths on
    // entries with at least `threshold` samples; counters also counts lookups and substitutions
    // (EnsureQueryStatistics).
    void SetQueryRender(bool renderLibrary, float threshold, bool counters)
    {
        m_queryRenderLibrary = renderLibrary;
        m_queryThreshold = threshold;
        m_queryCounters = counters;
    }

    bool UsesQueryRenderLibrary() const
    {
        return m_queryLibrary && m_queryRenderLibrary;
    }

    // Whether the query binding writes the coverage counters.
    bool QueryCounts() const
    {
        const bool countingLibrary = UsesQueryTerminationLibrary() || UsesQueryRenderLibrary();
        return m_queryMode == 3 || (countingLibrary && m_queryCounters);
    }

    std::uint32_t GetQueryMode() const
    {
        return m_queryMode;
    }

    // The query coverage counters (QueryWords words), allocated on first use and retired with the cache.
    bool EnsureQueryStatistics(ID3D12Device* device, DescriptorHeap& descriptorHeap)
    {
        if(m_queryStatistics)
        {
            return true;
        }

        Buffer buffer;

        if(!m_resources || !CreateRawBuffer(device, descriptorHeap, buffer, 4 * QueryWords, L"SHARC D6 query statistics"))
        {
            buffer.Shutdown();
            return false;
        }

        m_queryStatistics = std::move(buffer);
        return true;
    }

    // Diagnostics: the query counters (EnsureQueryStatistics must have succeeded).
    Buffer& GetQueryStatistics()
    {
        return *m_queryStatistics;
    }

    // SHARC.6A (experimental) history prior: whether training also learns primary-class (x0) entries and extraction
    // frames write the prior. Off by default; the render and training libraries are otherwise unchanged.
    void SetHistoryPrior(bool enabled)
    {
        m_historyPrior = enabled;
    }

    bool UsesHistoryPrior() const
    {
        return m_historyPrior;
    }

    // Per learning frame, before RecordLearningBegin: the first primary-class record (0: none this frame).
    void SetPrimaryRecordOffset(std::uint32_t offset)
    {
        m_primaryRecordOffset = offset;
    }

    // Diagnostics: the prior's full weight (PriorWeight by default).
    void SetPriorWeightForTesting(float weight)
    {
        m_priorWeight = weight;
    }

    // Per query frame, before RecordQueryBegin: the prior texture's UAV (0: this frame writes no prior).
    void SetHistoryPriorOutput(std::uint32_t uavIndex)
    {
        m_priorOutputIndex = uavIndex;
    }

    // The prior counters, counted with the query counters (SHARC statistics).
    bool EnsurePriorCounters(ID3D12Device* device, DescriptorHeap& descriptorHeap)
    {
        if(m_priorCounters)
        {
            return true;
        }

        Buffer buffer;

        if(!m_resources || !CreateRawBuffer(device, descriptorHeap, buffer, 4 * PriorWords, L"SHARC D6 prior counters"))
        {
            buffer.Shutdown();
            return false;
        }

        m_priorCounters = std::move(buffer);
        return true;
    }

    // The counters of the last counted prior frame (EnsurePriorCounters must have succeeded).
    Buffer& GetPriorCounters()
    {
        return *m_priorCounters;
    }

    // Whether this frame's query binding writes the prior and counts it.
    bool PriorCounted() const
    {
        return m_priorOutputIndex != 0 && QueryCounts() && m_priorCounters.has_value();
    }

    // Whether the last RecordQueryBegin cleared the prior counters for its frame (the temporal pass then counts too).
    bool PriorCountedThisFrame() const
    {
        return m_priorCountedThisFrame;
    }

    // SHARC.4D-R, after RecordGeneration in the same list and before the query library renders: the query binding's
    // constants (0: render with the reference library). The binding carries the published buffer for lookups only;
    // in mode 3 the frame's coverage counters are cleared first.
    D3D12_GPU_VIRTUAL_ADDRESS RecordQueryBegin(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        const SceneLightingEpoch::Status& epoch)
    {
        if(!m_queryLibrary || !m_usable || !m_resources || !commandList || (QueryCounts() && !m_queryStatistics))
        {
            return 0;
        }

        m_priorCountedThisFrame = false;
        Binding binding = MakeBinding(epoch);
        binding.queryMode = m_queryMode;
        binding.queryThreshold = m_queryThreshold;
        const bool countingLibrary = UsesQueryTerminationLibrary() || UsesQueryRenderLibrary();
        binding.queryCounters = countingLibrary && m_queryCounters ? 1u : 0u;

        if(QueryCounts())
        {
            m_queryStatistics->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_clearPipeline.Bind(commandList);
            ClearConstants clear{};
            clear.targetIndex = m_queryStatistics->GetUAVDescriptorIndex();
            clear.wordCount = QueryWords;

            if(!RecordDispatch(commandList, uploadBuffer, clear, 1))
            {
                return 0;
            }

            Buffer* statistics[] = {&*m_queryStatistics};
            RecordUAVBarriers(commandList, statistics);
            binding.queryStatisticsIndex = m_queryStatistics->GetUAVDescriptorIndex();
        }

        // SHARC.6A: the prior's output and weight; its counters are cleared here and read after the temporal pass.
        binding.priorOutputIndex = m_priorOutputIndex;
        binding.priorWeight = m_priorWeight;

        if(PriorCounted())
        {
            m_priorCounters->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_clearPipeline.Bind(commandList);
            ClearConstants clear{};
            clear.targetIndex = m_priorCounters->GetUAVDescriptorIndex();
            clear.wordCount = PriorWords;

            if(!RecordDispatch(commandList, uploadBuffer, clear, 1))
            {
                return 0;
            }

            Buffer* counters[] = {&*m_priorCounters};
            RecordUAVBarriers(commandList, counters);
            binding.priorCountersIndex = m_priorCounters->GetUAVDescriptorIndex();
            m_priorCountedThisFrame = true;
        }

        return uploadBuffer.Allocate(&binding, sizeof(Binding), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }

    // The record buffer's capacity in records (0 without one), its bytes, and the last learning binding (diagnostics).
    std::uint32_t GetRecordCapacity() const
    {
        return m_records ? m_records->capacity : 0;
    }

    std::uint64_t GetRecordBufferBytes() const
    {
        return m_records ? m_records->capacity * RecordBytes : 0;
    }

    size_t GetRetiredRecordCount() const
    {
        return m_retiredRecords.size();
    }

    const Binding& GetLearningBinding() const
    {
        return m_learningBinding;
    }

    // Diagnostics: the record buffer (GetRecordCapacity() must be non-zero).
    Buffer& GetRecordBuffer()
    {
        return m_records->buffer;
    }

    // SHARC.4D: after the path tracer's cache library in the same list. Folds the frame's accumulations into the
    // published history (one thread per bucket), evicts stale entries and zeroes the accumulation for the next frame.
    // The resolve checks the GPU header itself and changes nothing if it does not name this binding's epoch.
    bool RecordResolve(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        const SceneLightingEpoch::Status& epoch)
    {
        if(!m_usable || !m_resources || !commandList)
        {
            return false;
        }

        Resources& resources = *m_resources;
        Buffer* entries[] = {&resources.tags, &resources.keys, &resources.accumulation, &resources.published,
            &resources.bucketFlags, &resources.statistics};
        // The path tracer's insertions complete before resolve reads them.
        RecordUAVBarriers(commandList, entries);
        Binding binding = MakeBinding(epoch);
        binding.frameIndex = ++m_resolveFrame;
        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&binding, sizeof(Binding),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            Log::Error("Failed to allocate D6 radiance cache resolve constants.");
            return false;
        }

        m_resolvePipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch((binding.bucketCount + 63) / 64, 1, 1);
        ++m_trainedFrames;
        // Later frames' insertions and any later reader see the resolved entries.
        RecordUAVBarriers(commandList, entries);
        return true;
    }

    // Diagnostics only (SHARC.4D-P): learn in 1 of `thinning` 8 x 8 pixel blocks per frame, and stop the harvest at a
    // stage (RadianceCacheD6Binding::harvestStop). 0, 0 is production behaviour.
    void SetLearningProfiling(std::uint32_t thinning, std::uint32_t harvestStop)
    {
        m_profilingThinning = thinning;
        m_profilingHarvestStop = harvestStop;
    }

    // The published-buffer index of the last learning binding: always UINT_MAX (training never reads cached radiance).
    std::uint32_t GetLearningBindingPublishedIndex() const
    {
        return m_learningBinding.publishedIndex;
    }

    std::optional<std::uint64_t> GetCommittedEpoch() const
    {
        return m_committedEpoch;
    }

    const Statistics& GetStatistics() const
    {
        return m_statistics;
    }

    size_t GetRetiredCount() const
    {
        return m_retired.size();
    }

    const Settings& GetSettings() const
    {
        return m_settings;
    }

    // Diagnostics and future stages: the current resources (HasResources() must be true).
    Resources& GetResources()
    {
        return *m_resources;
    }

private:
    static constexpr std::uint32_t HeaderCleared = 1;
    // Matches CSClearWords: 64 threads of 16 words.
    static constexpr std::uint32_t ClearWordsPerGroup = 64 * 16;

    // Must match RadianceCacheD6ClearConstants (RadianceCacheD6.hlsl).
    struct ClearConstants
    {
        std::uint32_t targetIndex = 0;
        std::uint32_t wordCount = 0;
        std::uint32_t epoch[2]{};

        std::uint32_t configurationIdentity = 0;
        std::uint32_t state = 0;
        std::uint32_t padding[2]{};
    };

    static_assert(sizeof(ClearConstants) == 2 * 16);

    // CSInsertRecords and CSClearRecordStamps: 64 records per group, in rows of RADIANCE_CACHE_D6_INSERT_GROUPS_PER_ROW
    // groups.
    static constexpr std::uint32_t InsertThreadsPerGroup = 64;
    static constexpr std::uint32_t InsertGroupsPerRow = 1024;
    // The record buffer's bytes must fit a raw buffer's 32-bit addresses.
    static constexpr std::uint64_t MaximumRecordCount = UINT_MAX / RecordBytes;

    struct RetiredResources
    {
        Resources resources;
        std::uint64_t lastUseFence = 0;
    };

    // SHARC.4D-Q: the training records (RADIANCE_CACHE_D6_RECORD_WORDS words each), then a stamp word per record.
    struct Records
    {
        Buffer buffer;
        std::uint32_t capacity = 0;
    };

    struct RetiredRecords
    {
        Records records;
        std::uint64_t lastUseFence = 0;
    };

    void RetireRecords(std::uint64_t lastSubmissionFence)
    {
        if(!m_records)
        {
            return;
        }

        m_retiredRecords.push_back(RetiredRecords{std::move(*m_records), lastSubmissionFence});
        m_records.reset();
        m_recordsNeedClear = false;
        m_recordsClearRecorded = false;
        ++m_statistics.recordRetirements;
    }

    bool EnsureInsertPipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary)
    {
        if(m_insertPipelineReady)
        {
            return true;
        }

        if(m_insertPipelineFailed)
        {
            return false;
        }

        if(!CreatePipeline(device, shaderLibrary, "RadianceCacheD6Insert.hlsl", "CSInsertRecords", m_insertPipeline) ||
            !CreatePipeline(device, shaderLibrary, "RadianceCacheD6Insert.hlsl", "CSClearRecordStamps",
                m_clearStampsPipeline))
        {
            Log::Error("The D6 radiance cache deferred insertion pipelines could not be created.");
            m_insertPipeline.Shutdown();
            m_clearStampsPipeline.Shutdown();
            m_insertPipelineFailed = true;
            return false;
        }

        m_insertPipelineReady = true;
        return true;
    }

    void DispatchRecords(ID3D12GraphicsCommandList* commandList, std::uint32_t records) const
    {
        const std::uint32_t groups = (records + InsertThreadsPerGroup - 1) / InsertThreadsPerGroup;
        commandList->Dispatch((std::min)(groups, InsertGroupsPerRow), (groups + InsertGroupsPerRow - 1) / InsertGroupsPerRow,
            1);
    }

    // A deferred learning frame's part of RecordLearningBegin: the stamps cleared if they may be stale, the previous
    // work on the buffer ordered before this frame's stores, and a new stamp sequence in the binding.
    bool RecordRecordsBegin(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, Binding& binding)
    {
        Records& records = *m_records;
        records.buffer.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        binding.recordsIndex = records.buffer.GetUAVDescriptorIndex();
        binding.recordCapacity = records.capacity;

        // A wrapped sequence could match stamps of its earlier use.
        if(m_recordSequence >= MaximumRecordSequence)
        {
            m_recordSequence = 0;
            m_recordsNeedClear = true;
        }

        if(m_recordsNeedClear)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&binding, sizeof(Binding),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

            if(address == 0)
            {
                Log::Error("Failed to allocate D6 radiance cache record clear constants.");
                return false;
            }

            m_clearStampsPipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, address);
            DispatchRecords(commandList, records.capacity);
            m_recordsNeedClear = false;
            m_recordsClearRecorded = true;
        }

        // The clear and the previous frame's insertion complete before this frame's stores.
        Buffer* buffers[] = {&records.buffer};
        RecordUAVBarriers(commandList, buffers);
        binding.recordSequence = ++m_recordSequence;
        binding.deferred = 1;
        return true;
    }

    Binding MakeBinding(const SceneLightingEpoch::Status& epoch) const
    {
        const Resources& resources = *m_resources;
        Binding binding;
        binding.tagsIndex = resources.tags.GetUAVDescriptorIndex();
        binding.keysIndex = resources.keys.GetUAVDescriptorIndex();
        binding.accumulationIndex = resources.accumulation.GetUAVDescriptorIndex();
        binding.publishedIndex = resources.published.GetUAVDescriptorIndex();
        binding.bucketFlagsIndex = resources.bucketFlags.GetUAVDescriptorIndex();
        binding.statisticsIndex = resources.statistics.GetUAVDescriptorIndex();
        binding.headerIndex = resources.header.GetUAVDescriptorIndex();
        binding.basisIndex = resources.basis.GetDescriptorIndex();
        binding.bucketCount = m_settings.slotCount / BucketSlots;
        binding.frameIndex = m_resolveFrame;
        binding.epochLow = static_cast<std::uint32_t>(epoch.epoch);
        binding.epochHigh = static_cast<std::uint32_t>(epoch.epoch >> 32);
        binding.configurationIdentity = HeaderIdentity(IdentityOf(m_settings));
        binding.inverseCellSize = 1.0f / m_settings.cellSize;
        binding.maximumSampleCount = HistoryCap;
        binding.varianceBlend = VarianceBlend;
        binding.staleFrames = StaleFrames;
        binding.anchor[0] = m_spatialAnchor[0];
        binding.anchor[1] = m_spatialAnchor[1];
        binding.anchor[2] = m_spatialAnchor[2];
        binding.spatialMode = m_settings.adaptive ? 1u : 0u;
        return binding;
    }

    static void ReleaseResources(Resources& resources)
    {
        for(Buffer* buffer : {&resources.tags, &resources.keys, &resources.accumulation, &resources.published,
                 &resources.bucketFlags, &resources.statistics, &resources.header, &resources.basis})
        {
            buffer->Shutdown();
        }
    }

    static bool CreateRawBuffer(ID3D12Device* device, DescriptorHeap& descriptorHeap, Buffer& buffer,
        std::uint64_t bytes, const wchar_t* name)
    {
        if(bytes == 0 || bytes > UINT_MAX || !buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
            !buffer.CreateRawUAV(device, descriptorHeap) || !buffer.CreateRawSRV(device, descriptorHeap))
        {
            return false;
        }

        buffer.GetResource()->SetName(name);
        return true;
    }

    bool EnsurePipelines(ID3D12Device* device, ShaderLibrary& shaderLibrary)
    {
        if(m_pipelinesReady)
        {
            return true;
        }

        if(m_pipelinesFailed)
        {
            return false;
        }

        if(!CreatePipeline(device, shaderLibrary, "RadianceCacheD6.hlsl", "CSClearWords", m_clearPipeline) ||
            !CreatePipeline(device, shaderLibrary, "RadianceCacheD6.hlsl", "CSWriteHeader", m_headerPipeline) ||
            !CreatePipeline(device, shaderLibrary, "RadianceCacheD6Resolve.hlsl", "CSResolve", m_resolvePipeline))
        {
            Log::Error("The D6 radiance cache pipelines could not be created; the cache stays disabled.");
            m_clearPipeline.Shutdown();
            m_headerPipeline.Shutdown();
            m_resolvePipeline.Shutdown();
            m_pipelinesFailed = true;
            return false;
        }

        m_pipelinesReady = true;
        return true;
    }

    // A compute pipeline whose only root parameter is a constant buffer at b0.
    static bool CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* path,
        const char* entryPoint, ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = path;
        shaderDescription.entryPoint = entryPoint;
        shaderDescription.stage = ShaderStage::Compute;
        Shader* computeShader = shaderLibrary.Load(shaderDescription);
        ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = computeShader;
        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        pipelineDescription.rootParameters.push_back(constants);
        return computeShader && pipeline.Initialize(device, pipelineDescription);
    }

    static bool RecordDispatch(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
        const ClearConstants& constants, std::uint32_t groups)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(ClearConstants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            Log::Error("Failed to allocate D6 radiance cache clear constants.");
            return false;
        }

        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch(groups, 1, 1);
        return true;
    }

    template<size_t Count>
    static void RecordUAVBarriers(ID3D12GraphicsCommandList* commandList, Buffer* (&buffers)[Count])
    {
        D3D12_RESOURCE_BARRIER barriers[Count]{};

        for(size_t index = 0; index < Count; ++index)
        {
            barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[index].UAV.pResource = buffers[index]->GetResource();
        }

        commandList->ResourceBarrier(static_cast<UINT>(Count), barriers);
    }

    std::optional<Resources> m_resources;
    Settings m_settings;
    std::vector<RetiredResources> m_retired;
    ComputePipeline m_clearPipeline;
    ComputePipeline m_headerPipeline;
    ComputePipeline m_resolvePipeline;
    // Resolves performed (the published entries' frame clock for staleness); never reset, wraps at 2^32.
    std::uint32_t m_resolveFrame = 0;
    float m_spatialAnchor[3]{};
    // The learning binding of the current recording, valid after RecordLearningBegin.
    Binding m_learningBinding;
    std::uint32_t m_profilingThinning = 0;
    std::uint32_t m_profilingHarvestStop = 0;
    bool m_pipelinesReady = false;
    bool m_pipelinesFailed = false;
    // The epoch of the last submitted generation, and of one recorded in the current, not yet submitted, recording.
    std::optional<std::uint64_t> m_committedEpoch;
    std::optional<std::uint64_t> m_pendingEpoch;
    bool m_usable = false;
    Statistics m_statistics;

    // SHARC.4D-Q deferred insertion: the pipelines (created with the first record buffer), the record buffer, its
    // retired predecessors, and the stamp sequence of the last deferred learning frame (never reset, so a stamp is
    // only ever current in the frame that wrote it).
    Insertion m_insertion = Insertion::Inline;
    ComputePipeline m_insertPipeline;
    ComputePipeline m_clearStampsPipeline;
    bool m_insertPipelineReady = false;
    bool m_insertPipelineFailed = false;
    std::optional<Records> m_records;
    std::vector<RetiredRecords> m_retiredRecords;
    std::uint32_t m_recordSequence = 0;
    // The stamps must be cleared before the next deferred frame (a new buffer, a wrapped sequence, an uncertain
    // submission), and a clear recorded in the current, not yet submitted, recording.
    bool m_recordsNeedClear = false;
    bool m_recordsClearRecorded = false;
    // The record count whose allocation failed (0: none); not retried until the count changes or the cache retires.
    std::uint64_t m_recordsFailedCount = 0;
    bool m_skipDeferredInsertionForTesting = false;

    // SHARC.4D-R diagnostics: sparse training tile, query library and mode, and the query coverage counters.
    std::uint32_t m_trainingTile = 5;
    bool m_queryLibrary = false;
    std::uint32_t m_queryMode = 0;
    std::optional<Buffer> m_queryStatistics;
    // SHARC.6A history prior.
    bool m_historyPrior = false;
    std::uint32_t m_primaryRecordOffset = 0;
    std::uint32_t m_priorOutputIndex = 0;
    std::optional<Buffer> m_priorCounters;
    bool m_priorCountedThisFrame = false;
    float m_priorWeight = PriorWeight;
    // SHARC.4D-S diagnostics: the query-termination library, its threshold, and its counters.
    bool m_queryTerminationLibrary = false;
    float m_queryThreshold = 0.0f;
    bool m_queryCounters = false;
    // SHARC.4E: the cached-radiance render library, the user-visible training counter, and the statistics readback
    // (ReadbackSlots slots; whether a slot was ever recorded).
    bool m_queryRenderLibrary = false;
    std::uint64_t m_trainedFrames = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
    bool m_readbackRecorded[ReadbackSlots]{};
};
