#include "Renderer/Presentation/FramePresenter.h"

#include "Core/Log.h"
#include "Renderer/SwapChain.h"
#include "Renderer/TextureMemoryAllocator.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    // Real-frame intervals above this are pauses (loading, a stall, a minimized window): pacing restarts after them.
    constexpr double kMaximumPacedIntervalMilliseconds = 200.0;
    // Smoothing of the real-frame interval: follows a change within a few frames, steady against single-frame noise.
    constexpr double kIntervalSmoothing = 0.2;
    // Sleep step of the pacing wait, in 100 ns units (0.5 ms), the high-resolution timer's practical granularity, and the
    // final stretch (0.6 ms) spent yielding instead, so the real frame is presented on time rather than up to a step late.
    constexpr LONGLONG kPacingSleepHundredNanoseconds = 5000;
    constexpr LONGLONG kPacingSpinHundredNanoseconds = 6000;
    // A frame that has not completed after this long is a hung GPU, not a slow frame.
    constexpr DWORD kFrameWaitMilliseconds = 10000;

    double Milliseconds(std::chrono::steady_clock::duration duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }
}

FramePresenter::~FramePresenter()
{
    Shutdown();
}

bool FramePresenter::Initialize(ID3D12Device* device, std::string& failure)
{
    if(IsInitialized())
    {
        return true;
    }

    if(!device)
    {
        failure = "no D3D12 device";
        return false;
    }

    // Stage 1: the present queue. High priority, so the short copies before each present wait as little as the hardware allows
    // behind the frame queue's long dispatches.
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;

    if(FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))))
    {
        failure = "the present queue could not be created";
        return false;
    }

    queue->SetName(L"Frame Generation present queue");
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;

    if(FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        failure = "the present fence could not be created";
        return false;
    }

    // Stage 2: one allocator and list per in-flight copy, and the composites' render-target views.
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, ListCount> allocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, ListCount> lists;

    for(unsigned int index = 0; index < ListCount; ++index)
    {
        const bool created =
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[index]))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[index].Get(), nullptr,
                IID_PPV_ARGS(&lists[index]))) &&
            SUCCEEDED(lists[index]->Close());

        if(!created)
        {
            failure = "the present command lists could not be created";
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors = CompositeCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;

    if(FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&rtvHeap))))
    {
        failure = "the composite RTV heap could not be created";
        return false;
    }

    // Stage 3: events for fence waits on each thread, and the pacing timer (without it, the wait falls back to sleep_for).
    void* const presentThreadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void* const renderThreadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if(!presentThreadEvent || !renderThreadEvent)
    {
        if(presentThreadEvent)
        {
            CloseHandle(presentThreadEvent);
        }

        if(renderThreadEvent)
        {
            CloseHandle(renderThreadEvent);
        }

        failure = "the present fence events could not be created";
        return false;
    }

    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    m_device = device;
    m_queue = std::move(queue);
    m_presentFence = std::move(fence);
    m_presentFenceValue = 0;
    m_allocators = std::move(allocators);
    m_lists = std::move(lists);
    m_listFenceValues = {};
    m_rtvHeap = std::move(rtvHeap);
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_presentThreadEvent = presentThreadEvent;
    m_renderThreadEvent = renderThreadEvent;
    return true;
}

