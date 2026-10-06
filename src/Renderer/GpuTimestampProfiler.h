#pragma once

#include <d3d12.h>
#include <cstdint>


class GpuTimestampProfiler
{
    public:
        static constexpr unsigned int FrameCount = 2;

        enum class Region : unsigned int
        {
            TotalFrame,
            Geometry,
            RayTracedShadows,
            ShadowDenoise,
            RayTracedReflections,
            ReflectionContributionLimiter,
            NrdPrepare,
            NrdReblur,
            NrdResolve,
            DeferredLighting,
            ToneMap,

            Count
        };

        struct Results
        {
            uint64_t frameSerial = 0;
            double milliseconds[static_cast<unsigned int>(Region::Count)]{};
            uint32_t validRegions = 0;
        };

        bool Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue);
        void Shutdown();

        uint64_t GetTimestampFrequency() const;

        void BeginFrame(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex);
        void BeginRegion(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, Region region);
        void EndRegion(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, Region region);
        void EndFrameAndResolve(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex);

        void MarkSubmitted(unsigned int frameIndex, uint64_t submissionFence, uint64_t frameSerial);

        bool CollectCompleted(uint64_t completedFenceValue);

        const Results& GetResults() const;

    private:
        static unsigned int GetBeginQueryIndex(unsigned int frameIndex, Region region);
        static unsigned int GetEndQueryIndex(unsigned int frameIndex, Region region);

        static constexpr unsigned int RegionCount = static_cast<unsigned int>(Region::Count);

        static constexpr unsigned int QueriesPerRegion = 2;
        static constexpr unsigned int QueriesPerFrame = RegionCount * QueriesPerRegion;
        static constexpr unsigned int QueryCount = QueriesPerFrame * FrameCount;

        static constexpr uint64_t ReadbackBytesPerFrame = QueriesPerFrame * sizeof(uint64_t);
        static constexpr uint64_t ReadbackBufferSize = ReadbackBytesPerFrame * FrameCount;

        ID3D12QueryHeap* m_queryHeap = nullptr;
        ID3D12Resource* m_readbackBuffer = nullptr;

        uint64_t m_timestampFrequency = 0;

        struct FrameSlot
        {
            uint64_t submissionFence = 0;
            uint64_t frameSerial = 0;
            uint32_t recordedRegions = 0;
            bool pending = false;
        };

        FrameSlot m_frameSlots[FrameCount]{};
        Results m_results{};
};