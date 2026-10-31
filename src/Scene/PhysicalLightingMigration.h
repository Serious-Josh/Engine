#pragma once

#include "Scene/PhotometricUnits.h"
#include "Scene/SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Codec boundary only (scene schema < 14, cooked payload < 14): converts the historical arbitrary light units to
// physical ones. Never called for a current document.
//
// Legacy GPU values were color * intensity: Point/Spot as on-axis intensity at 1 m (inverse-square, faded to zero at
// range), Directional as irradiance. The physical model reproduces color * intensity exactly at the chosen reference
// exposure EV_ref (render pre-scale S, PhotometricUnits.h); ranges are kept with range attenuation enabled, the
// same fade, so appearance is unchanged up to float rounding:
//   Point:       flux        = 4 pi * v * Y(c) / S
//   Spot:        flux        = SpotEmissionSolidAngle(inner, outer) * v * Y(c) / S
//   Directional: illuminance = v * Y(c) / S
// with v the legacy intensity and Y(c) the Rec.709 luminance of the legacy colour. The colour is kept as the tint.
// A legacy Directional colour above 1 is divided by its largest channel (moved into v, an exact rewrite of the
// same product); a negative channel has no physical meaning and is rejected. The environment calibration becomes
// the scale that leaves HDRI texel values unchanged on the GPU. ambientStrength was never read and is dropped.
// EV_ref is a calibration assumption: it decides how plausible the physical numbers are, not the appearance.

struct LegacyLightConversion
{
    std::string objectID;
    std::string name;
    LightType type = LightType::Point;
    Vector3 legacyColor;
    float legacyIntensity = 0.0f;
    // Point/Spot only; NaN when the light had none (Directional).
    float legacyRange = 0.0f;
    Vector3 color;
    float intensity = 0.0f;
};

struct LegacyLightingReport
{
    std::uint32_t sourceSchemaVersion = 0;
    float referenceEV100 = 0.0f;
    float legacyAmbientStrength = 0.0f;
    float environmentLuminanceScale = 0.0f;
    std::vector<LegacyLightConversion> lights;
};

struct LegacyLightingMigrationOptions
{
    float referenceEV100 = Photometry::DefaultReferenceEV100;
    // Filled when not null.
    LegacyLightingReport* report = nullptr;
};

// Historical light values as decoded, before conversion: the old validation rules, never clamped.
inline void ValidateLegacyLightValues(const LightComponent& light, float legacyRange)
{
    const auto finite3 = [](const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };

    if(!finite3(light.color) || !std::isfinite(light.intensity) || light.intensity < 0.0f)
    {
        throw std::runtime_error("Legacy light colour and intensity must be finite and nonnegative.");
    }

    if(light.type != LightType::Directional)
    {
        if(light.color.x < 0.0f || light.color.y < 0.0f || light.color.z < 0.0f || light.color.x > 1.0f ||
            light.color.y > 1.0f || light.color.z > 1.0f || light.intensity > 1000000.0f)
        {
            throw std::runtime_error("Legacy Point/Spot colour must be in [0, 1] and intensity at most 10^6.");
        }

        if(!std::isfinite(legacyRange) || legacyRange < 0.01f || legacyRange > 10000.0f)
        {
            throw std::runtime_error("Legacy light range must be between 0.01 and 10000 meters.");
        }
    }
}

// Legacy-unit values of one light, as the codec decoded them, keyed by its document object.
struct LegacyLightValues
{
    std::string objectID;
    float range = std::numeric_limits<float>::quiet_NaN();
};

// Converts every Light of a decoded historical document and its scene calibration. legacyRanges lists the
// decoded Point/Spot ranges for the report (a Directional or a migrated legacy sun has none).
inline void MigrateLegacyLightUnits(SceneDocument& document, std::uint32_t sourceSchemaVersion, float legacyAmbientStrength,
    const std::vector<LegacyLightValues>& legacyRanges, const LegacyLightingMigrationOptions& options)
{
    const float referenceEV100 = options.referenceEV100;

    if(!std::isfinite(referenceEV100) || referenceEV100 < Photometry::MinimumReferenceEV100 ||
        referenceEV100 > Photometry::MaximumReferenceEV100)
    {
        throw std::runtime_error("Legacy lighting migration needs a reference exposure between -10 and 30 EV100.");
    }

    const double renderScale = Photometry::RenderScale(referenceEV100);
    LegacyLightingReport report;
    report.sourceSchemaVersion = sourceSchemaVersion;
    report.referenceEV100 = referenceEV100;
    report.legacyAmbientStrength = legacyAmbientStrength;
    document.lightingSettings.referenceEV100 = referenceEV100;
    document.lightingSettings.environmentLuminanceScale = Photometry::NeutralEnvironmentLuminanceScaleFloat(referenceEV100);
    report.environmentLuminanceScale = document.lightingSettings.environmentLuminanceScale;

    for(auto& object : document.objects)
    {
        if(!object.light)
        {
            continue;
        }

        LightComponent& light = *object.light;
        LegacyLightConversion entry;
        entry.objectID = object.id;
        entry.name = object.name;
        entry.type = light.type;
        entry.legacyColor = light.color;
        entry.legacyIntensity = light.intensity;
        entry.legacyRange = std::numeric_limits<float>::quiet_NaN();

        for(const auto& legacy : legacyRanges)
        {
            if(legacy.objectID == object.id)
            {
                entry.legacyRange = legacy.range;
            }
        }

        Vector3 color = light.color;
        double value = light.intensity;

        if(color.x < 0.0f || color.y < 0.0f || color.z < 0.0f)
        {
            throw std::runtime_error("Light " + object.id + " has a negative legacy colour channel, which has no "
                "physical meaning; correct it in a schema 13 build before migrating.");
        }

        const float largest = (std::max)({color.x, color.y, color.z});

        if(largest > 1.0f)
        {
            color = color * (1.0f / largest);
            value *= largest;
        }

        double factor = 1.0;

        if(light.type == LightType::Point)
        {
            factor = 4.0 * Photometry::Pi;
        }
        else if(light.type == LightType::Spot)
        {
            factor = Photometry::SpotEmissionSolidAngle(light.innerConeAngle, light.outerConeAngle);
        }

        const double physical = factor * value * Photometry::Luminance(color) / renderScale;
        light.color = color;
        light.intensity = float(physical);
        std::string error;

        if(!std::isfinite(light.intensity) || !ValidateLightComponent(light, &error))
        {
            throw std::runtime_error("Light " + object.id + " cannot be expressed in physical units: " + error);
        }

        entry.color = light.color;
        entry.intensity = light.intensity;
        report.lights.push_back(std::move(entry));
    }

    if(options.report)
    {
        *options.report = std::move(report);
    }
}