void FramePresenter::Shutdown()
{
    Stop();

    for(void** handle : {&m_presentThreadEvent, &m_renderThreadEvent, &m_timer})
    {
        if(*handle)
        {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    }

    m_lists = {};
    m_allocators = {};
    m_rtvHeap.Reset();
    m_presentFence.Reset();
    m_queue.Reset();
    m_device = nullptr;
}

bool FramePresenter::Start(SwapChain& swapChain, ID3D12Fence* frameFence, unsigned int width, unsigned int height,
    DXGI_FORMAT format, std::string& failure)
{
    if(!IsInitialized() || IsStarted() || !frameFence || width == 0 || height == 0)
    {
        failure = "the present path is not initialized, already started, or has no extent";
        return false;
    }

    // Stage 1: the composites, in COPY_SOURCE: the state they wait in between a frame's rendering and the present copies.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for(unsigned int index = 0; index < CompositeCount; ++index)
    {
        // Engine-owned memory when created on the renderer's thread, which the allocator activates on the frame recording
        // before the first clear (TextureMemoryAllocator.h); Stop drains the present queue before releasing them.
        const bool placed = SUCCEEDED(TextureMemoryAllocator::CreateTexture(m_device, description,
            D3D12_RESOURCE_STATE_COPY_SOURCE, m_composites[index].ReleaseAndGetAddressOf()));

        if(!placed && FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_composites[index]))))
        {
            m_composites = {};
            failure = "the present composites could not be created";
            return false;
        }

        const std::wstring name = L"Frame Generation present composite " + std::to_wstring(index);
        m_composites[index]->SetName(name.c_str());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(m_composites[index].Get(), nullptr, rtv);
    }

    // Stage 2: a fresh run (nothing pending, pacing restarted), then the thread.
    m_swapChain = &swapChain;
    m_frameFence = frameFence;
    m_nextComposite = 0;
    m_lastSubmittedComposite.reset();
    m_hasLastFrameReady = false;
    m_pacedInterval = 0.0;

    {
        const std::lock_guard lock(m_mutex);
        m_realFrameIntervalMilliseconds = 0.0;
        m_requests.clear();
        m_stopping = false;
        m_failure.clear();
        m_pendingReads = {};
        m_lastReadFenceValue = {};
        m_presents = 0;
        m_generatedPresents = 0;
        m_hasLastPresent = false;
        m_intervalCount = 0;
        m_nextInterval = 0;
    }

    m_thread = std::thread([this]()
    {
        Run();
    });

    Log::Info("Frame Generation present path started: " + std::to_string(width) + "x" + std::to_string(height) + ", " +
        std::to_string(CompositeCount) + " composites.");
    return true;
}

void FramePresenter::Stop()
{
    if(IsStarted())
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }

        m_changed.notify_all();
        m_thread.join();

        // The present queue idle: nothing reads the composites or the swap chain's buffers any more.
        const std::uint64_t value = ++m_presentFenceValue;

        if(SUCCEEDED(m_queue->Signal(m_presentFence.Get(), value)))
        {
            WaitForPresentFence(value, m_renderThreadEvent);
        }

        const Statistics statistics = GetStatistics();
        Log::Info("Frame Generation present path stopped after " + std::to_string(statistics.presents) + " presents (" +
            std::to_string(statistics.generatedPresents) + " generated); last window: generated->real " +
            std::to_string(statistics.meanGeneratedToReal) + " ms, real->generated " +
            std::to_string(statistics.meanRealToGenerated) + " ms, deviation " +
            std::to_string(statistics.standardDeviation) + " ms.");
    }

    m_composites = {};
    m_swapChain = nullptr;
    m_frameFence = nullptr;
}

bool FramePresenter::AcquireComposite(Composite& composite)
{
    if(!IsStarted())
    {
        return false;
    }

    const unsigned int index = m_nextComposite;
    std::uint64_t lastRead = 0;

    {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&]()
        {
            return m_pendingReads[index] == 0 || !m_failure.empty();
        });

        if(!m_failure.empty())
        {
            return false;
        }

        lastRead = m_lastReadFenceValue[index];
    }

    // Every copy reading it is submitted; its GPU completion is what the frame queue must not overtake.
    WaitForPresentFence(lastRead, m_renderThreadEvent);
    m_nextComposite = (index + 1) % CompositeCount;

    composite.index = index;
    composite.resource = m_composites[index].Get();
    composite.rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    composite.rtv.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
    return true;
}

void FramePresenter::Submit(unsigned int composite, std::uint64_t frameFenceValue,
    std::optional<unsigned int> generatedComposite)
{
    const std::optional<unsigned int> generated = generatedComposite;

    {
        const std::lock_guard lock(m_mutex);
        ++m_pendingReads[composite];

        if(generated)
        {
            ++m_pendingReads[*generated];
        }

        m_requests.push_back({composite, frameFenceValue, generated});
    }

    m_changed.notify_all();
    m_lastSubmittedComposite = composite;
}

bool FramePresenter::HasFailed() const
{
    const std::lock_guard lock(m_mutex);
    return !m_failure.empty();
}

std::string FramePresenter::GetFailure() const
{
    const std::lock_guard lock(m_mutex);
    return m_failure;
}

