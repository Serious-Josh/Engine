#pragma once

// Private to the audio module: M7 probe generation and the loaded batches of a gameplay session. See SteamAudioIntegration.md, section 32.
//
// Nothing here touches the simulator. A batch reaches reflections only through iplSimulatorAddProbeBatch, which belongs to M8 and is
// called nowhere in M7, so loading probes cannot activate baked reflections.

#include "Audio/AudioSystem.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include <phonon.h>

#include <cstddef>
#include <vector>

namespace Audio::Internal
{
// Blocks for as long as the scene takes, on any one thread. Builds and releases its own temporary scene, separate from the simulation's.
// It shares only the context with other threads, and the context's allocator accepts calls from any thread (SteamAudioMixerAllocator).
// `job`, when given, receives progress and is checked for cancellation between SDK calls. Never throws.
ProbeGenerationResult GenerateProbeBatch(IPLContext context, const ProbeGenerationRequest& request, ProbeGenerationJob* job = nullptr) noexcept;

// Main thread. Every batch is released by Clear, and by the destructor before the context goes.
class SteamAudioProbeBatches final
{
public:
    SteamAudioProbeBatches() = default;
    ~SteamAudioProbeBatches();
    SteamAudioProbeBatches(const SteamAudioProbeBatches&) = delete;
    SteamAudioProbeBatches& operator=(const SteamAudioProbeBatches&) = delete;

    // The caller has already verified the bytes (ProbeBatchAssets::Decode): iplProbeBatchLoad is not safe on malformed data.
    bool Load(IPLContext context, std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error);
    void Clear() noexcept;
    ProbeBatchStats GetStats() const noexcept;

private:
    std::vector<IPLProbeBatch> m_batches;
    ProbeBatchStats m_stats;
};
}
#endif
