#pragma once

#include "Renderer/ImageLoader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Physical sky S4, HDRI option A (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.11): an HDRI sky lights the frame
// from the sky's persistent resources instead of an environment of its own. A worker thread loads the HDRI, applies its
// calibration and rotation, and resamples it to RGBA16F equirects: the sky's 1024x512 set equirect (importance sampling, the
// cubes and every non-delta miss read it, so MIS stays consistent) and, at the 2048 and 4096 HDRI sky qualities, the background
// slot's equirect (camera rays, delta misses and the reflection cube). The renderer hands the texels to SkyAtmosphere, which
// uploads them through its persistent staging into resources allocated once at startup. Nothing here touches the GPU.
//
// Resampling preserves the integral of radiance over the sphere: a smaller target averages the source texels it overlaps,
// weighted by the overlap's solid angle; a larger target interpolates linearly. Rotation is a yaw about +Y (degrees; positive
// moves features toward increasing longitude, atan2(z, x)).
struct SkyHdriSource
{
    // As EnvironmentPreparationSource::load: runs on the worker, returns the uncalibrated RGBA image and sets its source identity
    // (the argument starts as `identity`). Throws on a missing or invalid source; the message becomes the request's error.
    std::function<HDRImageData(std::string& identity)> load;
    // The source identity when known without loading (an archive entry), else empty.
    std::string identity;
    std::string displayName;
    std::string reference;
    float radianceScale = 1.0f;
    float rotationDegrees = 0.0f;
    // Snap automatic exposure when the sky publishes it (scene activation).
    bool snapExposure = false;
};

// A prepared HDRI sky. Immutable once published by the worker; shared by the renderer and SkyAtmosphere.
struct SkyHdriTexels
{
    // The calibrated identity: source identity, radiance scale and rotation (SkyHdriIdentity).
    std::string identity;
    std::string displayName;
    // The sets' equirect, RGBA16F rows top (+Y) to bottom.
    unsigned int width = 0;
    unsigned int height = 0;
    std::vector<std::uint16_t> texels;
    // The background slot's equirect (empty at the 1024 quality).
    unsigned int backgroundWidth = 0;
    unsigned int backgroundHeight = 0;
    std::vector<std::uint16_t> background;
};

// The calibrated identity of a source at a radiance scale and rotation (exact bit patterns).
std::string SkyHdriIdentity(const std::string& sourceIdentity, float radianceScale, float rotationDegrees);

// Resamples an RGBA float equirect (alpha ignored, written as 1) to width x height RGBA16F, scaled and rotated as described above.
// Values are clamped to [0, 65504] and non-finite values become 0. False when cancelled (cancel may be empty).
bool ResampleSkyHdri(const HDRImageData& source, float radianceScale, float rotationDegrees, unsigned int width,
    unsigned int height, std::vector<std::uint16_t>& texels, const std::function<bool()>& cancel = {});

class SkyHdriJob
{
public:
    // width: the sets' equirect width (1024); backgroundWidth: the background slot's (0 at the 1024 quality). Heights are half.
    SkyHdriJob(std::uint64_t ticket, SkyHdriSource source, unsigned int width, unsigned int backgroundWidth);
    // Cancels and joins the worker. Owners keep superseded jobs until IsDone so this never waits on a load.
    ~SkyHdriJob();

    SkyHdriJob(const SkyHdriJob&) = delete;
    SkyHdriJob& operator=(const SkyHdriJob&) = delete;

    void Start();
    void Cancel() noexcept { m_cancelled.store(true, std::memory_order_relaxed); }
    bool IsDone() const noexcept { return m_done.load(std::memory_order_acquire); }
    // Valid after IsDone: the prepared texels, or null with the error set.
    std::shared_ptr<const SkyHdriTexels> TakeResult(std::string& error);

    const std::uint64_t ticket;
    const SkyHdriSource source;
    // The calibrated identity when the source identity is known, else empty.
    const std::string requestedIdentity;
    std::chrono::steady_clock::time_point requested;

private:
    void Run();

    const unsigned int m_width;
    const unsigned int m_backgroundWidth;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_done{false};
    std::shared_ptr<SkyHdriTexels> m_result;
    std::string m_error;
    std::thread m_worker;
};