FramePresenter::Statistics FramePresenter::GetStatistics() const
{
    const std::lock_guard lock(m_mutex);
    Statistics statistics;
    statistics.presents = m_presents;
    statistics.generatedPresents = m_generatedPresents;
    statistics.intervals = m_intervalCount;
    statistics.realFrameInterval = m_realFrameIntervalMilliseconds;

    double sum = 0.0;
    double generatedToRealSum = 0.0;
    double realToGeneratedSum = 0.0;
    std::uint32_t generatedToReal = 0;
    std::uint32_t realToGenerated = 0;

    for(std::uint32_t index = 0; index < m_intervalCount; ++index)
    {
        const double interval = m_intervals[index];
        sum += interval;
        statistics.maximumInterval = (std::max)(statistics.maximumInterval, interval);

        switch(m_intervalKinds[index])
        {
        case IntervalKind::GeneratedToReal:
            generatedToRealSum += interval;
            ++generatedToReal;
            break;
        case IntervalKind::RealToGenerated:
            realToGeneratedSum += interval;
            ++realToGenerated;
            break;
        case IntervalKind::RealToReal:
            ++statistics.realToReal;
            break;
        }
    }

    if(m_intervalCount == 0)
    {
        return statistics;
    }

    statistics.meanInterval = sum / m_intervalCount;
    statistics.meanGeneratedToReal = generatedToReal > 0 ? generatedToRealSum / generatedToReal : 0.0;
    statistics.meanRealToGenerated = realToGenerated > 0 ? realToGeneratedSum / realToGenerated : 0.0;
    double squares = 0.0;

    for(std::uint32_t index = 0; index < m_intervalCount; ++index)
    {
        const double difference = m_intervals[index] - statistics.meanInterval;
        squares += difference * difference;
    }

    statistics.standardDeviation = std::sqrt(squares / m_intervalCount);
    return statistics;
}

void FramePresenter::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Frame Generation present");

    while(true)
    {
        Request request;

        {
            std::unique_lock lock(m_mutex);
            m_changed.wait(lock, [&]()
            {
                return !m_requests.empty() || m_stopping;
            });

            // Stopping presents everything already submitted first.
            if(m_requests.empty())
            {
                return;
            }

            request = m_requests.front();
            m_requests.pop_front();
        }

        if(!WaitForFrame(request.frameFenceValue))
        {
            Fail("frame fence value " + std::to_string(request.frameFenceValue) + " did not complete in " +
                std::to_string(kFrameWaitMilliseconds) + " ms");
            return;
        }

        // Pacing: the smoothed interval between frames becoming ready, restarted after a pause.
        const Clock::time_point ready = Clock::now();
        double realFrameInterval = m_pacedInterval;

        if(m_hasLastFrameReady)
        {
            const double interval = Milliseconds(ready - m_lastFrameReady);

            if(interval > kMaximumPacedIntervalMilliseconds)
            {
                realFrameInterval = 0.0;
            }
            else if(realFrameInterval == 0.0)
            {
                realFrameInterval = interval;
            }
            else
            {
                realFrameInterval += kIntervalSmoothing * (interval - realFrameInterval);
            }
        }

        m_lastFrameReady = ready;
        m_hasLastFrameReady = true;
        m_pacedInterval = realFrameInterval;

        {
            const std::lock_guard lock(m_mutex);
            m_realFrameIntervalMilliseconds = realFrameInterval;
        }

        // The generated frame goes as soon as the frame is ready; the real frame half a real-frame interval later.
        if(request.generatedComposite)
        {
            if(!PresentComposite(*request.generatedComposite, true))
            {
                return;
            }

            const auto halfInterval = std::chrono::duration<double, std::milli>(realFrameInterval * 0.5);
            WaitUntilOrNextFrame(ready + std::chrono::duration_cast<Clock::duration>(halfInterval));
        }

        if(!PresentComposite(request.composite, false))
        {
            return;
        }
    }
}

bool FramePresenter::WaitForFrame(std::uint64_t frameFenceValue)
{
    if(m_frameFence->GetCompletedValue() >= frameFenceValue)
    {
        return true;
    }

    if(FAILED(m_frameFence->SetEventOnCompletion(frameFenceValue, m_presentThreadEvent)))
    {
        return false;
    }

    return WaitForSingleObject(m_presentThreadEvent, kFrameWaitMilliseconds) == WAIT_OBJECT_0;
}

