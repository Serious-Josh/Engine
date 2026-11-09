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

// M8.3 (SteamAudioReflectionBaker.cpp): bakes in a scene and batch of its own, on any one thread.
// `job`, when given, receives progress, and its cancellation reaches the SDK only from inside the bake's progress callback, the one
// place M8.1 found iplReflectionsBakerCancelBake safe. Never throws, and never returns partial data.
// `embree`, when not null, is the session's validated Embree device, and the bake's scene uses it; otherwise the default ray tracer.
// The caller keeps it alive for the call.
ReflectionBakeResult BakeReflectionLayers(IPLContext context, IPLEmbreeDevice embree, const ReflectionBakeRequest& request,
    ReflectionBakeJob* job = nullptr) noexcept;

// M9.4 (SteamAudioReflectionBaker.cpp): one pathing layer, with the same guarantees as BakeReflectionLayers except one: the bake itself is
// uninterruptible. iplPathBakerCancelBake crashes Steam Audio 4.8.1 however it is called, so cancellation is checked before and after the
// SDK call, and a cancelled result is discarded whole. Cancelling waits for the bake in progress.
PathingBakeResult BakePathingLayer(IPLContext context, IPLEmbreeDevice embree, const PathingBakeRequest& request,
    PathingBakeJob* job = nullptr) noexcept;

// M8.4: iplProbeBatchLoad and then iplProbeBatchCommit, which builds the lookup tree the simulator needs (M8.1: without it the
// first reflection run crashes). Null, with a reason, unless it loads with exactly `expectedProbes` probes.
IPLProbeBatch LoadCommittedProbeBatch(IPLContext context, std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error) noexcept;

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
