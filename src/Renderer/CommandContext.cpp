#include "CommandContext.h"
#include "Debug/CpuFrameTiming.h"
#include "Core/Log.h"
#ifdef _DEBUG
#include "GraphicsDevice.h"
#endif

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
    return true;
}

void CommandContext::Shutdown()
{
    if (HasSubmissionReservations())
    {
        Log::Error("Command context shutdown: unresolved submission reservation; aborting without teardown.");
        std::abort();
    }

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

    ID3D12CommandAllocator* commandAllocator = m_commandAllocators[frameIndex];

    CpuTiming::ScopedDuration resetDuration(m_beginResetMilliseconds);

    HRESULT result = commandAllocator->Reset();

    if (FAILED(result))
    {
        return false;
    }

    result = m_commandList->Reset(commandAllocator, nullptr);

    return SUCCEEDED(result);
}

bool CommandContext::End()
{
    return SUCCEEDED(m_commandList->Close());
}

bool CommandContext::AbortRecording()
{
    if (!m_commandList)
    {
        return false;
    }

    // Close the command list without submitting it.
    // The next Begin() will reset it.
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

    m_commandQueue->ExecuteCommandLists(1, commandLists);

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

bool CommandContext::WaitForGPU()
{
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
