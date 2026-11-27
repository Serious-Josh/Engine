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
    // Blocking wait for this slot's last submission, as Begin performs. Lets a
    // pre-frame submission reuse the slot allocator rather than defer indefinitely.
    bool WaitForFrameFence(unsigned int frameIndex);
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

    // RR.1A: a short lifecycle command list on the frame queue, outside the frame recording, for NGX feature creation. The
    // frame recording can be discarded (R.1); work recorded here is submitted at once, so it is on the queue ahead of every
    // later frame submission without a CPU wait. Begin waits only if the previous lifecycle submission is still running
    // (its allocator is reused) and returns nullptr while one is open or when it cannot reset. Submit closes and executes
    // the list, then signals the frame fence: fenceValue is the signalled value, 0 when the signal failed after execution
    // (completion unknown). False: nothing was executed (the list is closed and may be begun again). Abandon closes it
    // without executing anything. Tracked resources (R.1) are never transitioned on this list. Render thread only.
    ID3D12GraphicsCommandList* BeginLifecycleCommands();
    bool SubmitLifecycleCommands(std::uint64_t& fenceValue);
    void AbandonLifecycleCommands();
    std::uint64_t GetLifecycleSubmissionCount() const noexcept { return m_lifecycleSubmissions; }

    // AIR async compute (AIR optimization report item 3): a compute queue beside the frame queue, with one compute list recorded
    // inside the frame recording and submitted with it by Execute. Render thread only, like the frame list.
    //
    // Ordering, for a frame N whose compute list is used:
    //   - Overlapped (the default): the compute queue waits only for the frame queue's earlier submissions (up to frame N-1), so
    //     the compute work runs beside frame N's direct work. The caller guarantees that frame N's direct list touches nothing the
    //     compute list reads or writes.
    //   - After the frame (RequireAsyncComputeAfterFrame): the compute queue waits for frame N's direct list, for a frame whose
    //     direct list did write something the compute list uses (a resource activated and zero-filled there).
    // Either way the frame queue waits for the compute work before it signals frame N's fence, so that fence (and every frame
    // fence, slot allocator and retirement keyed to it) covers the compute work too: nothing else needs to know about the queue.
    bool HasAsyncCompute() const noexcept
    {
        return m_computeQueue != nullptr;
    }

    // The compute list for this frame recording (opened on first use, reset on this slot's compute allocator; Begin has waited
    // for the slot's previous frame fence, which covers its previous compute work). Its barriers belong to the frame recording's
    // state transaction (R.1): AbortRecording discards them with it. nullptr without a compute queue, outside a recording, or
    // when the list cannot be reset.
    ID3D12GraphicsCommandList* BeginAsyncCompute(unsigned int frameIndex);
    void RequireAsyncComputeAfterFrame() noexcept
    {
        m_asyncComputeAfterFrame = true;
    }
    ID3D12CommandQueue* GetComputeQueue() const noexcept
    {
        return m_computeQueue;
    }
    // Submissions of the compute list (diagnostics), and those ordered after their frame.
    std::uint64_t GetAsyncComputeSubmissionCount() const noexcept { return m_asyncComputeSubmissions; }
    std::uint64_t GetAsyncComputeAfterFrameCount() const noexcept { return m_asyncComputeAfterFrameSubmissions; }

    // AIR async compute, readers in the frame: splits the frame's direct recording at the first reader of the compute work. The
    // direct list recorded so far is closed (the first part); recording continues on a second direct list (the second part), which
    // GetCommandList and GetRayTracingCommandList return from now on. Nothing is submitted here: Execute submits the compute list,
    // the first part, a direct-queue wait for the compute work, then the second part, and signals the frame fence once, after
    // the second part (so that fence, an ExecuteReserved reservation's value included, covers both parts, the slot allocator and
    // the upload ring). AbortRecording and DiscardCommandsAfterGPUDrain close both parts unsubmitted. Both parts share the slot's
    // allocator (one list records at a time) and the frame recording's transaction (R.1). The second list starts without state:
    // the caller sets its descriptor heaps and fixed-function state again. Returns the second list; nullptr (the recording cannot
    // continue: abort it) when no compute list is open in this recording, the recording is already split, or the second list
    // cannot be reset.
    ID3D12GraphicsCommandList* SplitForAsyncComputeReaders(unsigned int frameIndex);
    bool IsSplitForAsyncComputeReaders() const noexcept
    {
        return m_splitFirstList != nullptr;
    }
    std::uint64_t GetSplitSubmissionCount() const noexcept { return m_splitSubmissions; }

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

    // FG.2: the frame queue's fence, which the Frame Generation present thread waits on (read-only use: it never signals).
    ID3D12Fence* GetFence() const
    {
        return m_fence;
    }

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

    // RR.1A lifecycle list (created on first use).
    ID3D12CommandAllocator* m_lifecycleAllocator = nullptr;
    ID3D12GraphicsCommandList* m_lifecycleList = nullptr;
    bool m_lifecycleOpen = false;
    // The fence value of the last lifecycle submission (0: none, or its signal failed and m_lifecycleUncertain is set).
    std::uint64_t m_lifecycleFenceValue = 0;
    bool m_lifecycleUncertain = false;
    std::uint64_t m_lifecycleSubmissions = 0;

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

    // AIR async compute. The queue is optional: without it every frame records AIR on the frame list as before.
    void InitializeAsyncCompute(ID3D12Device* device);
    void ShutdownAsyncCompute();
    // Closes the open compute list without submitting it (abort, discard).
    void AbandonAsyncCompute();
    ID3D12CommandQueue* m_computeQueue = nullptr;
    ID3D12CommandAllocator* m_computeAllocators[FrameCount]{};
    ID3D12GraphicsCommandList* m_computeList = nullptr;
    ID3D12Fence* m_computeFence = nullptr;
    std::uint64_t m_computeFenceValue = 0;
    // The frame queue's own signal of "this frame's direct list is done", for after-the-frame ordering (the frame fence's values
    // are reserved for frame submissions: ExecuteReserved relies on Execute signalling exactly m_fenceValue).
    ID3D12Fence* m_directListFence = nullptr;
    std::uint64_t m_directListFenceValue = 0;
    bool m_asyncComputeOpen = false;
    bool m_asyncComputeAfterFrame = false;
    std::uint64_t m_asyncComputeSubmissions = 0;
    std::uint64_t m_asyncComputeAfterFrameSubmissions = 0;

    // The split recording (SplitForAsyncComputeReaders): the second direct list (created with the compute queue, kept closed between
    // uses) and, while a recording is split, its closed first part. m_commandList / m_rayTracingCommandList then name the second list.
    ID3D12GraphicsCommandList* m_splitSecondList = nullptr;
    ID3D12GraphicsCommandList4* m_splitSecondRayTracingList = nullptr;
    ID3D12GraphicsCommandList* m_splitFirstList = nullptr;
    ID3D12GraphicsCommandList4* m_splitFirstRayTracingList = nullptr;
    bool m_splitSecondOpen = false;
    bool m_splitSecondEnded = false;
    std::uint64_t m_splitSubmissions = 0;
    // Ends a split recording: closes the second list if `closeSecond`, and makes the first list the frame list again.
    void EndSplit(bool closeSecond);
};
