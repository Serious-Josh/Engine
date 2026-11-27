#include "CommandContext.h"
#include "Debug/TransitionTiming.h"
#include "Debug/CpuFrameTiming.h"
#include "Debug/StreamingTelemetry.h"
#include "Core/Log.h"
#include "ResourceStateTransaction.h"
#include "TextureMemoryAllocator.h"
#ifdef _DEBUG
#include "GraphicsDevice.h"
#endif

#include <wrl/client.h>

#include <string>
#include <atomic>
#include <cstdlib>

namespace
{
    uint64_t AllocateSubmissionLifetime()
    {
        // Unique across contexts/reinitialization; never wrap and reuse a tag.
        static std::atomic<uint64_t> next{1};
        uint64_t value = next.load(std::memory_order_relaxed);
        while (value != UINT64_MAX)
        {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            {
                return value;
            }
        }

        return 0;
    }

    bool SameSubmissionToken(
        SubmissionFenceToken left,
        SubmissionFenceToken right)
    {
        return left.fenceValue == right.fenceValue && left.lifetime == right.lifetime &&
               left.retentionID == right.retentionID;
    }
} // namespace

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

bool CommandContext::Initialize(ID3D12Device* device)
{
    if (HasSubmissionReservations())
    {
        Log::Error("Cannot initialize command context with unresolved submission reservations.");
        return false;
    }

    // Even a failed initialization attempt invalidates earlier provenance.

    m_submissionLifetime = 0;
    m_lastSubmissionFence = 0;
    m_retainedSubmissionFences = {};

    if (!device)
    {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};

    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;

    HRESULT result = device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue));

    if (FAILED(result))
    {
        return false;
    }

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));

        if (FAILED(result))
        {
            return false;
        }
    }

    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0], nullptr,
                                       IID_PPV_ARGS(&m_commandList));

    if (FAILED(result))
    {
        return false;
    }

    // Debug name, so validation messages identify the frame recording.
    m_commandList->SetName(L"Frame command list");

    result = m_commandList->QueryInterface(IID_PPV_ARGS(&m_rayTracingCommandList));

    if (FAILED(result))
    {
        return false;
    }

    result = m_commandList->Close();

    if (FAILED(result))
    {
        return false;
    }

    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    if (FAILED(result))
    {
        return false;
    }

    m_fenceValue = 1;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (m_fenceEvent == nullptr)
    {
        return false;
    }

    m_submissionLifetime = AllocateSubmissionLifetime();

    if (m_submissionLifetime == 0)
    {
        Log::Error("Submission fence lifetime tags exhausted.");

        return false;
    }

    m_lastSubmissionFence = 0;
    m_nextRetentionID = 1;
    m_retainedSubmissionFences = {};
    InitializeAsyncCompute(device);
    return true;
}

void CommandContext::InitializeAsyncCompute(ID3D12Device* device)
{
    ShutdownAsyncCompute();
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    bool created = SUCCEEDED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&m_computeQueue)));

    for (unsigned int i = 0; created && i < FrameCount; ++i)
    {
        created = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_computeAllocators[i])));
    }

    created = created && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_computeAllocators[0], nullptr,
        IID_PPV_ARGS(&m_computeList))) && SUCCEEDED(m_computeList->Close());
    created = created && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_computeFence)));
    created = created && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_directListFence)));
    // The second direct list of a split recording (SplitForAsyncComputeReaders), closed until a split resets it.
    created = created && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0], nullptr,
        IID_PPV_ARGS(&m_splitSecondList))) && SUCCEEDED(m_splitSecondList->Close()) &&
        SUCCEEDED(m_splitSecondList->QueryInterface(IID_PPV_ARGS(&m_splitSecondRayTracingList)));

    if (!created)
    {
        Log::Warning("AIR async compute: no compute queue (creation failed); AIR stays on the frame queue.");
        ShutdownAsyncCompute();
        return;
    }

    m_computeQueue->SetName(L"Async compute queue (AIR)");
    m_computeList->SetName(L"Async compute command list (AIR)");
    m_splitSecondList->SetName(L"Frame command list (after AIR's compute work)");
}

void CommandContext::ShutdownAsyncCompute()
{
    const auto release = [](auto*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    };

    EndSplit(true);
    release(m_computeList);
    release(m_splitSecondRayTracingList);
    release(m_splitSecondList);

    for (auto*& allocator : m_computeAllocators)
    {
        release(allocator);
    }

    release(m_computeFence);
    release(m_directListFence);
    release(m_computeQueue);
    m_computeFenceValue = 0;
    m_directListFenceValue = 0;
    m_asyncComputeOpen = false;
    m_asyncComputeAfterFrame = false;
}

