#pragma once

// Private to the audio module (D9 Option B). THE UNDOCUMENTED COMPATIBILITY BOUNDARY: the only code that implements Steam
// Audio's api::ISource, which Steam Audio does not offer as an extension point. Nothing may pass a SnapshotSource to the
// plugin before CheckSnapshotCompatibility has accepted the running binaries. See SteamAudioIntegration.md, section 18.
//
// Steam Audio's own IPLSource has unsynchronized direct outputs (section 16.3). A SnapshotSource never touches a real
// source: the simulation worker copies finished results into its SnapshotChannel, and the plugin reads them from there.

#include "Audio/SteamAudioRuntime.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include <fmod_studio.hpp>

#define STEAMAUDIO_SKIP_API_FUNCTIONS
#include <phonon_interfaces.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#endif

namespace Audio::Internal
{
#if defined(ENGINE_HAS_STEAMAUDIO)
// A triple buffer for one writer thread and one reader thread at a time. Wait-free on both sides.
//
// Three slots, one owned by the writer (back), one by the reader (front), one shared. back, front and the shared index are
// always a permutation of {0, 1, 2}, and ownership only moves through one atomic exchange, so neither side can reach a
// slot the other owns however long it stays preempted. Slots are members, so they live exactly as long as the owning SnapshotSource.
template<typename Value>
class TripleBuffer final
{
public:
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

    // The slot the writer alone owns. Invisible to the reader until Publish.
    Value& WriteSlot() noexcept
    {
        return m_slots[m_back].value;
    }

    // Release: every field written above is visible to whoever acquires this slot through m_shared. Acquire: the writer
    // sees the reader's finished reads of the slot it takes back.
    void Publish() noexcept
    {
        const std::uint8_t previous = m_shared.exchange(static_cast<std::uint8_t>(m_back | FreshBit),
            std::memory_order_acq_rel);
        m_back = previous & IndexMask;
    }

    // The newest published snapshot, or the previous one when nothing new exists. It stays valid and unchanged until this
    // reader's next call. The relaxed check can only delay taking a new slot; the exchange is what orders it.
    const Value& AcquireLatest() noexcept
    {
        if((m_shared.load(std::memory_order_relaxed) & FreshBit) != 0)
        {
            const std::uint8_t previous = m_shared.exchange(m_front, std::memory_order_acq_rel);
            m_front = previous & IndexMask;
        }

        return m_slots[m_front].value;
    }

private:
    static constexpr std::uint8_t IndexMask = 0x3;
    static constexpr std::uint8_t FreshBit = 0x4;

    struct alignas(64) Slot
    {
        Value value;
    };

    Slot m_slots[3];
    alignas(64) std::atomic<std::uint8_t> m_shared{1};
    alignas(64) std::uint8_t m_back = 0;
    alignas(64) std::uint8_t m_front = 2;
};

using SnapshotChannel = TripleBuffer<SteamAudioDirectSnapshot>;

// The source the plugin holds. Its methods run on FMOD's threads and never allocate, wait or free.
//
// release() only decrements. The owner destroys the object on its own thread, once FMOD has reported the DSP destroyed, the
// worker has let go, and References() shows the owner's reference as the last. No plugin reference then exists, so nothing
// can reach the vtable again.
class SnapshotSource final : public api::ISource
{
public:
    SnapshotSource() noexcept = default;
    SnapshotSource(const SnapshotSource&) = delete;
    SnapshotSource& operator=(const SnapshotSource&) = delete;

    ISource* retain() override;
    void release() override;
    void getOutputs(IPLSimulationFlags flags, IPLSimulationOutputs* outputs) override;

    // The verified plugin never links these. Counted, and otherwise inert.
    void add(api::ISimulator*) override;
    void remove(api::ISimulator*) override;
    void setInputs(IPLSimulationFlags, IPLSimulationInputs*) override;

    IPLSource AsIPLSource() noexcept
    {
        return reinterpret_cast<IPLSource>(static_cast<api::ISource*>(this));
    }

    SnapshotChannel& Channel() noexcept
    {
        return m_channel;
    }

