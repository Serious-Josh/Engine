#pragma once

#include "Math/Vector3.h"
#include "Scene/PhysicalSkySettings.h"

#include <array>

// Physical sky S0: the float64 CPU twin of the atmosphere in SkyAtmosphereCommon.hlsli (Hillaire 2020): the same media (exponential
// Rayleigh and Mie densities, the ozone tent) over the same spherical planet, integrated in double precision. S2's
// BuildFrameLighting attenuates the sun with Transmittance; the CPU tests check it against the design's reference model
// (Documentation/Rendering/validation/PhysicalSkyReferenceModel.py).
//
// Geometry: the planet's centre is below the observer; the observer stands altitudeMeters above the planet's top (the ground
// sphere of radius planetRadiusKm), and a direction is given by the cosine of its angle to the local zenith (world +Y at the
// observer). Distances are kilometres inside the model, coefficients per kilometre.
namespace SkyAtmosphereModel
{
    using Rgb = std::array<double, 3>;

    // The lowest camera altitude the sky is evaluated at (§4.2): a camera at or below the planet's top sees it from 0.5 m.
    inline constexpr double MinimumCameraAltitudeMeters = 0.5;
    // Quadratically spaced midpoint steps; 512 keep the optical depth within about 3e-6 of the converged integral on Earth.
    inline constexpr int DefaultSteps = 512;

    // max(camera y - sea level, 0.5 m).
    double CameraAltitudeMeters(const PhysicalSkySettings& settings, double cameraY);

    // Extinction (Rayleigh, Mie and ozone; per kilometre, per channel) at an altitude above the ground (kilometres).
    Rgb Extinction(const PhysicalSkySettings& settings, double altitudeKm);

    // Optical depth (per channel) along the ray from altitudeMeters toward cosZenith, through the whole atmosphere above the
    // observer up to its top. The ground is ignored here; Transmittance applies it. Zero when the ray never crosses the atmosphere.
    Rgb OpticalDepth(const PhysicalSkySettings& settings, double altitudeMeters, double cosZenith, int steps = DefaultSteps);

    // exp(-OpticalDepth), or zero when the planet blocks the direction (the sun below the observer's geometric horizon).
    Rgb Transmittance(const PhysicalSkySettings& settings, double altitudeMeters, double cosZenith, int steps = DefaultSteps);

    // The same for a world direction (Y up; any non-zero length).
    Rgb Transmittance(const PhysicalSkySettings& settings, double altitudeMeters, const Vector3& direction, int steps = DefaultSteps);

    // Whether a ray from altitudeMeters toward cosZenith meets the planet.
    bool HitsGround(const PhysicalSkySettings& settings, double altitudeMeters, double cosZenith);

    // S5B: the transmittance averaged over a disk of angularRadius (radians) centred on `direction`: the sun's or the moon's light.
    // Near the observer's geometric horizon (the disk's centre within 4 radii of it) the disk is cut into 8 horizontal strips, each
    // weighted by its chord and by the part of it above the horizon, and transmitted at that part's middle, so the light fades
    // continuously as the disk sets instead of stopping when its centre does. Elsewhere, and with no radius, the centre's
    // Transmittance.
    Rgb DiskTransmittance(const PhysicalSkySettings& settings, double altitudeMeters, const Vector3& direction,
        double angularRadius, int steps = DefaultSteps);
}