ID3D12GraphicsCommandList* CommandContext::BeginAsyncCompute(unsigned int frameIndex)
{
    if (!m_computeQueue || frameIndex >= FrameCount || ResourceStateTransaction::RecordingFor(m_commandList) == 0)
    {
        return nullptr;
    }

    if (m_asyncComputeOpen)
    {
        return m_computeList;
    }

    // Begin waited for this slot's frame fence, and the frame queue waits for each frame's compute work before signalling it.
    if (FAILED(m_computeAllocators[frameIndex]->Reset()) || FAILED(m_computeList->Reset(m_computeAllocators[frameIndex], nullptr)))
    {
        Log::Error("AIR async compute: the compute list could not be reset; AIR records on the frame list.");
        return nullptr;
    }

    m_asyncComputeOpen = true;
    m_asyncComputeAfterFrame = false;
    ResourceStateTransaction::AttachRecordingList(m_computeList);
    return m_computeList;
}

ID3D12GraphicsCommandList* CommandContext::SplitForAsyncComputeReaders(unsigned int frameIndex)
{
    if (!m_asyncComputeOpen || m_splitFirstList != nullptr || m_splitSecondList == nullptr || frameIndex >= FrameCount ||
        ResourceStateTransaction::RecordingFor(m_commandList) == 0)
    {
        return nullptr;
    }

    if (FAILED(m_commandList->Close()))
    {
        Log::Error("AIR async compute: the frame list could not be closed at AIR's first reader.");
        return nullptr;
    }

    // From here the recording is split, whether or not the second list opens: abort and discard then close only what is open.
    m_splitFirstList = m_commandList;
    m_splitFirstRayTracingList = m_rayTracingCommandList;
    m_commandList = m_splitSecondList;
    m_rayTracingCommandList = m_splitSecondRayTracingList;

    // The first part is closed, so the slot's allocator is free for the second (Begin waited for the slot's previous frame).
    if (FAILED(m_splitSecondList->Reset(m_commandAllocators[frameIndex], nullptr)))
    {
        Log::Error("AIR async compute: the second frame list could not be reset; the frame is abandoned.");
        m_splitSecondOpen = false;
        return nullptr;
    }

    m_splitSecondOpen = true;
    ResourceStateTransaction::SwitchRecordingList(m_commandList, m_rayTracingCommandList);
    TextureMemoryAllocator::SwitchFrameRecordingList(m_commandList);
    return m_commandList;
}

void CommandContext::EndSplit(bool closeSecond)
{
    if (m_splitFirstList == nullptr)
    {
        return;
    }

    if (closeSecond && m_splitSecondOpen)
    {
        m_commandList->Close();
    }

    m_splitSecondOpen = false;
    m_splitSecondEnded = false;
    m_commandList = m_splitFirstList;
    m_rayTracingCommandList = m_splitFirstRayTracingList;
    m_splitFirstList = nullptr;
    m_splitFirstRayTracingList = nullptr;
}

void CommandContext::AbandonAsyncCompute()
{
    if (m_asyncComputeOpen)
    {
        m_computeList->Close();
        m_asyncComputeOpen = false;
        m_asyncComputeAfterFrame = false;
    }
}

void CommandContext::Shutdown()
{
    if (HasSubmissionReservations())
    {
        Log::Error("Command context shutdown: unresolved submission reservation; aborting without teardown.");
        std::abort();
    }

    // A split recording's first list is the frame list again (released below; the second list goes with the compute queue).
    EndSplit(true);

    // Invalidate provenance even if clients have not released their tokens.
    // Resource completion/teardown remains the caller's responsibility.

    m_submissionLifetime = 0;
    m_lastSubmissionFence = 0;
    m_retainedSubmissionFences = {};

    if (m_fence)
    {
        m_fence->Release();
        m_fence = nullptr;
    }

    if (m_fenceEvent)
    {
        CloseHandle(static_cast<HANDLE>(m_fenceEvent));

        m_fenceEvent = nullptr;
    }

    if (m_rayTracingCommandList)
    {
        m_rayTracingCommandList->Release();
        m_rayTracingCommandList = nullptr;
    }

    if (m_commandList)
    {
        m_commandList->Release();
        m_commandList = nullptr;
    }

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        if (m_commandAllocators[i])
        {
            m_commandAllocators[i]->Release();

            m_commandAllocators[i] = nullptr;
        }
    }

    // RR.1A: the lifecycle list. Its submissions signal the frame fence, so the caller's drain covers them.
    if (m_lifecycleList)
    {
        m_lifecycleList->Release();
        m_lifecycleList = nullptr;
    }

    if (m_lifecycleAllocator)
    {
        m_lifecycleAllocator->Release();
        m_lifecycleAllocator = nullptr;
    }

    m_lifecycleOpen = false;
    m_lifecycleFenceValue = 0;
    m_lifecycleUncertain = false;

    ShutdownAsyncCompute();

    if (m_commandQueue)
    {
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    m_fenceValue = 0;

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        m_frameFenceValues[i] = 0;
    }
}

