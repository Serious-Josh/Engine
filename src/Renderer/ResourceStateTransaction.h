#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// R.1: recording-scoped resource-state transactions.
//
// Engine's tracked resources (Buffer, Texture, RenderTarget, DepthBuffer) update their CPU-side state when a barrier is
// recorded. Those states describe the GPU only once the recording is submitted. CommandContext opens a transaction for
// each recording of the frame command list; ExecuteCommandLists commits it, and discarding the list (AbortRecording, a
// reset or a replacement without submission) marks it discarded. A tracked state first changed by a discarded recording
// returns, the next time it is read or transitioned, to the state it had before that recording: the state of the last
// submitted work. Submitted work is never rolled back, whether or not the GPU has completed it.
//
// Cost: a committed recording does nothing per resource; the first barrier on a resource in a recording copies its
// previous state (no allocation for single states; a Texture's per-subresource vector reuses its capacity). Discarded
// recordings are remembered by number (a few per session at most). Render thread only, like the command list.
namespace ResourceStateTransaction
{
    // CommandContext: a recording of `commandList` (and its ray-tracing interface, the same object) begins, is submitted,
    // or is discarded unsubmitted. Begin discards a still-open earlier recording (its list is being reset).
    void BeginRecording(const void* commandList, const void* rayTracingCommandList);
    // AIR async compute: a compute-queue list recorded beside the open frame recording and submitted with it (CommandContext::
    // Execute), so its barriers commit or discard with the frame recording. Cleared by commit and discard.
    void AttachRecordingList(const void* commandList);
    // AIR async compute, readers in the frame (CommandContext::SplitForAsyncComputeReaders): the open recording continues on a second
    // direct list. The first list stays part of the recording (its barriers commit or discard with it).
    void SwitchRecordingList(const void* commandList, const void* rayTracingCommandList);
    void CommitRecording();
    void DiscardRecording();

    // The open recording's number when `commandList` is its list; 0 for any other list (a separate, immediately submitted
    // context such as the resource uploader) or when no recording is open.
    std::uint64_t RecordingFor(const void* commandList);
    bool IsDiscarded(std::uint64_t recording);

    struct Statistics
    {
        std::uint64_t recordings = 0;
        std::uint64_t committed = 0;
        std::uint64_t discarded = 0;
        // States returned to their pre-recording value after a discard.
        std::uint64_t restored = 0;
    };

    Statistics GetStatistics();
    void CountRestored();

    // Diagnostics (tests): when enabled, the resources whose tracked state the open recording changed first, kept for the last
    // discarded recording.
    void SetDiagnostics(bool enabled);
    bool DiagnosticsEnabled();
    void NoteFirstChange(const void* resource);
    const std::vector<const void*>& LastDiscardedResources();
}

// One tracked state (a resource's, or a Texture's per-subresource vector) with its transaction bookkeeping.
template<typename State>
class TransactionalResourceState
{
    public:
    TransactionalResourceState() = default;

    explicit TransactionalResourceState(State state) : m_value(std::move(state))
    {
    }

    // The tracked state: what the submitted work and the open recording leave the resource in.
    const State& Get() const
    {
        Resolve();
        return m_value;
    }

    // An authoritative state outside any recording: creation, replacement, shutdown, or an explicit abort restore.
    void Reset(State state)
    {
        m_value = std::move(state);
        m_recording = 0;
    }

    // Mutable state for a barrier recorded on `commandList`. In the open frame recording, the state before it is kept first.
    State& Change(const void* commandList, const void* resource = nullptr)
    {
        Resolve();
        const std::uint64_t recording = ResourceStateTransaction::RecordingFor(commandList);

        if(recording != 0 && m_recording != recording)
        {
            m_submitted = m_value;
            m_recording = recording;

            if(ResourceStateTransaction::DiagnosticsEnabled())
            {
                ResourceStateTransaction::NoteFirstChange(resource);
            }
        }

        return m_value;
    }

    // Moves carry the bookkeeping with the state; the source is reset to `empty`.
    void TakeFrom(TransactionalResourceState& other, State empty)
    {
        other.Resolve();
        m_value = std::move(other.m_value);
        m_submitted = std::move(other.m_submitted);
        m_recording = other.m_recording;
        other.Reset(std::move(empty));
    }

    private:
    void Resolve() const
    {
        if(m_recording != 0 && ResourceStateTransaction::IsDiscarded(m_recording))
        {
            m_value = m_submitted;
            m_recording = 0;
            ResourceStateTransaction::CountRestored();
        }
    }

    mutable State m_value{};
    mutable State m_submitted{};
    mutable std::uint64_t m_recording = 0;
};
