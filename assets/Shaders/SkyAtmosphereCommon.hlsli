#ifndef ENGINE_SKY_ATMOSPHERE_COMMON
#define ENGINE_SKY_ATMOSPHERE_COMMON

// Physical sky S1 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.2): the one atmosphere integrator, after Hillaire,
// "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020). Air (Rayleigh), aerosols (Cornette-Shanks
// Mie) and an ozone tent over a spherical planet; single scattering of the sun through a transmittance LUT, and multiple scattering
// from a 32x32 LUT of its isotropic infinite-order term (Psi_ms = L2 / (1 - f_ms)) with a Lambertian ground bounce. Shared by the
// environment bake now and by the sky-view LUT, the aerial-perspective volume and the reference-sky mode later. SkyAtmosphereModel
// (C++, float64) integrates the same media for the sun's transmittance on the CPU.
//
// Units: kilometres and per-kilometre coefficients. Radiance is per unit of top-of-atmosphere sun illuminance (sr^-1); the bake
// multiplies by the sun's irradiance in render units.
//
// Geometry: positions are relative to the planet's centre in a frame whose +Y is the observer's local zenith (the bake's camera
// stands on the planet's +Y axis, so its frame is the world's). Near the ground r^2 - R^2 is a small difference of two numbers near
// 4e7 km^2, which float cannot form; every ray here therefore carries rho2 = r^2 - R^2 formed directly from the altitude, and
// distances and altitudes along it are derived from rho2 (SkyRay), never from |p|^2 - R^2.

#include "Common.hlsli"

struct SkyAtmosphereParameters
{
    float bottomRadius;
    float topRadius;
    // 1 / scale height (km^-1) of the exponential air and aerosol densities.
    float rayleighDensityScale;
    float mieDensityScale;

    float3 rayleighScattering;
    float3 rayleighExtinction;
    float3 mieScattering;
    float3 mieExtinction;
    float miePhaseG;

    float3 ozoneAbsorption;
    // The ozone tent: full density at ozoneCenter, none beyond ozoneHalfWidth from it.
    float ozoneCenter;
    float ozoneHalfWidth;

    float3 groundAlbedo;
    float multipleScatteringFactor;
};

// The scattering and extinction of the medium at one altitude.
struct SkyMediumSample
{
    float3 rayleighScattering;
    float3 mieScattering;
    float3 scattering;
    float3 extinction;
};

SkyMediumSample SampleSkyMedium(SkyAtmosphereParameters atmosphere, float altitude)
{
    altitude = max(altitude, 0.0f);
    const float rayleighDensity = exp(-altitude * atmosphere.rayleighDensityScale);
    const float mieDensity = exp(-altitude * atmosphere.mieDensityScale);
    const float ozoneDensity = saturate(1.0f - abs(altitude - atmosphere.ozoneCenter) / atmosphere.ozoneHalfWidth);

    SkyMediumSample medium;
    medium.rayleighScattering = rayleighDensity * atmosphere.rayleighScattering;
    medium.mieScattering = mieDensity * atmosphere.mieScattering;
    medium.scattering = medium.rayleighScattering + medium.mieScattering;
    medium.extinction = rayleighDensity * atmosphere.rayleighExtinction + mieDensity * atmosphere.mieExtinction +
        ozoneDensity * atmosphere.ozoneAbsorption;
    return medium;
}

float SkyRayleighPhase(float cosTheta)
{
    return 3.0f / (16.0f * PI) * (1.0f + cosTheta * cosTheta);
}

// Cornette-Shanks: Henyey-Greenstein with a Rayleigh-like (1 + cos^2) factor, normalized over the sphere.
float SkyCornetteShanksPhase(float g, float cosTheta)
{
    const float gSquared = g * g;
    const float normalization = 3.0f / (8.0f * PI) * (1.0f - gSquared) / (2.0f + gSquared);
    const float denominator = max(1.0f + gSquared - 2.0f * g * cosTheta, 1.0e-6f);
    return normalization * (1.0f + cosTheta * cosTheta) / (denominator * sqrt(denominator));
}

static const float SKY_UNIFORM_PHASE = 1.0f / (4.0f * PI);

// A ray from a point at `radius` from the planet's centre, on the frame's +Y axis, toward a unit `direction`.
struct SkyRay
{
    float radius;
    // radius^2 - bottomRadius^2, formed from the altitude.
    float rho2;
    float3 direction;
};