// Blocks until the GPU has finished the last submission that used this frame
// slot's allocator. Shared by Begin and by callers that record on that allocator
// before the frame does; failures and device removal are reported here.
bool CommandContext::WaitForFrameFence(unsigned int frameIndex)
{
    TransitionTiming::Scope er1Scope("GPU WaitForFrameFence", 1.0);
    const StreamingTelemetry::Scope streamingTiming(StreamingTelemetry::Metric::FrameFenceWaitNs,
        StreamingTelemetry::Metric::FrameFenceWaitCalls);
    if (!m_fence || frameIndex >= FrameCount)
    {
        return false;
    }

    const unsigned long long frameFenceValue = m_frameFenceValues[frameIndex];

    const auto deviceRemoved = [&]()
    {
        ID3D12Device* device = nullptr;
        const HRESULT result = m_fence->GetDevice(IID_PPV_ARGS(&device));

        if (FAILED(result))
        {
            Log::Error("Frame fence reports device removal; GetDevice HRESULT: " + std::to_string(result));
        }
        else
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            Log::Error("Frame fence reports device removal; removal HRESULT: " + std::to_string(reason));
#ifdef _DEBUG
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::Begin frame fence");
#endif
            device->Release();
        }
    };

    unsigned long long completedValue = m_fence->GetCompletedValue();

    if (completedValue == UINT64_MAX)
    {
        deviceRemoved();
        return false;
    }

    if (frameFenceValue != 0 && completedValue < frameFenceValue)
    {
        HRESULT result = m_fence->SetEventOnCompletion(frameFenceValue, static_cast<HANDLE>(m_fenceEvent));

        if (FAILED(result))
        {
            Log::Error("Failed to register frame fence event. HRESULT: " + std::to_string(result));

            return false;
        }

        m_beginWaitedForFence = true;

        const auto waitStart = CpuTiming::Clock::now();
        const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
        m_beginFenceWaitMilliseconds = CpuTiming::MillisecondsSince(waitStart);

        if (waitResult == WAIT_FAILED)
        {
            const DWORD error = GetLastError();
            Log::Error("Frame fence wait failed. Win32 error: " + std::to_string(error));
            return false;
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            Log::Error("Unexpected frame fence wait result: " + std::to_string(waitResult));
            return false;
        }

        completedValue = m_fence->GetCompletedValue();

        if (completedValue == UINT64_MAX)
        {
            deviceRemoved();
            return false;
        }

        if (completedValue < frameFenceValue)
        {
            Log::Error("Frame fence has not completed. Requested: " + std::to_string(frameFenceValue) +

                       "; completed: " + std::to_string(completedValue));
            return false;
        }
    }

    return true;
}

bool CommandContext::Begin(unsigned int frameIndex)
{
    for (ReservationState state : m_reservationStates)
    {
        if (state == ReservationState::SubmissionUncertain)
        {
            Log::Error("Cannot reset command recording after uncertain reserved submission.");

            return false;
        }
    }

    m_beginWaitedForFence = false;
    m_beginFenceWaitMilliseconds = 0.0;
    m_beginResetMilliseconds = 0.0;

    if (frameIndex >= FrameCount)
    {
        return false;
    }

    if (!WaitForFrameFence(frameIndex))
    {
        return false;
    }

    // AIR async compute: a compute list left open by a recording that was never executed is never submitted, nor a split
    // recording's parts.
    AbandonAsyncCompute();
    EndSplit(true);

    ID3D12CommandAllocator* commandAllocator = m_commandAllocators[frameIndex];

    CpuTiming::ScopedDuration resetDuration(m_beginResetMilliseconds);

    HRESULT result = commandAllocator->Reset();

    if (FAILED(result))
    {
        return false;
    }

    result = m_commandList->Reset(commandAllocator, nullptr);

    if (FAILED(result))
    {
        return false;
    }

    // R.1: the tracked resource states this recording changes stay provisional until Execute submits it.
    ResourceStateTransaction::BeginRecording(m_commandList, m_rayTracingCommandList);
    // Placed render-thread targets created since the last recording (or whose activation was discarded) are activated
    // first; the ones created while this recording is open, at once (TextureMemoryAllocator.h).
    TextureMemoryAllocator::BeginFrameRecording(m_commandList);
    return true;
}