    // Worker, once. `ir` is the triple buffer inside the private source's SimulationData, which Steam Audio's own convolution
    // reads lock-free. The private source outlives every plugin reference to this object (ReclaimRetiredSources), so the
    // pointer never dangles while the plugin can ask for it. The sizes are written first and the release pairs with getOutputs.
    void PublishReflectionIr(IPLReflectionEffectIR ir, int channels, int samples) noexcept
    {
        m_irChannels = channels;
        m_irSamples = samples;
        m_ir.store(ir, std::memory_order_release);
    }

    // Owner, as Stage B releases the private source. A read after this would be a read of freed memory, and counts as an anomaly.
    void MarkIrReleased() noexcept
    {
        m_irReleased.store(true, std::memory_order_release);
    }

    // Main thread. While withheld the plugin sees no IR, which is silence after the effect's own tail.
    void SetIrWithheld(bool withheld) noexcept
    {
        m_irWithheld.store(withheld, std::memory_order_relaxed);
    }

    std::int64_t IrReads() const noexcept
    {
        return m_irReads.load(std::memory_order_relaxed);
    }

    // M9.1: pathing reads answered with silent coefficients (no valid result published). The plugin asks on every block of a bound
    // source, so this counts blocks.
    std::int64_t NeutralPathingReads() const noexcept
    {
        return m_neutralPathingReads.load(std::memory_order_relaxed);
    }

    // M9.2: pathing reads that handed the plugin a published result.
    std::int64_t ValidPathingReads() const noexcept
    {
        return m_validPathingReads.load(std::memory_order_relaxed);
    }

    // M9.2, the simulation worker only: publishes one pathing result, sanitized so the path effect can never be given what makes it
    // output NaN (any EQ band at zero, M9.1/M9.2) or a non-finite value. A result that is not finite is published as invalid.
    // Returns false when it had to be altered or rejected.
    bool PublishPathing(const SteamAudioPathingSnapshot& result) noexcept;

    // M9.3: whether the latest published pathing result is valid. Written by PublishPathing; the main thread reads it to decide Apply
    // Pathing. Turning it on early is harmless (the reader still gets silence until the result is acquired), turning it off late too.
    bool PathingAvailable() const noexcept
    {
        return m_pathingAvailable.load(std::memory_order_acquire);
    }

    // M9.5: how much of the direct path the plugin lets through, as it applies occlusion and transmission to this event (1 = all of it).
    // Written by the direct worker after each simulation, read by the pathing run on the reflection worker: pathing makes up the rest.
    void SetDirectVisibility(float visibility) noexcept
    {
        m_directVisibility.store(visibility, std::memory_order_relaxed);
    }

    float DirectVisibility() const noexcept
    {
        return m_directVisibility.load(std::memory_order_relaxed);
    }

    // M9.6, display only: what the last pathing run decided for this source, for the Editor. Written by the workers, read on the main
    // thread; each value is independent, so a reader may see one run's batch with the next run's gain, which is harmless here.
    enum class PathingCapacity : std::uint8_t
    {
        NotRequested,
        Waiting,
        Simulating
    };

    void SetPathingCapacity(PathingCapacity capacity) noexcept
    {
        m_pathingCapacity.store(capacity, std::memory_order_relaxed);
    }

    PathingCapacity GetPathingCapacity() const noexcept
    {
        return m_pathingCapacity.load(std::memory_order_relaxed);
    }

    void SetPathingRun(int batch, bool sourceCovered, bool listenerCovered, float gain) noexcept
    {
        m_pathingBatchShown.store(batch, std::memory_order_relaxed);
        m_pathingSourceCovered.store(sourceCovered, std::memory_order_relaxed);
        m_pathingListenerCovered.store(listenerCovered, std::memory_order_relaxed);
        m_pathingGainShown.store(gain, std::memory_order_relaxed);
    }

    int PathingBatchShown() const noexcept
    {
        return m_pathingBatchShown.load(std::memory_order_relaxed);
    }

    bool PathingSourceCovered() const noexcept
    {
        return m_pathingSourceCovered.load(std::memory_order_relaxed);
    }