SkyRay MakeSkyRay(SkyAtmosphereParameters atmosphere, float altitude, float3 direction)
{
    SkyRay ray;
    ray.radius = atmosphere.bottomRadius + altitude;
    ray.rho2 = altitude * (2.0f * atmosphere.bottomRadius + altitude);
    ray.direction = direction;
    return ray;
}

// |p(t)|^2 - R^2 at a distance along the ray.
float SkyRadiusSquaredAboveGround(SkyRay ray, float distance)
{
    return ray.rho2 + 2.0f * distance * ray.radius * ray.direction.y + distance * distance;
}

float SkyAltitudeAt(SkyAtmosphereParameters atmosphere, SkyRay ray, float distance)
{
    const float aboveGround = max(SkyRadiusSquaredAboveGround(ray, distance), 0.0f);
    return aboveGround / (sqrt(atmosphere.bottomRadius * atmosphere.bottomRadius + aboveGround) + atmosphere.bottomRadius);
}

float SkyRadiusAt(SkyAtmosphereParameters atmosphere, SkyRay ray, float distance)
{
    return atmosphere.bottomRadius + SkyAltitudeAt(atmosphere, ray, distance);
}

// The roots of t^2 + 2 b t + c = 0 without cancellation (q = -(b + sign(b) sqrt(D)); roots q and c / q), ascending. False when
// there is no real root.
bool SkySolveQuadratic(float b, float c, out float nearRoot, out float farRoot)
{
    nearRoot = 0.0f;
    farRoot = 0.0f;
    const float discriminant = b * b - c;

    if(discriminant < 0.0f)
    {
        return false;
    }

    const float root = sqrt(discriminant);
    const float q = b >= 0.0f ? -(b + root) : -b + root;
    const float other = q != 0.0f ? c / q : 0.0f;
    nearRoot = min(q, other);
    farRoot = max(q, other);
    return true;
}

// The nearest positive distance to the ground, or -1 when the ray misses it.
float SkyDistanceToGround(SkyRay ray)
{
    float nearRoot;
    float farRoot;

    if(ray.direction.y >= 0.0f || !SkySolveQuadratic(ray.radius * ray.direction.y, ray.rho2, nearRoot, farRoot))
    {
        return -1.0f;
    }

    return nearRoot >= 0.0f ? nearRoot : (farRoot >= 0.0f ? farRoot : -1.0f);
}

// The part of the ray inside the atmosphere's top sphere; false when it never enters.
bool SkyAtmosphereInterval(SkyAtmosphereParameters atmosphere, SkyRay ray, out float start, out float end)
{
    const float topAboveGround = (atmosphere.topRadius - atmosphere.bottomRadius) * (atmosphere.topRadius + atmosphere.bottomRadius);
    float nearRoot;
    float farRoot;
    start = 0.0f;
    end = 0.0f;

    if(!SkySolveQuadratic(ray.radius * ray.direction.y, ray.rho2 - topAboveGround, nearRoot, farRoot) || farRoot <= 0.0f)
    {
        return false;
    }

    start = max(nearRoot, 0.0f);
    end = farRoot;
    return end > start;
}

// Whether a point at `radius` (rho2 = radius^2 - R^2) sees the sun at cosine cosSunZenith to its zenith above its geometric horizon.
bool SkySunAboveHorizon(float radius, float rho2, float cosSunZenith)
{
    return cosSunZenith >= -sqrt(max(rho2, 0.0f)) / radius;
}

// Transmittance LUT (Bruneton's parameterization, as Hillaire): x_mu is the distance to the top of the atmosphere between its least
// and greatest values for the radius, x_r = rho / H with H the distance to the top along the ground's horizon. Rays that hit the
// ground are not in the LUT; callers test the horizon first. Both coordinates use the unit-range remap to texel centres.
float SkyUnitToTexelCenter(float unit, float size)
{
    return 0.5f / size + unit * (1.0f - 1.0f / size);
}

float SkyTexelCenterToUnit(float coordinate, float size)
{
    return (coordinate - 0.5f / size) / (1.0f - 1.0f / size);
}

float SkyHorizonDistance(SkyAtmosphereParameters atmosphere)
{
    return sqrt((atmosphere.topRadius - atmosphere.bottomRadius) * (atmosphere.topRadius + atmosphere.bottomRadius));
}