bool CommandContext::End()
{
    TextureMemoryAllocator::EndFrameRecording();

    // A split recording ends with its second list (the first part is closed already).
    if (m_splitFirstList != nullptr)
    {
        const bool closed = m_splitSecondOpen && SUCCEEDED(m_commandList->Close());
        m_splitSecondOpen = false;
        m_splitSecondEnded = true;
        return closed;
    }

    return SUCCEEDED(m_commandList->Close());
}

bool CommandContext::AbortRecording()
{
    if (!m_commandList)
    {
        return false;
    }

    // Close the command list without submitting it.
    // The next Begin() will reset it. R.1: its barriers never run, so the tracked states return to the submitted ones, and
    // the texture activations it held are recorded again by the next Begin.
    AbandonAsyncCompute();
    ResourceStateTransaction::DiscardRecording();
    TextureMemoryAllocator::EndFrameRecording();

    // A split recording: nothing of either part was submitted (Execute submits both); the first part is closed already, the
    // second is closed if still open (or if End closed it, the abort succeeds as for any ended recording).
    if (m_splitFirstList != nullptr)
    {
        // After End, closing again fails as for an unsplit recording; a second list that never opened has nothing to close.
        const bool closed = !m_splitSecondEnded && (!m_splitSecondOpen || SUCCEEDED(m_commandList->Close()));
        m_splitSecondOpen = false;
        EndSplit(false);
        return closed;
    }

    return SUCCEEDED(m_commandList->Close());
}

bool CommandContext::Execute(unsigned int frameIndex)
{
    CpuTiming::Clock::time_point diagnosticStart{};

    if (m_executeCpuTimingEnabled)
    {
        m_executeCpuTiming = {};

        diagnosticStart = CpuTiming::Clock::now();
    }

    // A failed Execute must never expose an older successful submission as new.
    m_lastSubmissionFence = 0;

    if (frameIndex >= FrameCount || m_fenceValue == 0 || m_fenceValue == UINT64_MAX)
    {
        return false;
    }

    ID3D12CommandList* commandLists[] = {m_commandList};

    if (m_executeCpuTimingEnabled)
    {

        m_executeCpuTiming.validation = CpuTiming::MillisecondsSince(diagnosticStart);

        diagnosticStart = CpuTiming::Clock::now();
    }

    // AIR async compute: the frame's compute list, overlapped (ahead of the direct list, after the frame queue's earlier work) or
    // after the frame (behind this direct list). computeWait: the compute fence value the frame fence must wait for (0: none).
    std::uint64_t computeWait = 0;
    const bool asyncCompute = m_asyncComputeOpen;
    const bool afterFrame = m_asyncComputeAfterFrame;
    m_asyncComputeOpen = false;
    m_asyncComputeAfterFrame = false;
    bool asyncClosed = asyncCompute && SUCCEEDED(m_computeList->Close());

    if (asyncCompute && !asyncClosed)
    {
        Log::Error("AIR async compute: the compute list could not be closed; this frame's AIR work is dropped.");
    }

    const auto submitCompute = [&]()
    {
        ID3D12CommandList* computeLists[] = {m_computeList};
        m_computeQueue->ExecuteCommandLists(1, computeLists);
        const std::uint64_t value = m_computeFenceValue + 1;

        if (FAILED(m_computeQueue->Signal(m_computeFence, value)))
        {
            Log::Error("AIR async compute: the compute fence could not be signalled.");
            return;
        }

        m_computeFenceValue = value;
        computeWait = value;
        ++m_asyncComputeSubmissions;
    };

    if (asyncClosed && !afterFrame)
    {
        // Every frame-fence value below m_fenceValue has been signalled (it advances only after a successful Signal).
        if (m_fenceValue > 1)
        {
            m_computeQueue->Wait(m_fence, m_fenceValue - 1);
        }

        submitCompute();
    }

    // A split recording (SplitForAsyncComputeReaders): the first part now; the compute work is ordered after it (after-the-frame
    // ordering) or beside it, and the second part, which reads the compute work, waits for it. One frame-fence signal follows.
    const bool split = m_splitFirstList != nullptr;

    if (split)
    {
        ID3D12CommandList* firstLists[] = {m_splitFirstList};
        m_commandQueue->ExecuteCommandLists(1, firstLists);

        if (asyncClosed && afterFrame)
        {
            const std::uint64_t directValue = m_directListFenceValue + 1;

            if (SUCCEEDED(m_commandQueue->Signal(m_directListFence, directValue)))
            {
                m_directListFenceValue = directValue;
                m_computeQueue->Wait(m_directListFence, directValue);
                submitCompute();
                ++m_asyncComputeAfterFrameSubmissions;
            }
            else
            {
                Log::Error("AIR async compute: the direct-list fence could not be signalled; this frame's AIR work is dropped.");
            }
        }

        if (computeWait != 0)
        {
            m_commandQueue->Wait(m_computeFence, computeWait);
            computeWait = 0;
        }

        ++m_splitSubmissions;
    }

    m_commandQueue->ExecuteCommandLists(1, commandLists);
    // R.1: submitted (whether or not the fence signal below succeeds): its state changes are authoritative.
    ResourceStateTransaction::CommitRecording();
    EndSplit(false);

    if (asyncClosed && afterFrame && !split)
    {
        const std::uint64_t directValue = m_directListFenceValue + 1;

        if (SUCCEEDED(m_commandQueue->Signal(m_directListFence, directValue)))
        {
            m_directListFenceValue = directValue;
            m_computeQueue->Wait(m_directListFence, directValue);
            submitCompute();
            ++m_asyncComputeAfterFrameSubmissions;
        }
        else
        {
            Log::Error("AIR async compute: the direct-list fence could not be signalled; this frame's AIR work is dropped.");
        }
    }

    // The frame fence covers the compute work: every user of it (slot allocators, the upload ring, retirements, readbacks) stays
    // correct without knowing about the compute queue.
    if (computeWait != 0)
    {
        m_commandQueue->Wait(m_computeFence, computeWait);
    }

    if (m_executeCpuTimingEnabled)
    {
        m_executeCpuTiming.commands = CpuTiming::MillisecondsSince(diagnosticStart);
        m_executeCpuTiming.commandsCalled = true;
    }

    const unsigned long long fenceValue = m_fenceValue;

    if (m_executeCpuTimingEnabled)
    {
        diagnosticStart = CpuTiming::Clock::now();
    }

    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);

    if (m_executeCpuTimingEnabled)
    {
        m_executeCpuTiming.signal = CpuTiming::MillisecondsSince(diagnosticStart);
        m_executeCpuTiming.signalCalled = true;

        diagnosticStart = CpuTiming::Clock::now();
    }

    if (FAILED(result))
    {
        Log::Error("Failed to signal frame fence after command submission. Frame index: " + std::to_string(frameIndex) +

                   "; fence value: " + std::to_string(fenceValue) + "; HRESULT: " + std::to_string(result));
#ifdef _DEBUG
        ID3D12Device* device = nullptr;

        if (SUCCEEDED(m_commandQueue->GetDevice(IID_PPV_ARGS(&device))))
        {
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::Execute Signal");
            device->Release();
        }
#endif
        return false;
    }

    m_frameFenceValues[frameIndex] = fenceValue;
    m_lastSubmissionFence = fenceValue;
    ++m_fenceValue;

    if (m_executeCpuTimingEnabled)
    {
        m_executeCpuTiming.bookkeeping = CpuTiming::MillisecondsSince(diagnosticStart);
    }

    return true;
}

