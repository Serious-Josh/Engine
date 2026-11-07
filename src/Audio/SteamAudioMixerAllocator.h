#pragma once

// Private to the audio module (M6.6): what Steam Audio allocates while FMOD mixes never reaches a general-purpose heap.
//
// Why this exists: the pinned plugin creates its effects lazily, inside the DSP's first PERFORM, so 58 to 189 allocations per DSP
// happen on the mixer. The plugin cannot be changed and has no earlier hook, but every one of them goes through the IPLContext's
// allocation callbacks.
//
// Contract, verified against Steam Audio 4.8.1:
// * The callbacks have no user data, and the SDK keeps them in one process-wide static that each iplContextCreate overwrites. Engine
//   already allows one context per process, so one process-wide allocator matches the SDK's own ownership. Its code lives in the
//   executable and so outlives phonon.dll, the plugin and the context.
// * A null return makes the SDK throw OutOfMemory. The plugin does not survive every such failure: its reflection branch tests
//   `initFlags && INIT_AMBISONICSEFFECT`, so a failed decode effect is used anyway. This allocator therefore never returns null for
//   lack of pool space. Exhaustion falls back to the heap, counted and logged: slower, never unsafe. This is a safety compromise, not
//   a real-time guarantee: the supported workload (see SteamAudioRuntime) does not reach it, and nothing stops a project exceeding
//   that. Admission control cannot close the gap, because the plugin's own channel changes re-create effects on a running DSP.
//
// Threads: Allocate and Free are called from any thread. Inside a mix (EnterMix/LeaveMix, from FMOD's PREMIX and POSTMIX) they take no
// lock and make no heap call in normal operation; outside one, Allocate uses the heap. A heap block freed inside a mix goes on a
// list that Drain empties on the main thread. Pool metadata is out of band, so freed blocks can hold anything, and a double free
// is detected rather than corrupting a list.
//
// Lifetime: a region stops serving at Disarm. Its memory goes when no block is outstanding AND no call is inside it, which Disarm,
// Drain and Arm each check without waiting. Blocks returned late therefore free their region on a later Drain or Arm. Arm fails
// only if four regions are all still held, which needs the SDK to have kept blocks across four sessions.

#include "Audio/AudioSystem.h"

#include <array>
#include <cstddef>

namespace Audio::Internal::SteamAudioMixerAllocator
{
    // 64 bytes to 8 MiB, in steps of a power of two and one and a half times it; each a multiple of the 64-byte alignment.
    inline constexpr int ClassCount = 34;
    using ClassCounts = std::array<std::size_t, ClassCount>;

    std::size_t ClassSize(int classIndex) noexcept;

    void* Allocate(std::size_t size, std::size_t alignment);
    void Free(void* block);

    void EnterMix() noexcept;
    void LeaveMix() noexcept;

    // Main thread. Between these two, this thread's allocations are tallied per class to give the demand of one DSP.
    void BeginProfile() noexcept;
    ClassCounts EndProfile() noexcept;

    // Main thread, before any bank. False leaves everything on the heap, which is the state before Arm in any case.
    bool Arm(const ClassCounts& blocks, bool fillFreedBlocks);
    // Returns heap blocks freed during a mix to the heap, and reclaims earlier sessions' regions now unused. Main thread.
    void Drain() noexcept;
    // After the context and plugin are released. Blocks the SDK has not returned keep their memory mapped, so nothing can fault.
    void Disarm() noexcept;

    SteamAudioMixerPoolStats GetStats() noexcept;
}
