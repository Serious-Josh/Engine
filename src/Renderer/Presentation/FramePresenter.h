#pragma once

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class SwapChain;

// FG.2: presents frames from a thread of its own on a dedicated present queue, so one rendered frame can be shown twice at
// controlled times: DLSS Frame Generation's generated frame (FG.3) as soon as the frame is done, and the real frame about
// half a real-frame interval later. The FG.2 diagnostics show the previous real frame as a stand-in generated frame.
//
// Why a queue of its own: a present is ordered behind earlier work on the queue that owns the swap chain. On the frame queue
// the delayed real present would wait behind the next frame's rendering; the present queue only ever holds small copies.
//
// Why the copies stay on the present queue (FG.4, measured on ManyLights 1024): they slow the frame queue's path tracing by
// about 0.6 ms per real frame (queue priority makes no difference), but only the queue that owns the swap chain may
// reference its buffers (D3D12 error #907, EXECUTECOMMANDLISTS_WRONGSWAPCHAINBUFFERREFERENCE): neither a copy queue nor the
// frame queue can write them. A copy-queue version was tried and rejected by the debug layer.
//
// Ownership and synchronization:
//  - The render thread renders each frame's final images into composites from a ring (AcquireComposite), then hands them
//    over (Submit). While started, the present thread alone uses the swap chain, which must have been created on GetQueue().
//  - A composite is written on the frame queue (left in COPY_SOURCE) and read by copies on the present queue. AcquireComposite
//    waits on the CPU until every copy reading the composite has completed, so the frame queue never waits on the present
//    queue, and the present thread waits only on GPU fences, never on the render thread.
//  - Stop presents everything already submitted, joins the thread and leaves the present queue idle.
class FramePresenter
{
public:
    // Three frames of two composites each (FG.3: the real frame's and the generated frame's).
    static constexpr unsigned int CompositeCount = 6;

    // Present spacing over the last IntervalWindow presents (CPU time at each Present call), for verification and logs.
    struct Statistics
    {
        std::uint64_t presents = 0;
        std::uint64_t generatedPresents = 0;
        // Intervals in the window, in milliseconds: generated -> real, real -> next generated, and real -> real (no generated
        // frame between, e.g. right after Start).
        std::uint32_t intervals = 0;
        double meanGeneratedToReal = 0.0;
        double meanRealToGenerated = 0.0;
        std::uint32_t realToReal = 0;
        double meanInterval = 0.0;
        double standardDeviation = 0.0;
        double maximumInterval = 0.0;
        // The smoothed real-frame interval pacing aims at half of (0: pacing restarted).
        double realFrameInterval = 0.0;
    };

    static constexpr unsigned int IntervalWindow = 240;

    struct Composite
    {
        unsigned int index = 0;
        ID3D12Resource* resource = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    };

    FramePresenter() = default;
    FramePresenter(const FramePresenter&) = delete;
    FramePresenter& operator=(const FramePresenter&) = delete;
    ~FramePresenter();

    // The present queue and its fixed objects, once per device. False leaves nothing created.
    bool Initialize(ID3D12Device* device, std::string& failure);
    bool IsInitialized() const noexcept
    {
        return m_queue != nullptr;
    }

    // Stops if started, then releases everything. Only after the GPU is idle on the frame queue.
    void Shutdown();

    ID3D12CommandQueue* GetQueue() const noexcept
    {
        return m_queue.Get();
    }

    // Creates the composites at the swap chain's extent and format and starts the present thread. frameFence is the frame
    // queue's fence whose values Submit receives.
    bool Start(SwapChain& swapChain, ID3D12Fence* frameFence, unsigned int width, unsigned int height, DXGI_FORMAT format,
        std::string& failure);
    void Stop();

    bool IsStarted() const noexcept
    {
        return m_thread.joinable();
    }

    // The next composite in the ring, once no copy reads it any more. False after a failure (the present thread stopped).
    bool AcquireComposite(Composite& composite);

    // The frame recording that rendered `composite` (and `generatedComposite`, when it has one) was submitted and signals
    // frameFenceValue. The generated frame is shown first, the real frame half a real-frame interval later; without one,
    // only the real frame is shown.
    void Submit(unsigned int composite, std::uint64_t frameFenceValue, std::optional<unsigned int> generatedComposite);

    // FG.2 diagnostics: the real composite of the last Submit since Start, which the stand-in shows again as the next
    // frame's generated frame (none right after Start).
    std::optional<unsigned int> GetLastSubmittedComposite() const noexcept
    {
        return m_lastSubmittedComposite;
    }

    bool HasFailed() const;
    std::string GetFailure() const;
    Statistics GetStatistics() const;

private:
    struct Request
    {
        unsigned int composite = 0;
        std::uint64_t frameFenceValue = 0;
        std::optional<unsigned int> generatedComposite;
    };

    using Clock = std::chrono::steady_clock;

    void Run();
    bool WaitForFrame(std::uint64_t frameFenceValue);
    // Sleeps until target, returning early once the next request's frame is complete (its generated frame is due now).
    void WaitUntilOrNextFrame(Clock::time_point target);
    bool PresentComposite(unsigned int composite, bool generated);
    void Fail(std::string failure);
    void RecordPresent(Clock::time_point time, bool generated);
    void WaitForPresentFence(std::uint64_t value, void* event) const;

    // Fixed objects (Initialize/Shutdown).
    ID3D12Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_presentFence;
    std::uint64_t m_presentFenceValue = 0;
    static constexpr unsigned int ListCount = 4;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, ListCount> m_allocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, ListCount> m_lists;
    std::array<std::uint64_t, ListCount> m_listFenceValues{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    unsigned int m_rtvDescriptorSize = 0;
    // Present thread: frame and present fence waits. Render thread: AcquireComposite's present fence wait.
    void* m_presentThreadEvent = nullptr;
    void* m_renderThreadEvent = nullptr;
    void* m_timer = nullptr;

    // Per Start.
    SwapChain* m_swapChain = nullptr;
    ID3D12Fence* m_frameFence = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, CompositeCount> m_composites;
    std::thread m_thread;
    unsigned int m_nextComposite = 0;
    std::optional<unsigned int> m_lastSubmittedComposite;

    // Shared between the threads (m_mutex).
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Request> m_requests;
    bool m_stopping = false;
    std::string m_failure;
    // Copies queued or recording that read each composite, and the present fence value after the last one submitted.
    std::array<unsigned int, CompositeCount> m_pendingReads{};
    std::array<std::uint64_t, CompositeCount> m_lastReadFenceValue{};

    // Present thread pacing state (m_pacedInterval is published as m_realFrameIntervalMilliseconds under m_mutex).
    Clock::time_point m_lastFrameReady{};
    bool m_hasLastFrameReady = false;
    double m_pacedInterval = 0.0;
    double m_realFrameIntervalMilliseconds = 0.0;

    // Present spacing, written by the present thread under m_mutex.
    enum class IntervalKind : std::uint8_t
    {
        RealToGenerated,
        GeneratedToReal,
        RealToReal
    };

    std::uint64_t m_presents = 0;
    std::uint64_t m_generatedPresents = 0;
    Clock::time_point m_lastPresent{};
    bool m_hasLastPresent = false;
    bool m_lastPresentGenerated = false;
    std::array<double, IntervalWindow> m_intervals{};
    std::array<IntervalKind, IntervalWindow> m_intervalKinds{};
    std::uint32_t m_intervalCount = 0;
    std::uint32_t m_nextInterval = 0;
};