ID3D12GraphicsCommandList* CommandContext::BeginLifecycleCommands()
{
    if (m_lifecycleOpen || !m_commandQueue || !m_fence || !m_fenceEvent)
    {
        Log::Error("Cannot begin lifecycle commands: one is already open or the command context is not initialized.");
        return nullptr;
    }

    if (m_lifecycleUncertain)
    {
        // A lifecycle submission whose signal failed may still run: its allocator cannot be reset safely.
        Log::Error("Cannot begin lifecycle commands after an uncertain lifecycle submission.");
        return nullptr;
    }

    if (!m_lifecycleAllocator)
    {
        ID3D12Device* device = nullptr;

        if (FAILED(m_commandQueue->GetDevice(IID_PPV_ARGS(&device))))
        {
            return nullptr;
        }

        const bool created =
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_lifecycleAllocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_lifecycleAllocator, nullptr,
                IID_PPV_ARGS(&m_lifecycleList))) &&
            SUCCEEDED(m_lifecycleList->Close());
        device->Release();

        if (!created)
        {
            Log::Error("Failed to create the lifecycle command list.");
            return nullptr;
        }

        m_lifecycleAllocator->SetName(L"Lifecycle Command Allocator");
        m_lifecycleList->SetName(L"Lifecycle Command List");
    }

    // The allocator is reused: its previous submission must have completed (rare; lifecycle submissions are rare).
    if (m_lifecycleFenceValue != 0 && m_fence->GetCompletedValue() < m_lifecycleFenceValue)
    {
        if (FAILED(m_fence->SetEventOnCompletion(m_lifecycleFenceValue, static_cast<HANDLE>(m_fenceEvent))) ||
            WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE) != WAIT_OBJECT_0 ||
            m_fence->GetCompletedValue() == UINT64_MAX)
        {
            Log::Error("Failed waiting for the previous lifecycle submission.");
            return nullptr;
        }
    }

    if (FAILED(m_lifecycleAllocator->Reset()) || FAILED(m_lifecycleList->Reset(m_lifecycleAllocator, nullptr)))
    {
        Log::Error("Failed to reset the lifecycle command list.");
        return nullptr;
    }

    m_lifecycleOpen = true;
    return m_lifecycleList;
}