void FramePresenter::WaitUntilOrNextFrame(Clock::time_point target)
{
    while(Clock::now() < target)
    {
        {
            const std::lock_guard lock(m_mutex);

            // The next frame is already done: its generated frame is due, so this real frame goes now.
            const bool nextFrameReady = !m_requests.empty() &&
                m_frameFence->GetCompletedValue() >= m_requests.front().frameFenceValue;

            if(nextFrameReady || m_stopping)
            {
                return;
            }
        }

        // Sleep in steps until the last fraction of a millisecond, then yield: the timer wakes late by up to a step.
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(target - Clock::now()).count();
        const LONGLONG remainingHundredNanoseconds = static_cast<LONGLONG>(remaining / 100);

        if(remainingHundredNanoseconds <= kPacingSpinHundredNanoseconds)
        {
            std::this_thread::yield();
            continue;
        }

        const LONGLONG step = (std::min)(kPacingSleepHundredNanoseconds,
            remainingHundredNanoseconds - kPacingSpinHundredNanoseconds);

        if(m_timer)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -step;
            SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(m_timer, INFINITE);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::nanoseconds(step * 100));
        }
    }
}

bool FramePresenter::PresentComposite(unsigned int composite, bool generated)
{
    // Stage 1: a list whose previous copy has completed.
    const unsigned int listIndex = static_cast<unsigned int>(m_presents % ListCount);
    WaitForPresentFence(m_listFenceValues[listIndex], m_presentThreadEvent);
    ID3D12CommandAllocator* const allocator = m_allocators[listIndex].Get();
    ID3D12GraphicsCommandList* const list = m_lists[listIndex].Get();

    if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr)))
    {
        Fail("a present command list could not be reset");
        return false;
    }

    // Stage 2: the composite into the current back buffer, which waits in PRESENT between presents.
    ID3D12Resource* const backBuffer = m_swapChain->GetCurrentBackBuffer();
    const D3D12_RESOURCE_BARRIER toCopy = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);
    list->CopyResource(backBuffer, m_composites[composite].Get());
    const D3D12_RESOURCE_BARRIER toPresent =
        Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &toPresent);

    if(FAILED(list->Close()))
    {
        Fail("a present command list could not be closed");
        return false;
    }

    ID3D12CommandList* const lists[] = {list};
    m_queue->ExecuteCommandLists(1, lists);

    // Stage 3: present, then the fence that tells the render thread the composite is no longer read.
    const bool presented = m_swapChain->Present();
    const Clock::time_point presentTime = Clock::now();
    const std::uint64_t value = ++m_presentFenceValue;

    if(FAILED(m_queue->Signal(m_presentFence.Get(), value)))
    {
        // Never published: a wait for this value would never end.
        Fail("the present fence could not be signalled");
        return false;
    }

    m_listFenceValues[listIndex] = value;

    {
        const std::lock_guard lock(m_mutex);
        m_lastReadFenceValue[composite] = value;
        --m_pendingReads[composite];
        RecordPresent(presentTime, generated);
    }

    m_changed.notify_all();

    if(!presented)
    {
        Fail("Present failed");
        return false;
    }

    return true;
}

void FramePresenter::Fail(std::string failure)
{
    {
        const std::lock_guard lock(m_mutex);

        if(m_failure.empty())
        {
            m_failure = failure;
        }
    }

    m_changed.notify_all();
    Log::Error("Frame Generation present path: " + failure + ".");
}

void FramePresenter::RecordPresent(Clock::time_point time, bool generated)
{
    // Called under m_mutex.
    ++m_presents;

    if(generated)
    {
        ++m_generatedPresents;
    }

    if(m_hasLastPresent)
    {
        IntervalKind kind = IntervalKind::RealToReal;

        if(generated)
        {
            kind = IntervalKind::RealToGenerated;
        }
        else if(m_lastPresentGenerated)
        {
            kind = IntervalKind::GeneratedToReal;
        }

        m_intervals[m_nextInterval] = Milliseconds(time - m_lastPresent);
        m_intervalKinds[m_nextInterval] = kind;
        m_nextInterval = (m_nextInterval + 1) % IntervalWindow;
        m_intervalCount = (std::min)(m_intervalCount + 1, IntervalWindow);
    }

    m_lastPresent = time;
    m_hasLastPresent = true;
    m_lastPresentGenerated = generated;
}

void FramePresenter::WaitForPresentFence(std::uint64_t value, void* event) const
{
    if(value == 0 || m_presentFence->GetCompletedValue() >= value)
    {
        return;
    }

    if(SUCCEEDED(m_presentFence->SetEventOnCompletion(value, event)))
    {
        WaitForSingleObject(event, INFINITE);
    }
}
