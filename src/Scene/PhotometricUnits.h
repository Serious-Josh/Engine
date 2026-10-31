#pragma once

#include "Math/Vector3.h"

#include <algorithm>
#include <cmath>

// Engine's physical lighting convention, shared by authoring, the scene codecs and every rendering path.
//
// Authored units:
// - Point and Spot Lights: luminous flux, lumens (lm). Point emits isotropically, I = flux / (4 pi) cd. Spot
//   flux is normalized over its actual emission profile (full on-axis inside the inner cone, the renderer's
//   squared linear-in-cosine falloff to zero at the outer cone), so I_axis = flux / SpotEmissionSolidAngle.
// - Directional Lights and the sun: illuminance on a surface facing the light, lux (lx). The sun disk has
//   uniform radiance L = E / (pi sin^2 alpha), which integrates exactly to E at normal incidence.
// - Environment maps: luminance in cd/m^2 = texel RGB (luminance-weighted) * the scene's environment
//   luminance scale (cd/m^2 per texel unit).
// - Emissive surfaces (future): luminance, cd/m^2 (nits), with the same unit-luminance colour convention.
//
// Colour: linear Rec.709/sRGB primaries, D65 white. Light colour is a tint only: the renderer divides it by its
// Rec.709 luminance Y = 0.2126 R + 0.7152 G + 0.0722 B, so the authored photometric value alone sets brightness.
// Every channel weight is at least 0.0722, so for any nonzero tint in [0, 1] each normalized channel is at most
// 1 / 0.0722 (about 13.9): saturated and near-black tints stay finite. A black tint emits nothing.
//
// Limitations of the RGB photometric approximation: there is no spectral data. "Lumens" and "lux" of a coloured
// light are defined by the luminance of its RGB, which is exact only within this three-primary rendering model;
// metamerism and out-of-gamut spectra are not represented. No luminous efficacy is assumed anywhere, because
// nothing is expressed in watts.
//
// Rendering scale: GPU lighting values are physical luminance-weighted RGB times a fixed per-scene pre-scale
// S = (0.18 * 100 / K) * 2^-EV_ref, with K = 12.5 the ISO 2720 reflected-light meter calibration constant and
// EV_ref the scene's reference exposure (EV100). At EV_ref a metered mid-grey luminance lands at 0.18, which keeps
// fp16 buffers in their accurate range. S is a scene property: changing EV_ref changes every lighting value (and
// restarts accumulation); runtime exposure does not.
namespace Photometry
{
    inline constexpr double Pi = 3.14159265358979323846;
    inline constexpr double MeterCalibrationK = 12.5;
    inline constexpr double MeteredMidGrey = 0.18;
    inline constexpr float DefaultReferenceEV100 = 10.0f;
    inline constexpr float MinimumReferenceEV100 = -10.0f;
    inline constexpr float MaximumReferenceEV100 = 30.0f;

    // Rec.709 luminance of a linear RGB tint.
    inline double Luminance(const Vector3& color)
    {
        return 0.2126 * double(color.x) + 0.7152 * double(color.y) + 0.0722 * double(color.z);
    }

    // The tint scaled to unit luminance, or black when it has no luminance.
    inline Vector3 UnitLuminanceColor(const Vector3& color)
    {
        const double luminance = Luminance(color);

        if(!(luminance > 0.0) || !std::isfinite(luminance))
        {
            return {0.0f, 0.0f, 0.0f};
        }

        return {float(color.x / luminance), float(color.y / luminance), float(color.z / luminance)};
    }

    // S: GPU lighting value per physical unit (cd/m^2, lux or cd) at reference exposure referenceEV100.
    inline double RenderScale(double referenceEV100)
    {
        return MeteredMidGrey * 100.0 / MeterCalibrationK * std::exp2(-referenceEV100);
    }

    // The environment luminance scale under which an HDRI texel value is unchanged on the GPU (cd/m^2 per unit).
    inline double NeutralEnvironmentLuminanceScale(double referenceEV100)
    {
        return 1.0 / RenderScale(referenceEV100);
    }

    // The neutral environment calibration as a float whose GPU multiplier float(scale * S) is exactly 1, so texels
    // upload unchanged (the nearest float to 1 / S can round the product just below 1).
    inline float NeutralEnvironmentLuminanceScaleFloat(double referenceEV100)
    {
        const double renderScale = RenderScale(referenceEV100);
        float scale = float(NeutralEnvironmentLuminanceScale(referenceEV100));

        for(int step = 0; step < 8 && float(double(scale) * renderScale) != 1.0f; ++step)
        {
            scale = std::nextafter(scale, float(double(scale) * renderScale) < 1.0f ? INFINITY : 0.0f);
        }

        return scale;
    }

    // Integral of the Spot emission profile over the sphere (sr): 2 pi * integral of f(mu) d mu, with f = 1 inside
    // the inner cone and ((mu - cosOuter) / width)^2 in the transition, width = max(cosInner - cosOuter, 1e-4) as in
    // SceneLights.hlsli. The cosines are the float values the renderer uploads, so the normalization matches the
    // profile it evaluates.
    inline double SpotEmissionSolidAngle(float innerConeAngle, float outerConeAngle)
    {
        const double cosOuter = double(std::cos(outerConeAngle));
        const double cosInner = double(std::cos(innerConeAngle));
        const double width = (std::max)(cosInner - cosOuter, 1.0e-4);
        const double full = (std::max)(1.0 - cosOuter - width, 0.0);
        return 2.0 * Pi * (full + width / 3.0);
    }

    // Luminous intensity (cd) on the emission axis of a Point (isotropic) or Spot Light of the given flux (lm).
    inline double PointIntensity(double luminousFlux)
    {
        return luminousFlux / (4.0 * Pi);
    }

    inline double SpotAxisIntensity(double luminousFlux, float innerConeAngle, float outerConeAngle)
    {
        return luminousFlux / SpotEmissionSolidAngle(innerConeAngle, outerConeAngle);
    }
}