bool CommandContext::SubmitLifecycleCommands(std::uint64_t& fenceValue)
{
    fenceValue = 0;

    if (!m_lifecycleOpen)
    {
        return false;
    }

    m_lifecycleOpen = false;

    if (FAILED(m_lifecycleList->Close()))
    {
        Log::Error("Failed to close the lifecycle command list; nothing was executed.");
        return false;
    }

    if (m_fenceValue == 0 || m_fenceValue == UINT64_MAX)
    {
        Log::Error("Cannot submit lifecycle commands: fence values exhausted or uninitialized.");
        return false;
    }

    ID3D12CommandList* lists[] = {m_lifecycleList};
    m_commandQueue->ExecuteCommandLists(1, lists);
    ++m_lifecycleSubmissions;

    // The frame fence, like WaitForGPU's idle signal: values stay ordered with frame submissions on the one queue, and
    // m_lastSubmissionFence (frame submissions only) is unchanged.
    const unsigned long long signalled = m_fenceValue;

    if (FAILED(m_commandQueue->Signal(m_fence, signalled)))
    {
        Log::Error("Failed to signal the frame fence after lifecycle commands; their completion is unknown.");
        m_lifecycleUncertain = true;
        m_lifecycleFenceValue = 0;
        return true;
    }

    ++m_fenceValue;
    m_lifecycleFenceValue = signalled;
    fenceValue = signalled;
    return true;
}

void CommandContext::AbandonLifecycleCommands()
{
    if (!m_lifecycleOpen)
    {
        return;
    }

    // Closed, never executed; the next Begin resets the allocator.
    m_lifecycleOpen = false;
    m_lifecycleList->Close();
}

bool CommandContext::WaitForGPU()
{
    TransitionTiming::Scope er1Scope("GPU WaitForGPU (idle)", 0.0, TransitionTiming::TransitionsAndStartup);
    const StreamingTelemetry::Scope streamingTiming(StreamingTelemetry::Metric::GpuIdleWaitNs,
        StreamingTelemetry::Metric::GpuIdleWaitCalls);
    ++m_gpuDrainCount;
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
    {
        Log::Error("Cannot wait for GPU: command context is not initialized.");

        return false;
    }

    if (m_fenceValue == 0 || m_fenceValue == UINT64_MAX)
    {
        Log::Error("Cannot signal GPU-idle fence: fence values exhausted or uninitialized.");
        return false;
    }

    const auto deviceRemoved = [&]()
    {
        ID3D12Device* device = nullptr;
        const HRESULT result = m_fence->GetDevice(IID_PPV_ARGS(&device));

        if (FAILED(result))
        {
            Log::Error("GPU fence reports device removal; GetDevice HRESULT: " + std::to_string(result));
        }
        else
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            Log::Error("GPU fence reports device removal; removal HRESULT: " + std::to_string(reason));
#ifdef _DEBUG
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::WaitForGPU idle fence");
#endif
            device->Release();
        }
    };

    if (m_fence->GetCompletedValue() == UINT64_MAX)
    {
        deviceRemoved();
        return false;
    }

    const unsigned long long fenceValue = m_fenceValue;
    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);

    if (FAILED(result))
    {
        Log::Error("Failed to signal GPU-idle fence; submitted work may still be in flight. HRESULT: " +
                   std::to_string(result));
        return false;
    }

    ++m_fenceValue;

    unsigned long long completedValue = m_fence->GetCompletedValue();

    if (completedValue == UINT64_MAX)
    {
        deviceRemoved();
        return false;
    }

    while (completedValue < fenceValue)
    {
        result = m_fence->SetEventOnCompletion(fenceValue, static_cast<HANDLE>(m_fenceEvent));

        if (FAILED(result))
        {
            Log::Error("Failed to register GPU-idle fence event. HRESULT: " + std::to_string(result));

            return false;
        }

        const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);

        if (waitResult != WAIT_OBJECT_0)
        {
            const DWORD error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;

            Log::Error("GPU-idle wait failed. Wait result: " + std::to_string(waitResult) +
                       "; Win32 error: " + std::to_string(error));
            return false;
        }

        completedValue = m_fence->GetCompletedValue();

        if (completedValue == UINT64_MAX)
        {
            deviceRemoved();

            return false;
        }
    }

    return true;
}

