#pragma once

#include "Renderer/EnvironmentMap.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/Passes/DiffuseIrradiancePass.h"
#include "Renderer/Passes/EnvironmentMipPass.h"
#include "Renderer/Passes/EquirectangularToCubemapPass.h"
#include "Renderer/Passes/GGXMultiscatterIrradiancePass.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <d3d12.h>
#include <wrl/client.h>

// S0.5 asynchronous scene environment preparation.
//
// A request moves through three owners, and only the newest request may publish:
//   1. Worker (one std::thread per job): loads the image, applies the radiance calibration, builds the importance CDFs and
//      writes the texels and CDFs into an UPLOAD-heap staging buffer it creates. It touches no descriptor heap, command
//      list, memory registry or renderer state; the main thread reads its result only after observing `cpuDone`.
//   2. Main thread, inside ordinary frame recordings: creates the destination resources and precompute passes, then records
//      the staging copies and the precompute draws a budgeted amount per frame. Nothing it creates is reachable by
//      rendering until publication.
//   3. Publication, at the start of a later frame, after every recorded step was submitted on the frame queue (which orders
//      it before any later frame): the renderer's environment is swapped with the prepared one. The previous environment,
//      the passes and the staging buffer retire when the fence of the last submission that may reference them completes.
//
// Logical cancellation (supersession, scene changes, shutdown) and physical completion are separate: a cancelled worker
// finishes on its own before its job is released, and cancelled GPU-referenced resources retire behind their fence.
struct EnvironmentPreparationSource
{
    // Runs on the worker. Returns the uncalibrated image and sets its source identity (the identity argument starts as
    // `identity` below). Throws on a missing or invalid source; the message becomes the request's error.
    std::function<HDRImageData(std::string& identity)> load;
    // The source identity when it is known without loading (archive, neutral, None): an unchanged request finishes at
    // once and a request equal to the one in flight joins it. Empty when only the load can establish it.
    std::string identity;
    std::string displayName;
    std::string reference;
    float radianceScale = 1.0f;
    // Snap automatic exposure in the frame that publishes (scene activation: the new HDR is unrelated to the adapted one).
    bool snapExposure = false;
};

enum class EnvironmentPreparationStage
{
    Cpu,
    Copy,
    MipChain,
    DiffuseIrradiance,
    Cubemap,
    Multiscatter,
    Recorded,
};

class EnvironmentPreparationJob
{
public:
    // Set by the worker before cpuDone (release); read by the main thread after it (acquire).
    struct CpuResult
    {
        std::string sourceIdentity;
        // The calibrated identity matched the published environment when the job started: nothing was staged.
        bool unchanged = false;
        unsigned int width = 0;
        unsigned int height = 0;
        float totalWeight = 0.0f;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureFootprint{};
        unsigned int textureRows = 0;
        std::uint64_t conditionalOffset = 0;
        std::uint64_t conditionalBytes = 0;
        std::uint64_t marginalOffset = 0;
        std::uint64_t marginalBytes = 0;
        std::uint64_t workerNanoseconds = 0;
        std::string error;
        // The destination resources, created by the worker without views or memory registration.
        std::unique_ptr<EnvironmentMap> environment;
    };

    EnvironmentPreparationJob(std::uint64_t ticket, EnvironmentPreparationSource source, std::string publishedIdentity);
    // Requests cancellation and joins the worker. Callers release GPU-referenced members only behind their fence.
    ~EnvironmentPreparationJob();

    EnvironmentPreparationJob(const EnvironmentPreparationJob&) = delete;
    EnvironmentPreparationJob& operator=(const EnvironmentPreparationJob&) = delete;

    void Start(ID3D12Device* device);
    void Cancel() noexcept { m_cancelled.store(true, std::memory_order_relaxed); }
    bool IsCpuDone() const noexcept { return m_cpuDone.load(std::memory_order_acquire); }
    // Joins a finished worker; valid only after IsCpuDone.
    CpuResult& TakeCpuResult();

    std::uint64_t ticket = 0;
    EnvironmentPreparationSource source;
    // The calibrated known identity (empty when the load establishes it).
    std::string requestedIdentity;
    std::string calibratedIdentity;
    std::uint32_t attempts = 1;

    // Main-thread GPU stage. Unreachable by rendering until publication.
    EnvironmentPreparationStage stage = EnvironmentPreparationStage::Cpu;
    std::unique_ptr<EnvironmentMap> environment;
    EnvironmentMipPass mipPass;
    EquirectangularToCubemapPass cubemapPass;
    DiffuseIrradiancePass diffusePass;
    GGXMultiscatterIrradiancePass multiscatterPass;
    // Copy cursor: texture rows, then conditional bytes, then marginal bytes.
    unsigned int copyRow = 0;
    std::uint64_t copyConditional = 0;
    std::uint64_t copyMarginal = 0;
    // Draw cursor within the current stage.
    unsigned int mip = 0;
    unsigned int face = 0;
    unsigned int row = 0;
    // Recording bookkeeping: commands recorded in the frame now being built, the last confirmed submission that may
    // reference this job's resources, and whether a submission's outcome is unknown (resources then live until shutdown).
    bool recordedUncommitted = false;
    bool copiesFinishedUncommitted = false;
    std::uint64_t lastSubmissionFence = 0;
    bool submissionUncertain = false;
    std::chrono::steady_clock::time_point requested;

    void ShutdownGpuObjects();

private:
    void Run(ID3D12Device* device);

    std::string m_publishedIdentity;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_cpuDone{false};
    CpuResult m_result;
    std::thread m_worker;
};
