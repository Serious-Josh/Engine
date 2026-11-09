#include "Audio/AudioSystemInternal.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/SteamAudioSnapshotSource.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace Audio::Internal
{
namespace
{
    std::atomic<SteamAudioSnapshotReadObserver> readObserver{nullptr};
    std::atomic<SteamAudioPathingReadObserver> pathingReadObserver{nullptr};
    // simulation_data.cpp's initial pathing EQ (v4.8.1).
    constexpr float NeutralPathingEq = 0.1f;
    // Published EQ bands are raised to at least this. A band at exactly zero makes the path effect output NaN on every sample
    // (measured: EQ 1,0,1); 1e-6 and 1e-4 stayed finite. -80 dB, so nothing audible is lost.
    constexpr float MinimumPathingEq = 1e-4f;
    // The plugin renders at its own simulation order. Engine's is 1, so it reads four coefficients; the rest stay zero.
    constexpr int PathingCoefficients(int order) noexcept
    {
        return (order + 1) * (order + 1);
    }

    void CopyDirect(const SteamAudioDirectSnapshot& snapshot, IPLDirectEffectParams& direct) noexcept
    {
        direct.distanceAttenuation = snapshot.distanceAttenuation;
        direct.directivity = snapshot.directivity;
        direct.occlusion = snapshot.occlusion;

        for(int band = 0; band < IPL_NUM_BANDS; ++band)
        {
            direct.airAbsorption[band] = snapshot.airAbsorption[band];
            direct.transmission[band] = snapshot.transmission[band];
        }
    }

    // Exactly the binaries Option B was validated against: Steam Audio 4.8.1 (FMOD archive) and FMOD 2.04.00 build 168298,
    // in FMOD's production and logging variants. Moving any of these means revalidating (section 18.9).
    struct PinnedModule
    {
        const wchar_t* name;
        std::string_view sha256;
    };

    constexpr std::array<PinnedModule, 6> PinnedModules{{
        {L"phonon.dll", "957e8667d9d5b0e532334ec744c8897c9c09c4ec895d51a4a3faa3c8b033a2af"},
        {L"phonon_fmod.dll", "b59a67a467e71a065e5a02892ccffbe82de678bc712eab5fd9930cb60bf7cbed"},
        {L"fmod.dll", "dd928a636989bf6b3ff0bdd9e5cd7c713971062e7a929bcf7798c0a310057a8b"},
        {L"fmodL.dll", "1ac076b898fa847b78b9acaececab30b2379c91a12273473012fd04fea584550"},
        {L"fmodstudio.dll", "7ff9545fbe98144b60f260660118d8e3d109f2fe836357f990c0c190cef9ba1e"},
        {L"fmodstudioL.dll", "1c6e2c396d8c7ac2225aa9d3c5145929e5663219d34a78869f9e71ff3b90bf43"},
    }};

    std::string Narrow(const wchar_t* text)
    {
        std::string result;

        for(; *text != L'\0'; ++text)
        {
            result += static_cast<char>(*text);
        }

        return result;
    }

    // Empty when the file cannot be read or hashed.
    std::string Sha256OfFile(const std::wstring& path)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(file == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool hashed = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;

        std::vector<UCHAR> buffer(1 << 20);
        DWORD bytesRead = 0;

        while(hashed && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
        {
            hashed = BCryptHashData(hash, buffer.data(), bytesRead, 0) == 0;
        }

        std::array<UCHAR, 32> digest{};
        hashed = hashed && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;

        if(hash)
        {
            BCryptDestroyHash(hash);
        }

        if(algorithm)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }

        CloseHandle(file);

        if(!hashed)
        {
            return {};
        }

        constexpr std::string_view HexDigits = "0123456789abcdef";
        std::string text;

        for(const UCHAR byte : digest)
        {
            text += HexDigits[byte >> 4];
            text += HexDigits[byte & 0xF];
        }

        return text;
    }

    // Every pinned module loaded in this process must match, and one core and one Studio module must be among them.
    bool VerifyPinnedBinaries(std::string& reason)
    {
        int verified = 0;

        for(const PinnedModule& pinned : PinnedModules)
        {
            const HMODULE module = GetModuleHandleW(pinned.name);

            if(!module)
            {
                continue;
            }

            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            path.resize(length);

            if(Sha256OfFile(path) != pinned.sha256)
            {
                reason = Narrow(pinned.name) + " is not the validated binary";
                return false;
            }

            ++verified;
        }

        // phonon, phonon_fmod, and the FMOD core and Studio pair.
        if(verified != 4)
        {
            reason = "expected 4 validated modules loaded, found " + std::to_string(verified);
            return false;
        }

        return true;
    }

    // Retention and release through the plugin's real paths, on a spatializer connected to nothing: no block ever runs on
    // it, and no event or ordinary FMOD sound is involved. DSP::release is deferred, so the source lives until the plugin
    // demonstrably lets go. If it never does, the source is quarantined, never freed.
    bool RunHandshake(const SteamAudioRuntime& runtime, FMOD::Studio::System& studio, FMOD::System& core,
        SteamAudioInjectedFailure injectedFailure, std::string& reason, std::size_t& quarantined)
    {
        constexpr std::chrono::seconds ReleaseLimit{2};

        FMOD::DSP* spatializer = runtime.CreateUnconnectedSpatializer(core);

        if(!spatializer)
        {
            reason = "the handshake could not create a spatializer";
            return false;
        }

        auto source = std::make_unique<SnapshotSource>();

        const int afterBind = runtime.BindSimulationSourceForHandshake(*spatializer, source->AsIPLSource());

        if(injectedFailure == SteamAudioInjectedFailure::HandshakeNoRelease)
        {
            // Models a plugin that keeps a reference: the wait below must end in quarantine, not in a free.
            source->retain();
        }

        spatializer->release();
        const auto deadline = std::chrono::steady_clock::now() + ReleaseLimit;

        while(source->References() != 1 && std::chrono::steady_clock::now() < deadline)
        {
            studio.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        const int afterRelease = source->References();
        const std::int64_t anomalies = source->Anomalies();

        if(afterRelease != 1)
        {
            static_cast<void>(source.release());
            ++quarantined;
            reason = "the plugin kept a handshake reference (" + std::to_string(afterRelease) + ")";
            return false;
        }

        // Owner + DSP while bound, with the registry's reference already gone: 2.
        if(afterBind != 2 || anomalies != 0)
        {
            reason = "unexpected handshake references (" + std::to_string(afterBind) + ", " + std::to_string(anomalies) +
                " anomalies)";
            return false;
        }

        return true;
    }
}

api::ISource* SnapshotSource::retain()
{
    m_references.fetch_add(1, std::memory_order_relaxed);
    return this;
}

void SnapshotSource::release()
{
    // A count reaching zero is an accounting failure and never a destruction: the owner's reference is always there.
    if(m_references.fetch_sub(1, std::memory_order_acq_rel) <= 1)
    {
        m_anomalies.fetch_add(1, std::memory_order_relaxed);
    }
}

void SnapshotSource::add(api::ISimulator*)
{
    m_anomalies.fetch_add(1, std::memory_order_relaxed);
}

void SnapshotSource::remove(api::ISimulator*)
{
    m_anomalies.fetch_add(1, std::memory_order_relaxed);
}

void SnapshotSource::setInputs(IPLSimulationFlags, IPLSimulationInputs*)
{
    m_anomalies.fetch_add(1, std::memory_order_relaxed);
}

bool SnapshotSource::PublishPathing(const SteamAudioPathingSnapshot& result) noexcept
{
    SteamAudioPathingSnapshot& slot = m_pathingChannel.WriteSlot();
    bool unaltered = true;
    const int order = std::clamp(result.order, 0, SteamAudioPathingSnapshot::MaximumOrder);
    const int coefficients = PathingCoefficients(order);
    bool finite = true;

    for(int index = 0; index < coefficients; ++index)
    {
        finite = finite && std::isfinite(result.sh[index]);
    }

    for(int band = 0; band < IPL_NUM_BANDS; ++band)
    {
        finite = finite && std::isfinite(result.eq[band]);
    }

    slot = SteamAudioPathingSnapshot{};
    slot.generation = result.generation;
    slot.order = order;

    if(result.valid && finite)
    {
        slot.valid = true;

        for(int band = 0; band < IPL_NUM_BANDS; ++band)
        {
            slot.eq[band] = std::max(result.eq[band], MinimumPathingEq);
            unaltered = unaltered && slot.eq[band] == result.eq[band];
        }

        for(int index = 0; index < coefficients; ++index)
        {
            slot.sh[index] = result.sh[index];
        }
    }
    else
    {
        unaltered = !result.valid;
    }

    unaltered = unaltered && order == result.order;
    const bool available = slot.valid;
    m_pathingChannel.Publish();
    m_pathingAvailable.store(available, std::memory_order_release);
    return unaltered;
}

// FMOD's mixer thread. A few atomic operations and short copies: no loop over anything unbounded, no wait, allocation or free.
void SnapshotSource::getOutputs(IPLSimulationFlags flags, IPLSimulationOutputs* outputs)
{
    // Never a null shCoeffs (M9.1). The plugin asks for pathing on every block of a bound source, and runs its path effect whenever
    // Apply Pathing is on. Zero Ambisonic coefficients are silence. The EQ is never zero: the path effect then outputs NaN on every
    // sample, which would spread through the whole mix. 0.1 is what Steam Audio itself gives a source with no pathing result yet.
    // M9.2: a valid published result is copied into m_pathingOutput, which shCoeffs then points at; PublishPathing kept its EQ positive.
    if((flags & IPL_SIMULATIONFLAGS_PATHING) != 0)
    {
        bool valid = false;

        // One reader at a time, as for direct: a second one gets silence and leaves the channel and the output buffer alone.
        if(!m_readerBusy.exchange(true, std::memory_order_acquire))
        {
            const SteamAudioPathingSnapshot& snapshot = m_pathingChannel.AcquireLatest();
            valid = snapshot.valid;

            if(valid)
            {
                for(int band = 0; band < IPL_NUM_BANDS; ++band)
                {
                    outputs->pathing.eqCoeffs[band] = snapshot.eq[band];
                }

                m_pathingOutput = std::to_array(snapshot.sh);
                outputs->pathing.shCoeffs = m_pathingOutput.data();
            }

            m_readerBusy.store(false, std::memory_order_release);
        }
        else
        {
            m_anomalies.fetch_add(1, std::memory_order_relaxed);
        }

        if(!valid)
        {
            for(int band = 0; band < IPL_NUM_BANDS; ++band)
            {
                outputs->pathing.eqCoeffs[band] = NeutralPathingEq;
            }

            outputs->pathing.shCoeffs = m_neutralPathing.data();
        }

        (valid ? m_validPathingReads : m_neutralPathingReads).fetch_add(1, std::memory_order_relaxed);

        if(const SteamAudioPathingReadObserver observer = pathingReadObserver.load(std::memory_order_relaxed))
        {
            observer(outputs->pathing.eqCoeffs, outputs->pathing.shCoeffs, SteamAudioPathingSnapshot::MaximumCoefficients, valid, this);
        }
    }

    // Pointer and sizes only: convolution reads the IR through Steam Audio's own triple buffer. Without a published IR the
    // outputs stay as the plugin zeroed them. The plugin overwrites type and sizes from its settings anyway; these were checked
    // against those at start.
    if((flags & IPL_SIMULATIONFLAGS_REFLECTIONS) != 0)
    {
        const IPLReflectionEffectIR ir = m_ir.load(std::memory_order_acquire);

        if(ir && !m_irWithheld.load(std::memory_order_relaxed))
        {
            outputs->reflections.ir = ir;
            outputs->reflections.numChannels = m_irChannels;
            outputs->reflections.irSize = m_irSamples;
            m_irReads.fetch_add(1, std::memory_order_relaxed);

            if(m_irReleased.load(std::memory_order_relaxed))
            {
                m_anomalies.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if((flags & IPL_SIMULATIONFLAGS_DIRECT) == 0)
    {
        return;
    }

    if(m_readerBusy.exchange(true, std::memory_order_acquire))
    {
        m_anomalies.fetch_add(1, std::memory_order_relaxed);
        CopyDirect(SteamAudioDirectSnapshot{}, outputs->direct);
        return;
    }

    const SteamAudioDirectSnapshot& snapshot = m_channel.AcquireLatest();
    CopyDirect(snapshot, outputs->direct);

    if(const SteamAudioSnapshotReadObserver observer = readObserver.load(std::memory_order_relaxed))
    {
        observer(snapshot, this);
    }

    m_readerBusy.store(false, std::memory_order_release);
}

void SetSnapshotReadObserver(SteamAudioSnapshotReadObserver observer) noexcept
{
    readObserver.store(observer, std::memory_order_relaxed);
}

void SetPathingReadObserver(SteamAudioPathingReadObserver observer) noexcept
{
    pathingReadObserver.store(observer, std::memory_order_relaxed);
}

SnapshotCompatibility CheckSnapshotCompatibility(const SteamAudioRuntime& runtime, FMOD::Studio::System& studio,
    FMOD::System& core, SteamAudioInjectedFailure injectedFailure)
{
    SnapshotCompatibility result;

    if(injectedFailure == SteamAudioInjectedFailure::BinaryIdentity)
    {
        result.detail = "injected binary identity failure";
        return result;
    }

    if(!VerifyPinnedBinaries(result.detail))
    {
        return result;
    }

    if(injectedFailure == SteamAudioInjectedFailure::HandshakeRefused)
    {
        result.detail = "injected handshake failure";
        return result;
    }

    if(!RunHandshake(runtime, studio, core, injectedFailure, result.detail, result.quarantined))
    {
        return result;
    }

    result.accepted = true;
    result.detail = "validated binaries and plugin handshake";
    return result;
}
}
#endif