ID3D12GraphicsCommandList* CommandContext::GetCommandList() const
{
    return m_commandList;
}

ID3D12CommandQueue* CommandContext::GetCommandQueue() const
{
    return m_commandQueue;
}

ID3D12GraphicsCommandList4* CommandContext::GetRayTracingCommandList() const
{
    return m_rayTracingCommandList;
}

uint64_t CommandContext::GetCompletedFenceValue() const
{
    if (!m_fence)
    {
        return 0;
    }

    return m_fence->GetCompletedValue();
}

uint64_t CommandContext::GetFrameFenceValue(unsigned int frameIndex) const
{
    if (frameIndex >= FrameCount)
    {
        return 0;
    }

    return m_frameFenceValues[frameIndex];
}

SubmissionFenceToken CommandContext::RetainLastSubmissionFence()
{
    if (!m_submissionLifetime || !m_fence || !m_commandQueue || m_lastSubmissionFence == 0 ||

        m_lastSubmissionFence == UINT64_MAX || m_nextRetentionID == UINT64_MAX)
    {
        return {};
    }

    for (SubmissionFenceToken& entry : m_retainedSubmissionFences)
    {
        if (entry.retentionID == 0)
        {
            entry = {m_lastSubmissionFence, m_submissionLifetime, m_nextRetentionID++};

            return entry;
        }
    }

    return {}; // Never evict an outstanding client's provenance.
}

bool CommandContext::HasSubmissionReservations() const
{
    for (ReservationState state : m_reservationStates)
    {
        if (state != ReservationState::None)
        {
            return true;
        }
    }

    return false;
}

SubmissionFenceReservation CommandContext::ReserveSubmissionFence()
{
    if (!m_submissionLifetime || !m_fence || !m_commandQueue || !m_commandList || !m_fenceEvent ||
        m_nextRetentionID == UINT64_MAX)
    {
        return {};
    }

    for (size_t i = 0; i < m_retainedSubmissionFences.size(); ++i)
    {
        if (m_retainedSubmissionFences[i].retentionID == 0)
        {
            const uint64_t id = m_nextRetentionID++;

            m_retainedSubmissionFences[i] = {0, m_submissionLifetime, id};
            m_reservationStates[i] = ReservationState::Reserved;

            return {m_submissionLifetime, id};
        }
    }

    return {};
}

bool CommandContext::CancelSubmissionFenceReservation(SubmissionFenceReservation reservation)
{
    if (!reservation.lifetime || reservation.lifetime != m_submissionLifetime || !reservation.reservationID)
    {
        return false;
    }

    for (size_t i = 0; i < m_retainedSubmissionFences.size(); ++i)
    {
        const SubmissionFenceToken& entry = m_retainedSubmissionFences[i];

        if (m_reservationStates[i] == ReservationState::Reserved && entry.fenceValue == 0 &&
            entry.lifetime == reservation.lifetime && entry.retentionID == reservation.reservationID)
        {
            m_retainedSubmissionFences[i] = {};

            m_reservationStates[i] = ReservationState::None;
            return true;
        }
    }

    return false;
}

ReservedSubmissionResult CommandContext::ExecuteReserved(

    unsigned int frameIndex,
    SubmissionFenceReservation reservation,
    SubmissionFenceToken& token)
{
    CpuTiming::Clock::time_point diagnosticReservedStart{};

    if (m_executeCpuTimingEnabled)
    {
        m_executeCpuTiming = {};
        diagnosticReservedStart = CpuTiming::Clock::now();
    }

    if (!reservation.lifetime || reservation.lifetime != m_submissionLifetime || !reservation.reservationID)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    if (frameIndex >= FrameCount || !m_commandQueue || !m_commandList || !m_fence || !m_fenceEvent)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    if (m_fenceValue == 0 || m_fenceValue == UINT64_MAX)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    for (ReservationState state : m_reservationStates)
    {
        if (state == ReservationState::SubmissionUncertain)
        {
            return ReservedSubmissionResult::NotSubmitted;
        }
    }

    for (size_t i = 0; i < m_retainedSubmissionFences.size(); ++i)
    {
        SubmissionFenceToken& entry = m_retainedSubmissionFences[i];

        if (m_reservationStates[i] != ReservationState::Reserved || entry.fenceValue != 0 ||

            entry.lifetime != reservation.lifetime || entry.retentionID != reservation.reservationID)
        {
            continue;
        }

        // Lock before the attempt. Single-threaded Execute signals exactly this
        // value; conversion never consults a mutable last-submission capture.
        const uint64_t fenceValue = m_fenceValue;

        m_reservationStates[i] = ReservationState::SubmissionUncertain;

        const double reservedValidation =

            m_executeCpuTimingEnabled ? CpuTiming::MillisecondsSince(diagnosticReservedStart) : 0.0;

        if (!Execute(frameIndex))
        {
            return ReservedSubmissionResult::SubmissionUncertain;
        }

        CpuTiming::Clock::time_point diagnosticAssociationStart{};

        if (m_executeCpuTimingEnabled)
        {
            diagnosticAssociationStart = CpuTiming::Clock::now();
            m_executeCpuTiming.validation += reservedValidation;
        }

        entry.fenceValue = fenceValue;
        m_reservationStates[i] = ReservationState::None;
        token = entry;

        if (m_executeCpuTimingEnabled)
        {
            m_executeCpuTiming.bookkeeping += CpuTiming::MillisecondsSince(diagnosticAssociationStart);
        }

        return ReservedSubmissionResult::Submitted;
    }

    return ReservedSubmissionResult::NotSubmitted;
}