float2 SkyTransmittanceLutUv(SkyAtmosphereParameters atmosphere, float radius, float rho2, float cosZenith, float2 lutSize)
{
    const float horizon = SkyHorizonDistance(atmosphere);
    const float rho = sqrt(max(rho2, 0.0f));
    const float topAboveGround = horizon * horizon;
    float nearRoot;
    float farRoot;
    SkySolveQuadratic(radius * cosZenith, rho2 - topAboveGround, nearRoot, farRoot);
    const float distance = max(farRoot, 0.0f);
    const float minimumDistance = atmosphere.topRadius - radius;
    const float maximumDistance = rho + horizon;
    const float xMu = (distance - minimumDistance) / max(maximumDistance - minimumDistance, 1.0e-6f);
    const float xR = rho / horizon;
    return float2(SkyUnitToTexelCenter(saturate(xMu), lutSize.x), SkyUnitToTexelCenter(saturate(xR), lutSize.y));
}

// The inverse: the radius, rho2 and view zenith cosine a transmittance LUT texel stands for.
void SkyTransmittanceLutParameters(SkyAtmosphereParameters atmosphere, float2 uv, float2 lutSize, out float radius, out float rho2,
    out float cosZenith)
{
    const float xMu = saturate(SkyTexelCenterToUnit(uv.x, lutSize.x));
    const float xR = saturate(SkyTexelCenterToUnit(uv.y, lutSize.y));
    const float horizon = SkyHorizonDistance(atmosphere);
    const float rho = horizon * xR;
    rho2 = rho * rho;
    radius = sqrt(rho2 + atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float minimumDistance = atmosphere.topRadius - radius;
    const float maximumDistance = rho + horizon;
    const float distance = minimumDistance + xMu * (maximumDistance - minimumDistance);
    cosZenith = distance <= 0.0f ? 1.0f : (horizon * horizon - rho2 - distance * distance) / (2.0f * radius * distance);
    cosZenith = clamp(cosZenith, -1.0f, 1.0f);
}

float3 SampleSkyTransmittance(SkyAtmosphereParameters atmosphere, Texture2D<float4> transmittanceLut, SamplerState lutSampler,
    float2 lutSize, float radius, float rho2, float cosZenith)
{
    const float2 uv = SkyTransmittanceLutUv(atmosphere, radius, rho2, cosZenith, lutSize);
    return transmittanceLut.SampleLevel(lutSampler, uv, 0.0f).rgb;
}

// The sun's transmittance to a point, zero when the planet hides the sun from it.
float3 SkySunTransmittance(SkyAtmosphereParameters atmosphere, Texture2D<float4> transmittanceLut, SamplerState lutSampler,
    float2 lutSize, float radius, float rho2, float cosSunZenith)
{
    if(!SkySunAboveHorizon(radius, rho2, cosSunZenith))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, lutSize, radius, rho2, cosSunZenith);
}

// Multiple-scattering LUT: x the sun's zenith cosine from -1 to 1, y the altitude from the ground to the top; texel centres at
// (i + 0.5) / size (bilinear, clamped).
float2 SkyMultipleScatteringLutUv(SkyAtmosphereParameters atmosphere, float altitude, float cosSunZenith)
{
    return float2(saturate(cosSunZenith * 0.5f + 0.5f), saturate(altitude / (atmosphere.topRadius - atmosphere.bottomRadius)));
}

float3 SampleSkyMultipleScattering(SkyAtmosphereParameters atmosphere, Texture2D<float4> multipleScatteringLut,
    SamplerState lutSampler, float altitude, float cosSunZenith)
{
    const float2 uv = SkyMultipleScatteringLutUv(atmosphere, altitude, cosSunZenith);
    return multipleScatteringLut.SampleLevel(lutSampler, uv, 0.0f).rgb;
}

// The fraction of a segment's in-scattered light that leaves it toward its start: integral over [0, dt] of exp(-sigma_t t), per unit
// length of source (Hillaire's energy-conserving integration); dt where the medium is clear.
float3 SkySegmentIntegral(float3 extinction, float3 segmentTransmittance, float segmentLength)
{
    return float3(
        extinction.x > 1.0e-8f ? (1.0f - segmentTransmittance.x) / extinction.x : segmentLength,
        extinction.y > 1.0e-8f ? (1.0f - segmentTransmittance.y) / extinction.y : segmentLength,
        extinction.z > 1.0e-8f ? (1.0f - segmentTransmittance.z) / extinction.z : segmentLength);
}

// The distance from the ray's start to the end of segment `index` of `count`, quadratically spaced (dense near the start).
float SkyQuadraticStep(float start, float end, uint index, uint count)
{
    const float fraction = float(index) / float(count);
    return start + (end - start) * fraction * fraction;
}

#endif