    bool PathingListenerCovered() const noexcept
    {
        return m_pathingListenerCovered.load(std::memory_order_relaxed);
    }

    float PathingGainShown() const noexcept
    {
        return m_pathingGainShown.load(std::memory_order_relaxed);
    }

    // Including the owner's own. Acquire pairs with release(): what a holder did happens before the owner sees it gone.
    int References() const noexcept
    {
        return m_references.load(std::memory_order_acquire);
    }

    // Accounting failures, none of which ever occurred on the pinned binaries.
    std::int64_t Anomalies() const noexcept
    {
        return m_anomalies.load(std::memory_order_relaxed);
    }

private:
    SnapshotChannel m_channel;
    std::atomic<int> m_references{1};
    std::atomic<std::int64_t> m_anomalies{0};
    // Catches a second concurrent reader without waiting: it gets neutral values and leaves the channel alone.
    std::atomic<bool> m_readerBusy{false};
    std::atomic<IPLReflectionEffectIR> m_ir{nullptr};
    int m_irChannels = 0;
    int m_irSamples = 0;
    std::atomic<bool> m_irReleased{false};
    std::atomic<bool> m_irWithheld{false};
    std::atomic<std::int64_t> m_irReads{0};
    // M9.1: all-zero Ambisonic coefficients, never written. The plugin dereferences shCoeffs whenever its pathing branch runs, and null
    // crashes it (M9.0). What a read gets when no valid result is available to it; the path effect renders them as silence, with
    // getOutputs' non-zero EQ. Sized for order 3, the most the plugin's maxOrder could ask for.
    alignas(16) std::array<float, SteamAudioPathingSnapshot::MaximumCoefficients> m_neutralPathing{};
    std::atomic<std::int64_t> m_neutralPathingReads{0};
    std::atomic<std::int64_t> m_validPathingReads{0};
    // M9.2: worker writes, mixer reads.
    TripleBuffer<SteamAudioPathingSnapshot> m_pathingChannel;
    std::atomic<bool> m_pathingAvailable{false};
    std::atomic<float> m_directVisibility{1.0f};
    std::atomic<PathingCapacity> m_pathingCapacity{PathingCapacity::NotRequested};
    std::atomic<int> m_pathingBatchShown{-1};
    std::atomic<bool> m_pathingSourceCovered{false};
    std::atomic<bool> m_pathingListenerCovered{false};
    std::atomic<float> m_pathingGainShown{0.0f};
    // M9.2: what shCoeffs points at for a valid result. Written only by the mixer reader in getOutputs and read by the plugin on that
    // same thread, and it lives as long as this object, which outlives every plugin reference (ReclaimRetiredSources). So the pointer is
    // valid however long the plugin keeps it, and never written while the plugin is reading it, whatever the plugin does between blocks (E8).
    alignas(16) std::array<float, SteamAudioPathingSnapshot::MaximumCoefficients> m_pathingOutput{};
};

// Verification only: called on the mixer for each read, between acquiring a snapshot and leaving it.
void SetSnapshotReadObserver(SteamAudioSnapshotReadObserver observer) noexcept;
// M9.2, verification only: called on the mixer after each pathing read, with what the plugin was handed.
void SetPathingReadObserver(SteamAudioPathingReadObserver observer) noexcept;

struct SnapshotCompatibility
{
    bool accepted = false;
    // Why not, for logs and diagnostics; or what was accepted.
    std::string detail;
    // Temporary sources the plugin never returned. Leaked deliberately: freeing one could leave a dangling vtable.
    std::size_t quarantined = 0;
};

// The fail-safe gate, in order: SHA-256 of every Steam Audio and FMOD module actually loaded against the pinned values, then
// a silent handshake on one real, unconnected spatializer. A binary mismatch never reaches the plugin with a SnapshotSource.
// Runs after the runtime started and before any bank. Main thread.
SnapshotCompatibility CheckSnapshotCompatibility(const SteamAudioRuntime& runtime, FMOD::Studio::System& studio,
    FMOD::System& core, SteamAudioInjectedFailure injectedFailure);
#endif
}
