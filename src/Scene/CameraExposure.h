#pragma once

#include "Scene/PhotometricUnits.h"

#include <cmath>
#include <cstdint>

// Photographic camera exposure (Physical Lighting & Exposure, Checkpoint 2). Exposure only scales the HDR image
// immediately before tone mapping; it never changes lighting, accumulation, SHARC or denoiser inputs.
//
// Equations (ISO 2720 reflected-light calibration, K = 12.5; PhotometricUnits.h):
//   EV100  = log2(N^2 / t) - log2(ISO / 100)                  (aperture N, shutter t seconds)
//   EV_eff = EV100 - compensation                              (positive compensation brightens)
//   The camera maps physical luminance L to tone-mapper input L * H, H = (0.18 * 100 / K) * 2^-EV_eff, so a metered
//   mid-grey lands at 0.18. GPU lighting is already pre-scaled by S = (0.18 * 100 / K) * 2^-EV_ref, hence the
//   multiplier applied to GPU values is H / S = 2^(EV_ref - EV_eff): exactly 1 when EV_eff == EV_ref.
// Auto exposure meters physical luminance (GPU value / S) and targets EV100 = log2(L_avg * 100 / K), the EV at
// which that average maps to mid-grey; compensation then applies as above.
enum class ExposureMode : std::uint32_t
{
    Manual = 0,
    Auto = 1
};

struct CameraExposure
{
    static constexpr float MinimumAperture = 0.5f;
    static constexpr float MaximumAperture = 128.0f;
    static constexpr float MinimumShutter = 1.0e-6f;
    static constexpr float MaximumShutter = 3600.0f;
    static constexpr float MinimumIso = 1.0f;
    static constexpr float MaximumIso = 1.0e6f;
    static constexpr float MaximumCompensation = 20.0f;
    static constexpr float MinimumMeteringEV = -20.0f;
    static constexpr float MaximumMeteringEV = 40.0f;
    static constexpr float MaximumAdaptationSpeed = 100.0f;

    // Auto by default; saved scenes always store their mode, so this only decides new cameras, new scenes and migration.
    ExposureMode mode = ExposureMode::Auto;

    // Manual: f/4, 1/64 s, ISO 100 is exactly EV100 10, the default scene reference exposure.
    float aperture = 4.0f;
    float shutterSeconds = 1.0f / 64.0f;
    float iso = 100.0f;

    // EV; applies in both modes.
    float compensation = 0.0f;

    // Auto: the metered range of the histogram (EV100), the averaged percentile band (fractions of valid pixels),
    // and exponential adaptation rates (1/s) towards a brighter image (lower EV) and a darker one (higher EV).
    float meteringMinEV100 = -6.0f;
    float meteringMaxEV100 = 20.0f;
    float lowPercentile = 0.5f;
    float highPercentile = 0.9f;
    float brighteningSpeed = 3.0f;
    float darkeningSpeed = 1.0f;
    // Auto: hold the current adapted exposure.
    bool locked = false;

    bool operator==(const CameraExposure&) const = default;
};

namespace Photometry
{
    inline double ManualEV100(const CameraExposure& exposure)
    {
        const double aperture = exposure.aperture;
        return std::log2(aperture * aperture / double(exposure.shutterSeconds)) - std::log2(double(exposure.iso) / 100.0);
    }

    // The multiplier from GPU values to tone-mapper input for an effective exposure.
    inline double ExposureMultiplier(double referenceEV100, double effectiveEV100)
    {
        return std::exp2(referenceEV100 - effectiveEV100);
    }

    // EV100 at which physical luminance (cd/m^2) maps to mid-grey: log2(L * 100 / K).
    inline double MeteredEV100(double luminance)
    {
        return std::log2(luminance * 100.0 / MeterCalibrationK);
    }

    // The manual default whose EV100 is exactly referenceEV100 (f/4, ISO 100, shutter 16 * 2^-EV_ref: powers of two),
    // so a scene renders exactly at its reference exposure until the camera is changed.
    inline CameraExposure ReferenceCameraExposure(float referenceEV100)
    {
        CameraExposure exposure;
        exposure.shutterSeconds = float(16.0 * std::exp2(-double(referenceEV100)));
        return exposure;
    }
}

inline bool ValidateCameraExposure(const CameraExposure& exposure, const char** reason = nullptr)
{
    const auto finite = [](float value) { return std::isfinite(value); };
    const char* failure = nullptr;

    if(exposure.mode != ExposureMode::Manual && exposure.mode != ExposureMode::Auto)
    {
        failure = "Exposure mode must be Manual or Auto.";
    }
    else if(!finite(exposure.aperture) || exposure.aperture < CameraExposure::MinimumAperture ||
        exposure.aperture > CameraExposure::MaximumAperture)
    {
        failure = "Aperture must be between f/0.5 and f/128.";
    }
    else if(!finite(exposure.shutterSeconds) || exposure.shutterSeconds < CameraExposure::MinimumShutter ||
        exposure.shutterSeconds > CameraExposure::MaximumShutter)
    {
        failure = "Shutter speed must be between 1 microsecond and 1 hour.";
    }
    else if(!finite(exposure.iso) || exposure.iso < CameraExposure::MinimumIso || exposure.iso > CameraExposure::MaximumIso)
    {
        failure = "ISO must be between 1 and 1000000.";
    }
    else if(!finite(exposure.compensation) || std::abs(exposure.compensation) > CameraExposure::MaximumCompensation)
    {
        failure = "Exposure compensation must be between -20 and 20 EV.";
    }
    else if(!finite(exposure.meteringMinEV100) || !finite(exposure.meteringMaxEV100) ||
        exposure.meteringMinEV100 < CameraExposure::MinimumMeteringEV ||
        exposure.meteringMaxEV100 > CameraExposure::MaximumMeteringEV ||
        !(exposure.meteringMaxEV100 - exposure.meteringMinEV100 >= 1.0f))
    {
        failure = "The metering range must span at least 1 EV within -20 to 40 EV100.";
    }
    else if(!finite(exposure.lowPercentile) || !finite(exposure.highPercentile) || exposure.lowPercentile < 0.0f ||
        exposure.highPercentile > 1.0f || !(exposure.highPercentile > exposure.lowPercentile))
    {
        failure = "Metering percentiles must satisfy 0 <= low < high <= 1.";
    }
    else if(!finite(exposure.brighteningSpeed) || !finite(exposure.darkeningSpeed) || exposure.brighteningSpeed <= 0.0f ||
        exposure.darkeningSpeed <= 0.0f || exposure.brighteningSpeed > CameraExposure::MaximumAdaptationSpeed ||
        exposure.darkeningSpeed > CameraExposure::MaximumAdaptationSpeed)
    {
        failure = "Adaptation speeds must be positive and at most 100 per second.";
    }

    if(reason)
    {
        *reason = failure;
    }

    return failure == nullptr;
}
