#pragma once

#include "Scene/PhotometricUnits.h"

#include <cmath>

// Scene-owned photometric calibration (PhotometricUnits.h), independent of the presence or energy of any light.
struct SceneLightingSettings
{
    // EV_ref: the fixed reference exposure (EV100) that sets the scene's GPU lighting pre-scale. Changing it
    // rescales every lighting value and restarts accumulation; runtime camera exposure never changes it.
    float referenceEV100 = Photometry::DefaultReferenceEV100;

    // Environment luminance, cd/m^2 per HDRI texel unit (luminance-weighted RGB). The default leaves texel values
    // unchanged on the GPU at the default reference exposure.
    float environmentLuminanceScale = Photometry::NeutralEnvironmentLuminanceScaleFloat(Photometry::DefaultReferenceEV100);

    bool operator==(const SceneLightingSettings&) const = default;
};

inline bool ValidateSceneLightingSettings(const SceneLightingSettings& settings, const char** reason = nullptr)
{
    const char* failure = nullptr;

    if(!std::isfinite(settings.referenceEV100) || settings.referenceEV100 < Photometry::MinimumReferenceEV100 ||
        settings.referenceEV100 > Photometry::MaximumReferenceEV100)
    {
        failure = "Scene reference exposure must be between -10 and 30 EV100.";
    }
    else if(!std::isfinite(settings.environmentLuminanceScale) || settings.environmentLuminanceScale < 0.0f)
    {
        failure = "Environment luminance scale must be finite and nonnegative.";
    }

    if(reason)
    {
        *reason = failure;
    }

    return failure == nullptr;
}

// The multiplier applied to HDRI texels on upload: environment luminance scale times the render pre-scale.
inline float EnvironmentRadianceMultiplier(const SceneLightingSettings& settings)
{
    return float(double(settings.environmentLuminanceScale) * Photometry::RenderScale(settings.referenceEV100));
}