bool CommandContext::ReleaseSubmissionFence(SubmissionFenceToken token)
{
    if (token.fenceValue == 0 ||
        token.fenceValue == UINT64_MAX ||
        token.lifetime == 0 ||
        token.lifetime != m_submissionLifetime)
    {
        return false;
    }

    if (token.retentionID == 0)
    {
        return false;
    }

    for (SubmissionFenceToken& entry : m_retainedSubmissionFences)
    {
        if (SameSubmissionToken(entry, token))
        {
            entry = {};
            return true;
        }
    }

    return false;
}

SubmissionFenceStatus CommandContext::QuerySubmissionFenceStatus(SubmissionFenceToken token) const
{
    if (!m_fence || !m_commandQueue || !m_fenceEvent)
    {
        return SubmissionFenceStatus::Error;
    }

    if (token.fenceValue == 0 || token.fenceValue == UINT64_MAX)
    {
        return SubmissionFenceStatus::Error;
    }

    if (token.lifetime == 0 || token.lifetime != m_submissionLifetime || token.retentionID == 0)
    {
        return SubmissionFenceStatus::Error;
    }

    bool retained = false;

    for (const SubmissionFenceToken& entry : m_retainedSubmissionFences)
    {
        retained = retained || SameSubmissionToken(entry, token);
    }

    if (!retained)
    {
        return SubmissionFenceStatus::Error;
    }

    ID3D12Device* device = nullptr;

    const HRESULT result = m_fence->GetDevice(IID_PPV_ARGS(&device));

    if (FAILED(result))
    {
        Log::Error("Submission fence query GetDevice failed. HRESULT: " + std::to_string(result));

        return SubmissionFenceStatus::Error;
    }

    const uint64_t completed = m_fence->GetCompletedValue();

    const HRESULT reason = device->GetDeviceRemovedReason();

    if (completed == UINT64_MAX || FAILED(reason))
    {
        Log::Error("Submission fence query reports device removal. HRESULT: " + std::to_string(reason));
#ifdef _DEBUG
        GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::QuerySubmissionFenceStatus");
#endif
        device->Release();
        return SubmissionFenceStatus::Error;
    }

    device->Release();

    return completed >= token.fenceValue ? SubmissionFenceStatus::Complete : SubmissionFenceStatus::Pending;
}

bool CommandContext::DiscardCommandsAfterGPUDrain()
{
    if (!m_commandList || !m_rayTracingCommandList || !m_commandAllocators[0] || HasSubmissionReservations())
    {
        return false;
    }

    for (const auto& token : m_retainedSubmissionFences)
    {
        if (token.retentionID != 0)
        {
            return false;
        }
    }

    if (!WaitForGPU())
    {
        return false;
    }

    // R.1: an open recording on the old list is never submitted (nor either part of a split one).
    AbandonAsyncCompute();
    EndSplit(true);
    ResourceStateTransaction::DiscardRecording();
    TextureMemoryAllocator::EndFrameRecording();

    // Use a fresh allocator: the old list might still be open or poisoned by
    // a failed Close/Reset. Nothing is replaced until the new closed list is ready.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> rayTracingCommands;

    if (FAILED(m_commandList->GetDevice(IID_PPV_ARGS(&device))))
    {
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
    {
        return false;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands))))
    {
        return false;
    }

    commands->SetName(L"Frame command list (replacement)");

    if (FAILED(commands.As(&rayTracingCommands)) || FAILED(commands->Close()))
    {
        return false;
    }

    m_rayTracingCommandList->Release();
    m_commandList->Release();
    m_commandAllocators[0]->Release();

    m_commandList = commands.Detach();
    m_rayTracingCommandList = rayTracingCommands.Detach();
    m_commandAllocators[0] = allocator.Detach();
    m_lastSubmissionFence = 0;
    return true;
}
