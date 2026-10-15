#pragma once

#include <d3d12.h>
#include <cstdint>
#include <array>

enum class SubmissionFenceStatus
{
    Pending,
    Complete,
    Error
};

struct SubmissionFenceToken
{
    uint64_t fenceValue = 0;
    uint64_t lifetime = 0;
    uint64_t retentionID = 0;
};

struct SubmissionFenceReservation
{
    uint64_t lifetime = 0;
    uint64_t reservationID = 0;
};

enum class ReservedSubmissionResult
{
    NotSubmitted,
    Submitted,
    SubmissionUncertain
};

class CommandContext
{
    public:
    static constexpr unsigned int FrameCount = 2;

    CommandContext() = default;

    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    bool Begin(unsigned int frameIndex);
    bool End();
    bool AbortRecording();

    // Teardown/recovery only. Requires exclusive command-context use and no
    // retained tokens or reservations. Drains first, then replaces the list
    // and its allocator so abandoned commands cannot be submitted by Execute.
    // Failure authorizes no caller resource release. Borrowed list pointers
    // must not be retained across this operation.
    bool DiscardCommandsAfterGPUDrain();

    bool Execute(unsigned int frameIndex);
    std::uint64_t GetGPUDrainCount() const noexcept { return m_gpuDrainCount; }
    bool WaitForGPU();

    ID3D12GraphicsCommandList* GetCommandList() const;
    ID3D12CommandQueue* GetCommandQueue() const;
    ID3D12GraphicsCommandList4* GetRayTracingCommandList() const;

    uint64_t GetCompletedFenceValue() const;
    // Borrowed fence values are valid only within this context lifetime.
    uint64_t GetLastSubmissionFenceValue() const
    {
        return m_lastSubmissionFence;
    }

    // Provenance for borrowed fence values owned by scene-lifetime consumers.
    // Such consumers must be cleared by checked teardown before context reset.
    uint64_t GetSubmissionLifetime() const { return m_submissionLifetime; }

    uint64_t GetFrameFenceValue(unsigned int frameIndex) const;

    // Single-threaded, like submission. Capture immediately after successful
    // Execute, before another Execute. Idle signals never become capturable.
    // At most eight retained tokens; exhaustion returns an invalid token.
    // Tokens survive later submissions until explicit release or Shutdown.
    SubmissionFenceToken RetainLastSubmissionFence();
    // UNUSED, single-threaded. Shares the eight token slots. Exhaustion or
    // invalid context returns {} without submission. Not tied to a recording;
    // Begin/abort may occur before use. Cancel before teardown if unused.
    SubmissionFenceReservation ReserveSubmissionFence();
    bool CancelSubmissionFenceReservation(SubmissionFenceReservation reservation);
    // Executes exactly once using this reservation. Output changes only on
    // Submitted. NotSubmitted leaves a valid reservation cancellable. Signal
    // failure locks it permanently (no drain recovery here): never cancel,
    // reset allocators, or tear down this context after SubmissionUncertain.
    ReservedSubmissionResult ExecuteReserved(
        unsigned int frameIndex,
        SubmissionFenceReservation reservation,
        SubmissionFenceToken& token);
    bool ReleaseSubmissionFence(SubmissionFenceToken token);
    // Nonblocking. Bare fence values cannot distinguish context lifetimes.
    // Released, unknown, or prior-lifetime tokens return Error. Retention
    // tracks provenance only; it does not own GPU resources or synchronize reuse.
    SubmissionFenceStatus QuerySubmissionFenceStatus(SubmissionFenceToken token) const;

    bool DidBeginWaitForFence() const
    {
        return m_beginWaitedForFence;
    }

    double GetBeginFenceWaitMilliseconds() const
    {
        return m_beginFenceWaitMilliseconds;
    }

    double GetBeginResetMilliseconds() const
    {
        return m_beginResetMilliseconds;
    }

    struct ExecuteCpuTiming
    {
        double validation = 0.0;
        double commands = 0.0;
        double signal = 0.0;
        double bookkeeping = 0.0;

        bool commandsCalled = false;
        bool signalCalled = false;
    };

    // Diagnostic only: opt in from Renderer; no fence or ownership operations.
    void EnableExecuteCpuTiming(bool enabled)
    {
        m_executeCpuTimingEnabled = enabled;
    }

    const ExecuteCpuTiming& GetExecuteCpuTiming() const
    {
        return m_executeCpuTiming;
    }

    private:

    std::uint64_t m_gpuDrainCount = 0;
    bool m_executeCpuTimingEnabled = false;

    ExecuteCpuTiming m_executeCpuTiming{};

    bool m_beginWaitedForFence = false;

    double m_beginFenceWaitMilliseconds = 0.0;
    double m_beginResetMilliseconds = 0.0;

    ID3D12CommandQueue* m_commandQueue = nullptr;
    ID3D12CommandAllocator* m_commandAllocators[FrameCount]{};
    ID3D12GraphicsCommandList* m_commandList = nullptr;

    ID3D12GraphicsCommandList4* m_rayTracingCommandList = nullptr;

    ID3D12Fence* m_fence = nullptr;
    unsigned long long m_fenceValue = 0;
    unsigned long long m_frameFenceValues[FrameCount]{};
    uint64_t m_submissionLifetime = 0;
    uint64_t m_lastSubmissionFence = 0;
    uint64_t m_nextRetentionID = 1;

    std::array<SubmissionFenceToken, 8> m_retainedSubmissionFences{};
    enum class ReservationState
    {
        None,
        Reserved,
        SubmissionUncertain
    };
    std::array<ReservationState, 8> m_reservationStates{};
    bool HasSubmissionReservations() const;

    void* m_fenceEvent = nullptr;
};
