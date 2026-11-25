// Physical sky S1 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.1, §4.5.1): SkyAtmosphere's passes.
//   CSTransmittance       [1] transmittance LUT (atmosphere parameter changes only)
//   CSMultipleScattering  [2] multiple-scattering LUT (atmosphere parameter changes only)
//   CSEnvironment         [3] environment bake into the inactive set's equirect mip 0, sun disk excluded
//   CSImportance          [4] the importance CDFs of that equirect, with EnvironmentMap::BuildImportanceDistribution's rules
// The mips and the cubes of the inactive set ([5]) are the existing EnvironmentMip, EquirectangularToCubemap, DiffuseIrradiance and
// GGXMultiscatterIrradiance passes. Every pass reads one constant buffer, SkyAtmosphereConstants (SkyAtmosphere.cpp mirrors it).

#include "SkyAtmosphereCommon.hlsli"

cbuffer SkyAtmosphereConstants : register(b0)
{
    float bottomRadius;
    float topRadius;
    float rayleighDensityScale;
    float mieDensityScale;

    float3 rayleighScattering;
    float miePhaseG;

    float3 rayleighExtinction;
    float ozoneCenter;

    float3 mieScattering;
    float ozoneHalfWidth;

    float3 mieExtinction;
    float multipleScatteringFactor;

    float3 ozoneAbsorption;
    // Kilometres above the planet's top, at least 0.5 m.
    float cameraAltitude;

    float3 groundAlbedo;
    // 2^EV or the multiplier (PhysicalSkyLuminanceFactor).
    float skyLuminanceFactor;

    float3 directionToSun;
    uint showGroundBelowHorizon;

    // The sun above the atmosphere, in render units (unit-luminance tint x lux x the render scale).
    float3 sunIrradiance;
    uint sampleBelowHorizon;

    uint transmittanceWidth;
    uint transmittanceHeight;
    uint multipleScatteringSize;
    uint environmentWidth;

    uint environmentHeight;
    uint transmittanceIndex;
    uint transmittanceOutputIndex;
    uint multipleScatteringIndex;

    uint multipleScatteringOutputIndex;
    uint environmentIndex;
    uint environmentOutputIndex;
    uint conditionalCdfIndex;

    uint marginalCdfIndex;
    uint rowWeightsIndex;
    uint skyViewOutputIndex;
    uint skyViewWidth;

    uint skyViewHeight;
    uint aerialOutputIndex;
    // The aerial-perspective volume's far distance (kilometres) and slice count.
    float aerialMaxDistance;
    uint aerialSlices;

    // The frame's camera (aerial perspective only): world from clip space, and its position in world metres.
    row_major float4x4 aerialInverseViewProjection;

    float3 aerialCameraPosition;
    float aerialPadding;

    // S4: HDRP's artistic overrides (non-physical): neutral at saturation 1 and white tints.
    float3 horizonTint;
    float colorSaturation;

    float3 zenithTint;
    float horizonZenithShift;

    // S5 (§14.1): the moon, a second atmosphere light scattered once, and the night sky in render units.
    float3 directionToMoon;
    uint moonEnabled;

    float3 moonIrradiance;
    float airglowRadiance;

    float starMeanRadiance;
    float planetRotation;
    // S5 (§14.3): the set's cloud environment map (rgb in-scatter, a background transmittance), or 0xFFFFFFFF.
    uint cloudEnvironmentIndex;
    uint cloudEnvironmentWidth;

    uint cloudEnvironmentHeight;
    // Sun behind clouds (2026-11-25): rays within this cosine of the sun or the moon march past the clouds' early-out, so a disk
    // behind a cloud gets the cloud's true transmittance (SkyMarchClouds). Above 1: no ray does.
    float cloudDiskCosine;
    uint2 nightPadding;
}

// S4 (§4.12.3): HDRP's Artistic Overrides, applied to the sky's radiance toward a direction for the environment and the sky-view map
// alike, so lighting matches what is seen. The tint blends from the horizon tint to the zenith tint with height (the shift moves the
// blend: positive toward the horizon), then the saturation mixes toward luminance. Neutral settings return the radiance unchanged.
float3 SkyArtisticOverrides(float3 radiance, float3 direction)
{
    const bool tinted = any(horizonTint != 1.0f) || any(zenithTint != 1.0f);

    if(!tinted && colorSaturation == 1.0f)
    {
        return radiance;
    }

    if(tinted)
    {
        const float height = pow(saturate(direction.y), exp2(-2.0f * horizonZenithShift));
        radiance *= lerp(horizonTint, zenithTint, height);
    }

    const float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
    return lerp(float3(luminance, luminance, luminance), radiance, colorSaturation);
}

// Linear, clamped: the LUT coordinates are already remapped to texel centres.
SamplerState lutSampler : register(s0);

static const uint SKY_TRANSMITTANCE_STEPS = 128u;
static const uint SKY_MULTIPLE_SCATTERING_STEPS = 32u;
static const uint SKY_MULTIPLE_SCATTERING_DIRECTIONS = 64u;
static const uint SKY_MULTIPLE_SCATTERING_DIRECTIONS_PER_AXIS = 8u;
static const uint SKY_ENVIRONMENT_STEPS = 40u;

SkyAtmosphereParameters LoadSkyAtmosphere()
{
    SkyAtmosphereParameters atmosphere;
    atmosphere.bottomRadius = bottomRadius;
    atmosphere.topRadius = topRadius;
    atmosphere.rayleighDensityScale = rayleighDensityScale;
    atmosphere.mieDensityScale = mieDensityScale;
    atmosphere.rayleighScattering = rayleighScattering;
    atmosphere.rayleighExtinction = rayleighExtinction;
    atmosphere.mieScattering = mieScattering;
    atmosphere.mieExtinction = mieExtinction;
    atmosphere.miePhaseG = miePhaseG;
    atmosphere.ozoneAbsorption = ozoneAbsorption;
    atmosphere.ozoneCenter = ozoneCenter;
    atmosphere.ozoneHalfWidth = ozoneHalfWidth;
    atmosphere.groundAlbedo = groundAlbedo;
    atmosphere.multipleScatteringFactor = multipleScatteringFactor;
    return atmosphere;
}

float2 TransmittanceLutSize()
{
    return float2(float(transmittanceWidth), float(transmittanceHeight));
}

float3 SunTransmittanceAt(SkyAtmosphereParameters atmosphere, float radius, float rho2, float cosSunZenith)
{
    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[transmittanceIndex];
    return SkySunTransmittance(atmosphere, transmittanceLut, lutSampler, TransmittanceLutSize(), radius, rho2, cosSunZenith);
}

// The sun's zenith cosine at a distance along the ray (the zenith there is the point's direction from the planet's centre).
float CosSunZenithAt(SkyRay ray, float distance, float radiusAt, float3 sun)
{
    return clamp((ray.radius * sun.y + distance * dot(ray.direction, sun)) / radiusAt, -1.0f, 1.0f);
}

// [1] One texel: the optical depth from its (radius, zenith cosine) to the top of the atmosphere.
[numthreads(8, 8, 1)]
void CSTransmittance(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= transmittanceWidth || dispatchThread.y >= transmittanceHeight)
    {
        return;
    }

    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const float2 lutSize = TransmittanceLutSize();
    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / lutSize;
    float radius;
    float rho2;
    float cosZenith;
    SkyTransmittanceLutParameters(atmosphere, uv, lutSize, radius, rho2, cosZenith);

    SkyRay ray;
    ray.radius = radius;
    ray.rho2 = rho2;
    ray.direction = float3(sqrt(saturate(1.0f - cosZenith * cosZenith)), cosZenith, 0.0f);

    float3 opticalDepth = float3(0.0f, 0.0f, 0.0f);
    float start;
    float end;

    if(SkyAtmosphereInterval(atmosphere, ray, start, end))
    {
        for(uint step = 0u; step < SKY_TRANSMITTANCE_STEPS; ++step)
        {
            const float segmentStart = SkyQuadraticStep(start, end, step, SKY_TRANSMITTANCE_STEPS);
            const float segmentEnd = SkyQuadraticStep(start, end, step + 1u, SKY_TRANSMITTANCE_STEPS);
            const float altitude = SkyAltitudeAt(atmosphere, ray, 0.5f * (segmentStart + segmentEnd));
            opticalDepth += SampleSkyMedium(atmosphere, altitude).extinction * (segmentEnd - segmentStart);
        }
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[transmittanceOutputIndex];
    output[dispatchThread.xy] = float4(exp(-opticalDepth), 1.0f);
}

groupshared float3 g_secondOrder[SKY_MULTIPLE_SCATTERING_DIRECTIONS];
groupshared float3 g_transfer[SKY_MULTIPLE_SCATTERING_DIRECTIONS];

// [2] One group per texel (altitude, sun zenith cosine), one thread per direction of an 8x8 stratified sphere. Each direction
// integrates the second-order light L' (isotropic phase, the sun through the transmittance LUT, plus the sunlit Lambertian ground
// where the ray ends on it) and the transfer f (the scattered fraction); the texel stores Psi_ms = mean(L') / (1 - mean(f)), the
// sum of every order >= 2 per unit sun illuminance (Hillaire 2020, eqs. 5-10).
[numthreads(64, 1, 1)]
void CSMultipleScattering(uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const float lutSize = float(multipleScatteringSize);
    const float cosSunZenith = (float(group.x) + 0.5f) / lutSize * 2.0f - 1.0f;
    const float altitude = (float(group.y) + 0.5f) / lutSize * (atmosphere.topRadius - atmosphere.bottomRadius);
    const float3 sun = float3(sqrt(saturate(1.0f - cosSunZenith * cosSunZenith)), cosSunZenith, 0.0f);

    const uint column = threadIndex % SKY_MULTIPLE_SCATTERING_DIRECTIONS_PER_AXIS;
    const uint row = threadIndex / SKY_MULTIPLE_SCATTERING_DIRECTIONS_PER_AXIS;
    const float azimuth = 2.0f * PI * (float(column) + 0.5f) / float(SKY_MULTIPLE_SCATTERING_DIRECTIONS_PER_AXIS);
    const float cosPolar = 1.0f - 2.0f * (float(row) + 0.5f) / float(SKY_MULTIPLE_SCATTERING_DIRECTIONS_PER_AXIS);
    const float sinPolar = sqrt(saturate(1.0f - cosPolar * cosPolar));
    const float3 direction = float3(cos(azimuth) * sinPolar, cosPolar, sin(azimuth) * sinPolar);
    const SkyRay ray = MakeSkyRay(atmosphere, altitude, direction);

    float3 secondOrder = float3(0.0f, 0.0f, 0.0f);
    float3 transfer = float3(0.0f, 0.0f, 0.0f);
    float start;
    float end;

    if(SkyAtmosphereInterval(atmosphere, ray, start, end))
    {
        const float groundDistance = SkyDistanceToGround(ray);
        const bool hitsGround = groundDistance >= 0.0f;

        if(hitsGround)
        {
            end = min(end, groundDistance);
        }

        float3 throughput = float3(1.0f, 1.0f, 1.0f);

        for(uint step = 0u; step < SKY_MULTIPLE_SCATTERING_STEPS; ++step)
        {
            const float segmentStart = SkyQuadraticStep(start, end, step, SKY_MULTIPLE_SCATTERING_STEPS);
            const float segmentEnd = SkyQuadraticStep(start, end, step + 1u, SKY_MULTIPLE_SCATTERING_STEPS);
            const float segmentLength = segmentEnd - segmentStart;
            const float distance = 0.5f * (segmentStart + segmentEnd);
            const float altitudeAt = SkyAltitudeAt(atmosphere, ray, distance);
            const float radiusAt = atmosphere.bottomRadius + altitudeAt;
            const float rho2At = max(SkyRadiusSquaredAboveGround(ray, distance), 0.0f);
            const SkyMediumSample medium = SampleSkyMedium(atmosphere, altitudeAt);
            const float3 segmentTransmittance = exp(-medium.extinction * segmentLength);
            const float3 segmentIntegral = SkySegmentIntegral(medium.extinction, segmentTransmittance, segmentLength);
            const float3 sunTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At,
                CosSunZenithAt(ray, distance, radiusAt, sun));

            secondOrder += throughput * medium.scattering * sunTransmittance * SKY_UNIFORM_PHASE * segmentIntegral;
            transfer += throughput * medium.scattering * segmentIntegral;
            throughput *= segmentTransmittance;
        }

        // The ground's Lambertian bounce of the sun where the ray ends on it.
        if(hitsGround)
        {
            const float cosSunGround = CosSunZenithAt(ray, groundDistance, atmosphere.bottomRadius, sun);
            const float3 sunAtGround = SunTransmittanceAt(atmosphere, atmosphere.bottomRadius, 0.0f, cosSunGround);
            secondOrder += throughput * sunAtGround * saturate(cosSunGround) * atmosphere.groundAlbedo / PI;
        }
    }

    g_secondOrder[threadIndex] = secondOrder;
    g_transfer[threadIndex] = transfer;
    GroupMemoryBarrierWithGroupSync();

    for(uint stride = SKY_MULTIPLE_SCATTERING_DIRECTIONS / 2u; stride > 0u; stride >>= 1u)
    {
        if(threadIndex < stride)
        {
            g_secondOrder[threadIndex] += g_secondOrder[threadIndex + stride];
            g_transfer[threadIndex] += g_transfer[threadIndex + stride];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if(threadIndex == 0u)
    {
        const float3 meanSecondOrder = g_secondOrder[0] / float(SKY_MULTIPLE_SCATTERING_DIRECTIONS);
        const float3 meanTransfer = g_transfer[0] / float(SKY_MULTIPLE_SCATTERING_DIRECTIONS);
        // The geometric series of every higher order; f_ms < 1 for any atmosphere that absorbs or lets light escape.
        const float3 infiniteOrders = meanSecondOrder / max(1.0f - meanTransfer, 1.0e-4f);
        RWTexture2D<float4> output = ResourceDescriptorHeap[multipleScatteringOutputIndex];
        output[group.xy] = float4(infiniteOrders, 1.0f);
    }
}

// Sky radiance toward `direction` from the camera, per unit top-of-atmosphere sun illuminance (sr^-1), without the sun disk: single
// scattering of the sun (Rayleigh and Cornette-Shanks phases, the sun through the transmittance LUT and the planet's shadow),
// multiple scattering from its LUT (S5: not for the second light, the moon), and the sunlit planet ground where the ray meets it,
// attenuated by the air in front of it.
float3 SkyRadianceFromCameraFor(SkyAtmosphereParameters atmosphere, float3 direction, float3 sun, bool multipleScatteringOn)
{
    const SkyRay ray = MakeSkyRay(atmosphere, cameraAltitude, direction);
    float start;
    float end;

    if(!SkyAtmosphereInterval(atmosphere, ray, start, end))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float groundDistance = SkyDistanceToGround(ray);
    const bool hitsGround = groundDistance >= 0.0f;

    if(hitsGround)
    {
        end = min(end, groundDistance);
    }

    Texture2D<float4> multipleScatteringLut = ResourceDescriptorHeap[multipleScatteringIndex];
    const float cosView = dot(direction, sun);
    const float rayleighPhase = SkyRayleighPhase(cosView);
    const float miePhase = SkyCornetteShanksPhase(atmosphere.miePhaseG, cosView);
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    for(uint step = 0u; step < SKY_ENVIRONMENT_STEPS; ++step)
    {
        const float segmentStart = SkyQuadraticStep(start, end, step, SKY_ENVIRONMENT_STEPS);
        const float segmentEnd = SkyQuadraticStep(start, end, step + 1u, SKY_ENVIRONMENT_STEPS);
        const float segmentLength = segmentEnd - segmentStart;
        const float distance = 0.5f * (segmentStart + segmentEnd);
        const float altitudeAt = SkyAltitudeAt(atmosphere, ray, distance);
        const float radiusAt = atmosphere.bottomRadius + altitudeAt;
        const float rho2At = max(SkyRadiusSquaredAboveGround(ray, distance), 0.0f);
        const float cosSunAt = CosSunZenithAt(ray, distance, radiusAt, sun);
        const SkyMediumSample medium = SampleSkyMedium(atmosphere, altitudeAt);
        const float3 segmentTransmittance = exp(-medium.extinction * segmentLength);
        const float3 sunTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At, cosSunAt);
        const float3 multipleScattering = multipleScatteringOn ? SampleSkyMultipleScattering(atmosphere, multipleScatteringLut,
            lutSampler, altitudeAt, cosSunAt) * atmosphere.multipleScatteringFactor : float3(0.0f, 0.0f, 0.0f);
        const float3 source = (medium.rayleighScattering * rayleighPhase + medium.mieScattering * miePhase) * sunTransmittance +
            medium.scattering * multipleScattering;

        radiance += throughput * source * SkySegmentIntegral(medium.extinction, segmentTransmittance, segmentLength);
        throughput *= segmentTransmittance;
    }

    // The planet ground: Lambertian, lit by the transmitted sun only (no sky irradiance in S1; the reference model does the same).
    if(hitsGround)
    {
        const float cosSunGround = CosSunZenithAt(ray, groundDistance, atmosphere.bottomRadius, sun);
        const float3 sunAtGround = SunTransmittanceAt(atmosphere, atmosphere.bottomRadius, 0.0f, cosSunGround);
        radiance += throughput * sunAtGround * saturate(cosSunGround) * atmosphere.groundAlbedo / PI;
    }

    return radiance;
}

float3 SkyRadianceFromCamera(SkyAtmosphereParameters atmosphere, float3 direction, float3 sun)
{
    return SkyRadianceFromCameraFor(atmosphere, direction, sun, true);
}

// S5 (§14.1): the night sky's emission toward `direction` (render units): airglow, brighter toward the horizon by the van Rhijn
// factor of a thin layer at 90 km, and (includeStars) the star field's mean radiance (the equirect's; camera rays draw the stars
// per pixel, SkyFrame.hlsli), seen through the atmosphere's transmittance. Nothing below the planet's horizon.
float3 SkyNightEmission(SkyAtmosphereParameters atmosphere, float3 direction, bool includeStars)
{
    if(airglowRadiance <= 0.0f && (!includeStars || starMeanRadiance <= 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const SkyRay ray = MakeSkyRay(atmosphere, cameraAltitude, direction);

    if(SkyDistanceToGround(ray) >= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float ratio = atmosphere.bottomRadius / (atmosphere.bottomRadius + 90.0f);
    const float sinSquared = saturate(1.0f - direction.y * direction.y);
    const float vanRhijn = rsqrt(max(1.0f - ratio * ratio * sinSquared, 1.0e-4f));
    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[transmittanceIndex];
    const float3 transmittance = SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, TransmittanceLutSize(), ray.radius,
        ray.rho2, direction.y);
    // The faint night sky is near-neutral; a slightly green airglow (the 558 nm line) on a unit-luminance tint.
    const float3 airglowTint = float3(0.85f, 1.05f, 0.9f) / dot(float3(0.85f, 1.05f, 0.9f), float3(0.2126f, 0.7152f, 0.0722f));
    const float3 emission = airglowRadiance * vanRhijn * airglowTint + (includeStars ? starMeanRadiance : 0.0f);
    return emission * transmittance;
}

// S5: the sky radiance toward a direction for both bakes: the sun (all orders), the moon (single scattering), the night sky, times
// the sky luminance factor, with the artistic overrides.
float3 SkyBakedRadiance(SkyAtmosphereParameters atmosphere, float3 direction, bool includeStars)
{
    float3 radiance = SkyRadianceFromCamera(atmosphere, direction, directionToSun) * sunIrradiance;

    if(moonEnabled != 0u)
    {
        radiance += SkyRadianceFromCameraFor(atmosphere, direction, directionToMoon, false) * moonIrradiance;
    }

    radiance += SkyNightEmission(atmosphere, direction, includeStars);
    return SkyArtisticOverrides(radiance * skyLuminanceFactor, direction);
}

// S5 (§14.3): the set's cloud environment map at a direction (DirectionToEquirectangularUV; bilinear, columns wrap): in-scattered
// light reaching the camera (rgb) and the transmittance of what lies behind (a). No clouds: (0, 1).
float4 SkyCloudEnvironmentAt(float3 direction)
{
    if(cloudEnvironmentIndex == 0xFFFFFFFFu)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    Texture2D<float4> clouds = ResourceDescriptorHeap[cloudEnvironmentIndex];
    const float2 texel = DirectionToEquirectangularUV(direction) * float2(float(cloudEnvironmentWidth), float(cloudEnvironmentHeight)) -
        0.5f;
    const float2 base = floor(texel);
    const float2 weight = texel - base;
    const int width = int(cloudEnvironmentWidth);
    const int column0 = (int(base.x) % width + width) % width;
    const int column1 = (column0 + 1) % width;
    const int row0 = clamp(int(base.y), 0, int(cloudEnvironmentHeight) - 1);
    const int row1 = clamp(int(base.y) + 1, 0, int(cloudEnvironmentHeight) - 1);
    const float4 top = lerp(clouds.Load(int3(column0, row0, 0)), clouds.Load(int3(column1, row0, 0)), weight.x);
    const float4 bottom = lerp(clouds.Load(int3(column0, row1, 0)), clouds.Load(int3(column1, row1, 0)), weight.x);
    return lerp(top, bottom, weight.y);
}

// [3] One equirect texel (DirectionToEquirectangularUV's convention): the sky toward its centre in render units, times the sky
// luminance factor. The lower hemisphere is black when the scene hides the planet ground.
[numthreads(8, 8, 1)]
void CSEnvironment(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= environmentWidth || dispatchThread.y >= environmentHeight)
    {
        return;
    }

    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / float2(float(environmentWidth), float(environmentHeight));
    const float3 direction = normalize(EquirectangularUVToDirection(uv));
    float3 radiance = float3(0.0f, 0.0f, 0.0f);

    if(direction.y >= 0.0f || showGroundBelowHorizon != 0u)
    {
        const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
        radiance = SkyBakedRadiance(atmosphere, direction, true);
    }

    // S5 (§14.3): the clouds over the sky, so the CDF, the cubes and every non-delta miss see them (the sky-view map stays clear:
    // camera rays get the full-resolution camera clouds instead).
    const float4 clouds = SkyCloudEnvironmentAt(direction);
    radiance = radiance * clouds.a + clouds.rgb;

    // Never publish a texel the CDF or a sampler would turn into NaN.
    radiance = all(isfinite(radiance)) ? max(radiance, 0.0f) : float3(0.0f, 0.0f, 0.0f);
    RWTexture2D<float4> output = ResourceDescriptorHeap[environmentOutputIndex];
    output[dispatchThread.xy] = float4(radiance, 1.0f);
}

// [3b] One sky-view texel (Common.hlsli's DirectionToSkyViewUV): the background for camera rays in every mode, at the horizon's
// resolution the equirect lacks. Same radiance as [3].
[numthreads(8, 8, 1)]
void CSSkyView(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= skyViewWidth || dispatchThread.y >= skyViewHeight)
    {
        return;
    }

    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / float2(float(skyViewWidth), float(skyViewHeight));
    const float3 direction = normalize(SkyViewUVToDirection(uv));
    float3 radiance = float3(0.0f, 0.0f, 0.0f);

    if(direction.y >= 0.0f || showGroundBelowHorizon != 0u)
    {
        const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
        // S5: no stars (drawn per pixel on camera rays) and no clouds (the camera clouds are composited at full resolution).
        radiance = SkyBakedRadiance(atmosphere, direction, false);
    }

    radiance = all(isfinite(radiance)) ? max(radiance, 0.0f) : float3(0.0f, 0.0f, 0.0f);
    RWTexture2D<float4> output = ResourceDescriptorHeap[skyViewOutputIndex];
    output[dispatchThread.xy] = float4(radiance, 1.0f);
}

// Physical sky S3 (§4.7): the aerial-perspective volume, per frame. One thread per screen tile column marches its view ray from
// the camera through aerialSlices slices at distances aerialMaxDistance * ((k + 1) / slices)^2 and stores, per slice, the light
// the atmosphere scatters toward the camera over that distance (render units, rgb) and the mean transmittance (alpha; a grey
// transmittance, as Unreal's volume). The volume is a 2D atlas: slice k occupies columns [k * tiles, (k + 1) * tiles).
static const uint SKY_AERIAL_TILES = 32u;
static const uint SKY_AERIAL_SUBSTEPS = 2u;

[numthreads(8, 8, 1)]
void CSAerialPerspective(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= SKY_AERIAL_TILES || dispatchThread.y >= SKY_AERIAL_TILES)
    {
        return;
    }

    // The tile centre's view direction (standard depth: the far plane at z = 1).
    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / float(SKY_AERIAL_TILES);
    const float4 farPoint = mul(aerialInverseViewProjection, float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f));
    const float3 direction = normalize(farPoint.xyz / farPoint.w - aerialCameraPosition);
    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const SkyRay ray = MakeSkyRay(atmosphere, cameraAltitude, direction);
    Texture2D<float4> multipleScatteringLut = ResourceDescriptorHeap[multipleScatteringIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[aerialOutputIndex];
    const float cosView = dot(direction, directionToSun);
    const float rayleighPhase = SkyRayleighPhase(cosView);
    const float miePhase = SkyCornetteShanksPhase(atmosphere.miePhaseG, cosView);
    // S5B: the moon's single scattering (as the sky-view map and the equirect carry it: no multiple scattering, Unreal's rule for
    // the second atmosphere light), with its own phase and transmittance.
    const bool moon = moonEnabled != 0u;
    const float cosMoonView = dot(direction, directionToMoon);
    const float moonRayleighPhase = SkyRayleighPhase(cosMoonView);
    const float moonMiePhase = SkyCornetteShanksPhase(atmosphere.miePhaseG, cosMoonView);
    float3 scattered = float3(0.0f, 0.0f, 0.0f);
    float3 moonScattered = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float previous = 0.0f;

    for(uint slice = 0u; slice < aerialSlices; ++slice)
    {
        const float fraction = float(slice + 1u) / float(aerialSlices);
        const float sliceEnd = aerialMaxDistance * fraction * fraction;
        const float step = (sliceEnd - previous) / float(SKY_AERIAL_SUBSTEPS);

        for(uint substep = 0u; substep < SKY_AERIAL_SUBSTEPS; ++substep)
        {
            const float distance = previous + (float(substep) + 0.5f) * step;
            const float altitudeAt = max(SkyAltitudeAt(atmosphere, ray, distance), 0.0f);
            const float radiusAt = atmosphere.bottomRadius + altitudeAt;
            const float rho2At = max(SkyRadiusSquaredAboveGround(ray, distance), 0.0f);
            const float cosSunAt = CosSunZenithAt(ray, distance, radiusAt, directionToSun);
            const SkyMediumSample medium = SampleSkyMedium(atmosphere, altitudeAt);
            const float3 segmentTransmittance = exp(-medium.extinction * step);
            const float3 sunTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At, cosSunAt);
            const float3 multipleScattering = SampleSkyMultipleScattering(atmosphere, multipleScatteringLut, lutSampler, altitudeAt,
                cosSunAt) * atmosphere.multipleScatteringFactor;
            const float3 source = (medium.rayleighScattering * rayleighPhase + medium.mieScattering * miePhase) * sunTransmittance +
                medium.scattering * multipleScattering;
            const float3 segment = SkySegmentIntegral(medium.extinction, segmentTransmittance, step);
            scattered += throughput * source * segment;

            if(moon)
            {
                const float cosMoonAt = CosSunZenithAt(ray, distance, radiusAt, directionToMoon);
                const float3 moonTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At, cosMoonAt);
                moonScattered += throughput * (medium.rayleighScattering * moonRayleighPhase + medium.mieScattering * moonMiePhase) *
                    moonTransmittance * segment;
            }

            throughput *= segmentTransmittance;
        }

        previous = sliceEnd;
        float3 light = (scattered * sunIrradiance + moonScattered * moonIrradiance) * skyLuminanceFactor;
        light = all(isfinite(light)) ? max(light, 0.0f) : float3(0.0f, 0.0f, 0.0f);
        const float transmittance = saturate(dot(throughput, float3(1.0f, 1.0f, 1.0f) / 3.0f));
        output[uint2(slice * SKY_AERIAL_TILES + dispatchThread.x, dispatchThread.y)] = float4(light, transmittance);
    }
}

// [4] The row-conditional and marginal CDFs of the baked equirect, following EnvironmentMap::BuildImportanceDistribution's rules:
// weight = Rec.709 luminance x sin(theta) (theta at the row centre, computed in float as the CPU does); each row's running sum in
// double, stored as float and divided by the row total as float; a zero row is uniform; the marginal accumulated in double and
// normalized by the total; a zero total is uniform; the last entry of every row and of the marginal is exactly 1. Rows below the
// horizon keep their radiance but get no weight unless the sky samples below the horizon (§4.5.2). The CPU total weight is the
// constant 1 for the sky, so a black sky samples uniformly and contributes nothing.
// The sums are parallel: one group per row (each thread sums a run of consecutive columns, then the group scans the runs), then
// one group scans the row totals. Their rounding order differs from the CPU's sequential sum by double rounding only, so the
// stored floats match the CPU's except at rare rounding ties (the GPU check compares them).
static const uint SKY_IMPORTANCE_ROW_THREADS = 256u;
static const uint SKY_IMPORTANCE_MAXIMUM_COLUMNS_PER_THREAD = 8u;
static const uint SKY_IMPORTANCE_MAXIMUM_ROWS = 1024u;
// EnvironmentMap.cpp's float pi.
static const float SKY_IMPORTANCE_PI = 3.14159265358979323846f;

groupshared double g_importanceScan[SKY_IMPORTANCE_MAXIMUM_ROWS];

// Inclusive prefix sum of g_importanceScan[0, count) in place; count is the group's thread count.
void SkyInclusiveScan(uint threadIndex, uint count)
{
    for(uint offset = 1u; offset < count; offset <<= 1u)
    {
        GroupMemoryBarrierWithGroupSync();
        const double addend = threadIndex >= offset ? g_importanceScan[threadIndex - offset] : 0.0;
        GroupMemoryBarrierWithGroupSync();
        g_importanceScan[threadIndex] += addend;
    }

    GroupMemoryBarrierWithGroupSync();
}

// One group per row: the row's conditional CDF and its total weight (rowWeights, a double as two words).
[numthreads(SKY_IMPORTANCE_ROW_THREADS, 1, 1)]
void CSImportanceRows(uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    Texture2D<float4> environment = ResourceDescriptorHeap[environmentIndex];
    RWStructuredBuffer<float> conditional = ResourceDescriptorHeap[conditionalCdfIndex];
    RWStructuredBuffer<uint2> rowWeights = ResourceDescriptorHeap[rowWeightsIndex];
    const uint width = environmentWidth;
    const uint height = min(environmentHeight, SKY_IMPORTANCE_MAXIMUM_ROWS);
    const uint row = group.x;

    if(row >= height)
    {
        return;
    }

    const float theta = SKY_IMPORTANCE_PI * (float(row) + 0.5f) / float(height);
    const float solidAngleWeight = sin(theta);
    const bool weighted = sampleBelowHorizon != 0u || row < height / 2u;
    const uint columnsPerThread = (width + SKY_IMPORTANCE_ROW_THREADS - 1u) / SKY_IMPORTANCE_ROW_THREADS;
    const uint firstColumn = threadIndex * columnsPerThread;
    const uint rowStart = row * width;

    // This thread's run of consecutive columns, summed in the CPU's order.
    double running[SKY_IMPORTANCE_MAXIMUM_COLUMNS_PER_THREAD];
    double runTotal = 0.0;

    [unroll]
    for(uint step = 0u; step < SKY_IMPORTANCE_MAXIMUM_COLUMNS_PER_THREAD; ++step)
    {
        const uint column = firstColumn + step;

        if(step < columnsPerThread && column < width)
        {
            const float3 radiance = environment.Load(int3(int(column), int(row), 0)).rgb;
            const float luminance = max(0.2126f * radiance.r + 0.7152f * radiance.g + 0.0722f * radiance.b, 0.0f);
            runTotal += double(weighted ? luminance * solidAngleWeight : 0.0f);
        }

        running[step] = runTotal;
    }

    g_importanceScan[threadIndex] = runTotal;
    SkyInclusiveScan(threadIndex, SKY_IMPORTANCE_ROW_THREADS);
    const double rowWeight = g_importanceScan[SKY_IMPORTANCE_ROW_THREADS - 1u];
    const double before = threadIndex > 0u ? g_importanceScan[threadIndex - 1u] : 0.0;
    const float rowTotal = float(rowWeight);

    [unroll]
    for(uint entry = 0u; entry < SKY_IMPORTANCE_MAXIMUM_COLUMNS_PER_THREAD; ++entry)
    {
        const uint column = firstColumn + entry;

        if(entry < columnsPerThread && column < width)
        {
            const float value = rowWeight > 0.0 ? float(before + running[entry]) / rowTotal : (float(column) + 1.0f) / float(width);
            conditional[rowStart + column] = column == width - 1u ? 1.0f : value;
        }
    }

    if(threadIndex == 0u)
    {
        uint low;
        uint high;
        asuint(rowWeight, low, high);
        rowWeights[row] = uint2(low, high);
    }
}

// One group: the marginal CDF from the row totals.
[numthreads(SKY_IMPORTANCE_MAXIMUM_ROWS, 1, 1)]
void CSImportanceMarginal(uint threadIndex : SV_GroupIndex)
{
    RWStructuredBuffer<float> marginal = ResourceDescriptorHeap[marginalCdfIndex];
    RWStructuredBuffer<uint2> rowWeights = ResourceDescriptorHeap[rowWeightsIndex];
    const uint height = min(environmentHeight, SKY_IMPORTANCE_MAXIMUM_ROWS);
    double weight = 0.0;

    if(threadIndex < height)
    {
        const uint2 words = rowWeights[threadIndex];
        weight = asdouble(words.x, words.y);
    }

    g_importanceScan[threadIndex] = weight;
    SkyInclusiveScan(threadIndex, SKY_IMPORTANCE_MAXIMUM_ROWS);
    const double totalWeight = g_importanceScan[height - 1u];

    if(threadIndex >= height)
    {
        return;
    }

    const float value = totalWeight > 0.0 ? float(g_importanceScan[threadIndex] / totalWeight) :
        (float(threadIndex) + 1.0f) / float(height);
    marginal[threadIndex] = threadIndex == height - 1u ? 1.0f : value;
}

// =====================================================================================================================================
// Physical sky S5 (Physical_Sky_Design_Proposal.md §14.3): clouds. One ray marcher for every pass, on spherical shells around the
// planet's centre (the same frame as the atmosphere: the camera on +Y), so the camera may be below, inside or above a layer.
//   CSCloudShapeNoise, CSCloudDetailNoise  the 128^3 Perlin-Worley shape and 32^3 Worley detail volumes, once at start-up
//   CSCloudAmbient                         the clear sky's mean radiance above and below (the clouds' ambient light), per rebuild
//   CSCloudEnvironment                     the set's 256x128 cloud map (rebuild stage 0), composited by CSEnvironment
//   CSCloudShadow                          the sun's transmittance through the clouds on a plane around the camera, per frame
//   CSCloudTrace, CSCloudReproject         the camera clouds at quarter resolution: one pixel of each 4x4 block traced per frame,
//                                          the other fifteen reprojected (SkyCloudComposite.hlsl draws them over the frame)
// Units: kilometres; radiance in render units. A cloud map texel holds the in-scattered light that reaches the camera through the air
// in front of the cloud (rgb) and the transmittance of whatever lies behind (a): background * a + rgb.
// =====================================================================================================================================

struct SkyCloudLayer
{
    float bottomRadius;
    float topRadius;
    float coverage;
    // Volumetric: extinction per km at full density. Layer: optical depth at full density.
    float density;

    float shapeFrequency;
    float detailFrequency;
    float erosion;
    float shapeFactor;

    float3 albedo;
    float multipleScattering;

    float phaseForward;
    float phaseBackward;
    float phaseBlend;
    float ambientStrength;

    // km, added to world x and z.
    float2 windOffset;
    // 0: a 2D Layer at bottomRadius; 1: Volumetric.
    uint kind;
    uint castShadows;
};

static const uint SKY_CLOUD_MAXIMUM_LAYERS = 4u;
static const uint SKY_CLOUD_KIND_LAYER = 0u;

cbuffer SkyCloudConstants : register(b1)
{
    SkyCloudLayer cloudLayers[SKY_CLOUD_MAXIMUM_LAYERS];

    uint cloudLayerCount;
    uint shapeNoiseIndex;
    uint detailNoiseIndex;
    uint cloudAmbientIndex;

    // The camera's world x and z in km (cloud noise coordinates follow the world).
    float2 cloudCameraWorldKm;
    uint cloudFrame;
    uint cloudHistoryValid;

    // Sparse fix (2026-11-25): camera-relative matrices (the camera at the origin). With world matrices the far-plane inverse lost
    // the view direction to float precision a few km from the origin: a still camera 7 km out reprojected its history 3.2-3.5
    // cloud pixels a frame (0.04 near the origin), so the 15 history pixels of each block drifted, and holes opened in clouds.
    row_major float4x4 cloudInverseViewProjection;
    row_major float4x4 cloudPreviousViewProjection;

    // World metres: the camera's move since the previous frame (this position minus the previous one).
    float3 cloudCameraMove;
    // 2^(previous EV_ref - this frame's): the history's render units to this frame's.
    float cloudHistoryScale;

    uint cloudTraceWidth;
    uint cloudTraceHeight;
    uint cloudWidth;
    uint cloudHeight;

    uint cloudTraceOutputIndex;
    uint cloudTraceDepthOutputIndex;
    uint cloudHistoryIndex;
    uint cloudHistoryDepthIndex;

    uint cloudOutputIndex;
    uint cloudDepthOutputIndex;
    uint cloudTraceIndex;
    uint cloudTraceDepthIndex;

    // The shadow plane: its centre's world x and z, its extent (metres) and its world y.
    float2 cloudShadowOrigin;
    float cloudShadowExtent;
    float cloudShadowReferenceY;

    uint cloudShadowOutputIndex;
    uint cloudShadowSize;
    uint cloudNoiseOutputIndex;
    uint cloudNoiseSize;

    uint cloudEnvironmentOutputIndex;
    uint cloudEnvironmentOutputWidth;
    uint cloudEnvironmentOutputHeight;
    // World y of sea level, metres.
    float cloudSeaLevel;
}

// Linear, wrapping (the noise volumes tile).
SamplerState cloudNoiseSampler : register(s1);

// ---- Hashes and tileable noise (the volumes' generation) -----------------------------------------------------------------------

uint SkyCloudHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float3 SkyCloudHash3(uint3 cell, uint seed)
{
    const uint h = SkyCloudHash(cell.x ^ SkyCloudHash(cell.y ^ SkyCloudHash(cell.z ^ seed)));
    const uint h1 = SkyCloudHash(h);
    const uint h2 = SkyCloudHash(h1);
    return float3(float(h >> 8), float(h1 >> 8), float(h2 >> 8)) * (1.0f / 16777216.0f);
}

uint3 SkyCloudWrap(int3 cell, uint period)
{
    const int p = int(period);
    return uint3((cell % p + p) % p);
}

// Inverted F1 Worley noise, tileable with `period` cells over p in [0, period).
float SkyCloudWorley(float3 p, uint period, uint seed)
{
    const float3 cell = floor(p);
    const float3 fraction = p - cell;
    float best = 1.0e9f;

    [unroll]
    for(int z = -1; z <= 1; ++z)
    {
        [unroll]
        for(int y = -1; y <= 1; ++y)
        {
            [unroll]
            for(int x = -1; x <= 1; ++x)
            {
                const int3 offset = int3(x, y, z);
                const float3 feature = float3(offset) + SkyCloudHash3(SkyCloudWrap(int3(cell) + offset, period), seed);
                const float3 delta = feature - fraction;
                best = min(best, dot(delta, delta));
            }
        }
    }

    return saturate(1.0f - sqrt(best));
}

// Gradient (Perlin) noise in about [-1, 1], tileable with `period` lattice cells.
float SkyCloudPerlin(float3 p, uint period, uint seed)
{
    const float3 cell = floor(p);
    const float3 f = p - cell;
    const float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float corners[8];

    [unroll]
    for(uint index = 0u; index < 8u; ++index)
    {
        const int3 corner = int3(index & 1u, (index >> 1u) & 1u, (index >> 2u) & 1u);
        const float3 gradient = normalize(SkyCloudHash3(SkyCloudWrap(int3(cell) + corner, period), seed) * 2.0f - 1.0f + 1.0e-4f);
        corners[index] = dot(gradient, f - float3(corner));
    }

    const float x0 = lerp(corners[0], corners[1], u.x);
    const float x1 = lerp(corners[2], corners[3], u.x);
    const float x2 = lerp(corners[4], corners[5], u.x);
    const float x3 = lerp(corners[6], corners[7], u.x);
    return lerp(lerp(x0, x1, u.y), lerp(x2, x3, u.y), u.z) * 1.4f;
}

float SkyCloudRemap(float value, float oldMinimum, float oldMaximum, float newMinimum, float newMaximum)
{
    return newMinimum + (value - oldMinimum) / max(oldMaximum - oldMinimum, 1.0e-5f) * (newMaximum - newMinimum);
}

// The shape volume: r a Perlin-Worley base, gba inverted Worley at 4, 8 and 16 cells per tile.
[numthreads(4, 4, 4)]
void CSCloudShapeNoise(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(any(dispatchThread >= cloudNoiseSize))
    {
        return;
    }

    const float3 p = (float3(dispatchThread) + 0.5f) / float(cloudNoiseSize);
    float perlin = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;

    [unroll]
    for(uint octave = 0u; octave < 4u; ++octave)
    {
        const uint period = 4u << octave;
        perlin += amplitude * SkyCloudPerlin(p * float(period), period, 17u + octave);
        total += amplitude;
        amplitude *= 0.5f;
    }

    perlin = saturate(perlin / total * 0.5f + 0.5f);
    const float worley4 = SkyCloudWorley(p * 4.0f, 4u, 101u);
    const float worley8 = SkyCloudWorley(p * 8.0f, 8u, 202u);
    const float worley16 = SkyCloudWorley(p * 16.0f, 16u, 303u);
    const float worleyFbm = worley4 * 0.625f + worley8 * 0.25f + worley16 * 0.125f;
    const float perlinWorley = saturate(SkyCloudRemap(perlin, 0.0f, 1.0f, worleyFbm, 1.0f));
    // Spread to about uniform on [0, 1], so a layer's coverage is the share of the sky it covers: the Perlin-Worley field spans
    // about 0.61 to 0.86 between its 5th and 95th percentiles (a CPU replica of this kernel, S5 report §12).
    RWTexture3D<unorm float4> output = ResourceDescriptorHeap[cloudNoiseOutputIndex];
    output[dispatchThread] = float4(saturate((perlinWorley - 0.6f) / 0.28f), worley4, worley8, worley16);
}

// The detail volume: inverted Worley at 2, 4 and 8 cells per tile.
[numthreads(4, 4, 4)]
void CSCloudDetailNoise(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(any(dispatchThread >= cloudNoiseSize))
    {
        return;
    }

    const float3 p = (float3(dispatchThread) + 0.5f) / float(cloudNoiseSize);
    RWTexture3D<unorm float4> output = ResourceDescriptorHeap[cloudNoiseOutputIndex];
    output[dispatchThread] = float4(SkyCloudWorley(p * 2.0f, 2u, 404u), SkyCloudWorley(p * 4.0f, 4u, 505u),
        SkyCloudWorley(p * 8.0f, 8u, 606u), 1.0f);
}

// ---- Density ------------------------------------------------------------------------------------------------------------------

// Rounded bottoms and tops: a flat sheet at shape factor 0, tall rounded towers at 1.
float SkyCloudHeightGradient(float heightFraction, float shapeFactor)
{
    const float bottom = saturate(SkyCloudRemap(heightFraction, 0.0f, lerp(0.04f, 0.2f, shapeFactor), 0.0f, 1.0f));
    const float top = saturate(SkyCloudRemap(heightFraction, lerp(0.92f, 0.45f, shapeFactor), 1.0f, 1.0f, 0.0f));
    return bottom * top;
}

// Normalized density (0-1) at world position (km: x, altitude, z) and height fraction in the layer. Coverage is modulated by a
// low-frequency field (the procedural weather; the coverage map slot replaces it in a later step), the Perlin-Worley shape is
// thresholded by it and (detail) eroded by the Worley detail.
float SkyCloudDensity(SkyCloudLayer layer, float3 worldKm, float heightFraction, bool detail)
{
    Texture3D<float4> shapeNoise = ResourceDescriptorHeap[shapeNoiseIndex];
    const float2 xz = worldKm.xz + layer.windOffset;
    const float weather = shapeNoise.SampleLevel(cloudNoiseSampler, float3(xz * (layer.shapeFrequency * 0.21f), 0.37f), 0.0f).g;
    const float coverage = saturate(layer.coverage * lerp(0.6f, 1.4f, weather));

    if(coverage <= 0.0f)
    {
        return 0.0f;
    }

    // The Perlin-Worley base (about uniform on [0, 1]) shaped by the height gradient; the layer covers where it exceeds
    // 1 - coverage.
    const float4 shape = shapeNoise.SampleLevel(cloudNoiseSampler, float3(xz.x, worldKm.y, xz.y) * layer.shapeFrequency, 0.0f);
    const float base = shape.r * SkyCloudHeightGradient(heightFraction, layer.shapeFactor);
    float density = saturate(SkyCloudRemap(base, 1.0f - coverage, 1.0f, 0.0f, 1.0f)) * coverage;

    if(detail && density > 0.0f && layer.erosion > 0.0f)
    {
        Texture3D<float4> detailNoise = ResourceDescriptorHeap[detailNoiseIndex];
        const float4 fine = detailNoise.SampleLevel(cloudNoiseSampler, float3(xz.x, worldKm.y, xz.y) * layer.detailFrequency, 0.0f);
        const float fbm = fine.r * 0.625f + fine.g * 0.25f + fine.b * 0.125f;
        const float modifier = lerp(fbm, 1.0f - fbm, saturate(heightFraction * 4.0f));
        density = saturate(SkyCloudRemap(density, modifier * layer.erosion * 0.6f, 1.0f, 0.0f, 1.0f));
    }

    return density;
}

// A planet-centred position (km, the camera at (0, R + h, 0)) in cloud world coordinates (x, altitude, z), flat around the camera.
float3 SkyCloudWorld(float3 planetPosition, float radius)
{
    return float3(planetPosition.x + cloudCameraWorldKm.x, radius - bottomRadius, planetPosition.z + cloudCameraWorldKm.y);
}

bool SkyCloudSphere(float3 origin, float3 direction, float originRadiusSquaredMinus, out float near, out float far)
{
    // |o + t d|^2 = r^2 with c = |o|^2 - r^2 given exactly by the caller.
    const float b = dot(origin, direction);
    const float discriminant = b * b - originRadiusSquaredMinus;
    near = 0.0f;
    far = -1.0f;

    if(discriminant < 0.0f)
    {
        return false;
    }

    const float root = sqrt(discriminant);
    near = -b - root;
    far = -b + root;
    return true;
}

// |o|^2 - r^2 for the camera origin, exactly, from altitudes (km).
float SkyCloudOriginTerm(float originAltitude, float radius)
{
    const float shellAltitude = radius - bottomRadius;
    return (originAltitude - shellAltitude) * (2.0f * bottomRadius + originAltitude + shellAltitude);
}

float SkyCloudHenyeyGreenstein(float g, float cosTheta)
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * PI * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-4f), 1.5f));
}

// Optical depth toward a light from a point in a volumetric layer: six quadratically spaced steps to the layer's top (at most three
// thicknesses), shape only.
float SkyCloudLightOpticalDepth(SkyCloudLayer layer, float3 position, float3 toLight)
{
    const float thickness = layer.topRadius - layer.bottomRadius;
    const float radiusSquared = dot(position, position);
    float near;
    float far;
    float total = 3.0f * thickness;

    if(SkyCloudSphere(position, toLight, radiusSquared - layer.topRadius * layer.topRadius, near, far) && far > 0.0f)
    {
        total = min(far, total);
    }

    float opticalDepth = 0.0f;
    float previous = 0.0f;

    [unroll]
    for(uint step = 0u; step < 6u; ++step)
    {
        const float fraction = float(step + 1u) / 6.0f;
        const float distance = total * fraction * fraction;
        const float segment = distance - previous;
        const float middle = 0.5f * (distance + previous);
        previous = distance;
        const float3 sample = position + toLight * middle;
        const float radius = length(sample);
        const float heightFraction = (radius - layer.bottomRadius) / thickness;

        if(heightFraction > 1.0f)
        {
            break;
        }

        if(heightFraction >= 0.0f)
        {
            opticalDepth += SkyCloudDensity(layer, SkyCloudWorld(sample, radius), heightFraction, false) * layer.density * segment;
        }
    }

    return opticalDepth;
}

// One light's in-scattered radiance per unit scattering at a cloud point: the light above the atmosphere, through the air (the
// transmittance LUT at the point) and through the cloud (opticalDepth), with Wrenninge's three multiple-scattering octaves of a
// dual-lobe Henyey-Greenstein phase.
float3 SkyCloudLight(SkyAtmosphereParameters atmosphere, SkyCloudLayer layer, float3 position, float radius, float3 view,
    float3 toLight, float3 irradiance, float opticalDepth)
{
    const float altitude = max(radius - atmosphere.bottomRadius, 0.0f);
    const float cosZenith = dot(position / radius, toLight);
    const float rho2 = altitude * (2.0f * atmosphere.bottomRadius + altitude);
    const float3 air = SunTransmittanceAt(atmosphere, radius, rho2, cosZenith);

    if(all(air <= 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float cosTheta = dot(view, toLight);
    float result = 0.0f;
    float a = 1.0f;
    float b = 1.0f;
    float c = 1.0f;

    [unroll]
    for(uint octave = 0u; octave < 3u; ++octave)
    {
        const float phase = lerp(SkyCloudHenyeyGreenstein(layer.phaseBackward * c, cosTheta),
            SkyCloudHenyeyGreenstein(layer.phaseForward * c, cosTheta), layer.phaseBlend);
        result += a * exp(-b * opticalDepth) * phase;
        a *= layer.multipleScattering;
        b *= max(layer.multipleScattering, 0.05f);
        c *= layer.multipleScattering;
    }

    return irradiance * air * result;
}

// Radiance per unit scattering at a cloud point from the sun, the moon and the clear sky's ambient light.
float3 SkyCloudInScattering(SkyAtmosphereParameters atmosphere, SkyCloudLayer layer, float3 position, float radius, float3 view,
    float heightFraction, float verticalOpticalDepth)
{
    const bool sheet = layer.kind == SKY_CLOUD_KIND_LAYER;
    const float3 up = position / radius;
    const float sunDepth = sheet ? verticalOpticalDepth / max(abs(dot(up, directionToSun)), 0.05f) :
        SkyCloudLightOpticalDepth(layer, position, directionToSun);
    float3 light = SkyCloudLight(atmosphere, layer, position, radius, view, directionToSun, sunIrradiance, sunDepth);

    if(moonEnabled != 0u)
    {
        const float moonDepth = sheet ? verticalOpticalDepth / max(abs(dot(up, directionToMoon)), 0.05f) :
            SkyCloudLightOpticalDepth(layer, position, directionToMoon);
        light += SkyCloudLight(atmosphere, layer, position, radius, view, directionToMoon, moonIrradiance, moonDepth);
    }

    // Ambient: the clear sky's mean radiance above (texel 0) and below (texel 1), isotropic phase; the tops see more of the sky.
    if(cloudAmbientIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> ambient = ResourceDescriptorHeap[cloudAmbientIndex];
        const float3 above = ambient.Load(int3(0, 0, 0)).rgb;
        const float3 below = ambient.Load(int3(1, 0, 0)).rgb;
        light += 0.5f * (above * lerp(0.55f, 1.0f, heightFraction) + below * lerp(1.0f, 0.6f, heightFraction)) * layer.ambientStrength;
    }

    return light;
}

// ---- The march ----------------------------------------------------------------------------------------------------------------

struct SkyCloudSpan
{
    float start;
    float end;
    uint layer;
};

struct SkyCloudResult
{
    float3 scattered;
    float transmittance;
    // The mean distance of the light received (km), or the first span's start; maxDistance without clouds.
    float depth;
};

// The spans of a ray from the camera (planet-centred origin at originAltitude above the ground) through every layer, nearest first:
// a volumetric shell clipped to [0, maxDistance] (two spans when the ray passes through its inner ball), a Layer's crossings as
// zero-length spans.
uint SkyCloudSpans(float3 origin, float originAltitude, float3 direction, float maxDistance, out SkyCloudSpan spans[8])
{
    uint count = 0u;

    for(uint index = 0u; index < min(cloudLayerCount, SKY_CLOUD_MAXIMUM_LAYERS); ++index)
    {
        const SkyCloudLayer layer = cloudLayers[index];
        float innerNear;
        float innerFar;
        const bool inner = SkyCloudSphere(origin, direction, SkyCloudOriginTerm(originAltitude, layer.bottomRadius), innerNear,
            innerFar);

        if(layer.kind == SKY_CLOUD_KIND_LAYER)
        {
            if(inner)
            {
                if(innerNear > 0.0f && innerNear < maxDistance && count < 8u)
                {
                    spans[count].start = innerNear;
                    spans[count].end = innerNear;
                    spans[count].layer = index;
                    ++count;
                }

                if(innerFar > 0.0f && innerFar < maxDistance && count < 8u)
                {
                    spans[count].start = innerFar;
                    spans[count].end = innerFar;
                    spans[count].layer = index;
                    ++count;
                }
            }

            continue;
        }

        float outerNear;
        float outerFar;

        if(!SkyCloudSphere(origin, direction, SkyCloudOriginTerm(originAltitude, layer.topRadius), outerNear, outerFar) ||
            outerFar <= 0.0f)
        {
            continue;
        }

        const float start = max(outerNear, 0.0f);
        const float end = min(outerFar, maxDistance);

        if(inner && innerFar > 0.0f && innerNear < end)
        {
            const float firstEnd = min(innerNear, end);
            const float secondStart = max(innerFar, start);

            if(firstEnd > start && count < 8u)
            {
                spans[count].start = start;
                spans[count].end = firstEnd;
                spans[count].layer = index;
                ++count;
            }

            if(end > secondStart && count < 8u)
            {
                spans[count].start = secondStart;
                spans[count].end = end;
                spans[count].layer = index;
                ++count;
            }
        }
        else if(end > start && count < 8u)
        {
            spans[count].start = start;
            spans[count].end = end;
            spans[count].layer = index;
            ++count;
        }
    }

    // Nearest first.
    for(uint i = 1u; i < count; ++i)
    {
        const SkyCloudSpan key = spans[i];
        int j = int(i) - 1;

        while(j >= 0 && spans[j].start > key.start)
        {
            spans[j + 1] = spans[j];
            --j;
        }

        spans[j + 1] = key;
    }

    return count;
}

SkyCloudResult SkyMarchClouds(SkyAtmosphereParameters atmosphere, float originAltitude, float3 direction, float jitter, uint maxSteps)
{
    SkyCloudResult result;
    result.scattered = float3(0.0f, 0.0f, 0.0f);
    result.transmittance = 1.0f;
    const float3 origin = float3(0.0f, atmosphere.bottomRadius + originAltitude, 0.0f);
    // The planet's ground ends every ray that meets it.
    float groundNear;
    float groundFar;
    float maxDistance = 1.0e5f;

    if(SkyCloudSphere(origin, direction, SkyCloudOriginTerm(originAltitude, atmosphere.bottomRadius), groundNear, groundFar) &&
        groundNear > 0.0f)
    {
        maxDistance = groundNear;
    }

    result.depth = maxDistance;
    SkyCloudSpan spans[8];
    const uint count = SkyCloudSpans(origin, originAltitude, direction, maxDistance, spans);
    float depthSum = 0.0f;
    float depthWeight = 0.0f;
    uint stepsLeft = maxSteps;
    // Sun behind clouds (2026-11-25): a ray stops lighting the clouds at T = 0.004 (what is left of their light is invisible), and
    // records T = 0 there. A ray toward the sun's or the moon's disk carries on with the density alone until T = 1e-6, so the disk
    // behind a cloud is dimmed by the cloud's whole optical depth: a display-clamped disk (60,000) outshines a sunlit cloud (about 1)
    // down to T near 1e-5, so stopping at 0.004 would hide it behind clouds that real light still crosses.
    const bool towardDisk = dot(direction, directionToSun) >= cloudDiskCosine ||
        (moonEnabled != 0u && dot(direction, directionToMoon) >= cloudDiskCosine);
    const float stopTransmittance = towardDisk ? 1.0e-6f : 0.004f;

    for(uint index = 0u; index < count && result.transmittance > stopTransmittance && stepsLeft > 0u; ++index)
    {
        const SkyCloudSpan span = spans[index];
        const SkyCloudLayer layer = cloudLayers[span.layer];

        if(layer.kind == SKY_CLOUD_KIND_LAYER)
        {
            const float3 position = origin + direction * span.start;
            const float radius = length(position);
            const float cosView = abs(dot(direction, position / radius));
            const float density = SkyCloudDensity(layer, SkyCloudWorld(position, radius), 0.5f, true);

            if(density <= 0.0f)
            {
                continue;
            }

            const float verticalDepth = density * layer.density;
            const float opacity = 1.0f - exp(-verticalDepth / max(cosView, 0.05f));

            if(result.transmittance <= 0.004f)
            {
                result.transmittance *= 1.0f - opacity;
                continue;
            }

            const float3 light = SkyCloudInScattering(atmosphere, layer, position, radius, direction, 0.5f, 0.5f * verticalDepth);
            result.scattered += result.transmittance * opacity * light * layer.albedo;
            depthSum += span.start * result.transmittance * opacity;
            depthWeight += result.transmittance * opacity;
            result.transmittance *= 1.0f - opacity;
            continue;
        }

        const float thickness = layer.topRadius - layer.bottomRadius;
        const float spanLength = span.end - span.start;
        const uint steps = clamp(uint(ceil(spanLength / max(thickness / 24.0f, 0.03f))), 4u, min(stepsLeft, 96u));
        const float stepLength = spanLength / float(steps);
        stepsLeft -= min(steps, stepsLeft);

        for(uint step = 0u; step < steps; ++step)
        {
            const float distance = span.start + (float(step) + jitter) * stepLength;
            const float3 position = origin + direction * distance;
            const float radius = length(position);
            const float heightFraction = (radius - layer.bottomRadius) / thickness;

            if(heightFraction < 0.0f || heightFraction > 1.0f)
            {
                continue;
            }

            const float density = SkyCloudDensity(layer, SkyCloudWorld(position, radius), heightFraction, true);

            if(density <= 1.0e-4f)
            {
                continue;
            }

            const float extinction = density * layer.density;
            const float stepTransmittance = exp(-extinction * stepLength);

            if(result.transmittance > 0.004f)
            {
                const float3 light = SkyCloudInScattering(atmosphere, layer, position, radius, direction, heightFraction, 0.0f);
                // Energy-conserving: the source over the step, attenuated within it (albedo = sigma_s / sigma_t).
                const float absorbed = 1.0f - stepTransmittance;
                result.scattered += result.transmittance * light * layer.albedo * absorbed;
                depthSum += distance * result.transmittance * absorbed;
                depthWeight += result.transmittance * absorbed;
            }

            result.transmittance *= stepTransmittance;

            if(result.transmittance <= stopTransmittance)
            {
                result.transmittance = 0.0f;
                break;
            }
        }
    }

    if(depthWeight > 1.0e-5f)
    {
        result.depth = depthSum / depthWeight;
    }
    else if(count > 0u)
    {
        result.depth = spans[0].start;
    }

    return result;
}

// The air's transmittance from the camera to a distance along a ray (km), from the transmittance LUT: the ratio of the two points'
// transmittances to the top, taken upward (a descending ray is evaluated reversed).
float3 SkyCloudAirTransmittance(SkyAtmosphereParameters atmosphere, float originAltitude, float3 direction, float distance)
{
    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[transmittanceIndex];
    const SkyRay ray = MakeSkyRay(atmosphere, originAltitude, direction);
    const float rho2End = max(SkyRadiusSquaredAboveGround(ray, distance), 0.0f);
    const float radiusEnd = sqrt(rho2End + atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float cosEnd = clamp((ray.radius * direction.y + distance) / max(radiusEnd, 1.0e-3f), -1.0f, 1.0f);
    const float2 size = TransmittanceLutSize();

    if(direction.y >= 0.0f)
    {
        const float3 fromCamera = SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, size, ray.radius, ray.rho2,
            direction.y);
        const float3 fromEnd = SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, size, radiusEnd, rho2End, cosEnd);
        return saturate(fromCamera / max(fromEnd, 1.0e-6f));
    }

    const float3 fromEnd = SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, size, radiusEnd, rho2End, -cosEnd);
    const float3 fromCamera = SampleSkyTransmittance(atmosphere, transmittanceLut, lutSampler, size, ray.radius, ray.rho2,
        -direction.y);
    return saturate(fromEnd / max(fromCamera, 1.0e-6f));
}

static const uint SKY_CLOUD_AIR_STEPS = 32u;

// Sun behind clouds (2026-11-25): the light the air between the camera and a distance along a ray (km) scatters toward the camera,
// in render units: the aerial-perspective volume's integrand (the sun with multiple scattering, the moon once, times the sky
// luminance factor) to any distance, on quadratically spaced segments.
float3 SkyCloudAirInScattering(SkyAtmosphereParameters atmosphere, float originAltitude, float3 direction, float distance)
{
    const SkyRay ray = MakeSkyRay(atmosphere, originAltitude, direction);
    Texture2D<float4> multipleScatteringLut = ResourceDescriptorHeap[multipleScatteringIndex];
    const float cosView = dot(direction, directionToSun);
    const float rayleighPhase = SkyRayleighPhase(cosView);
    const float miePhase = SkyCornetteShanksPhase(atmosphere.miePhaseG, cosView);
    const bool moon = moonEnabled != 0u;
    const float cosMoonView = dot(direction, directionToMoon);
    const float moonRayleighPhase = SkyRayleighPhase(cosMoonView);
    const float moonMiePhase = SkyCornetteShanksPhase(atmosphere.miePhaseG, cosMoonView);
    float3 scattered = float3(0.0f, 0.0f, 0.0f);
    float3 moonScattered = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    for(uint step = 0u; step < SKY_CLOUD_AIR_STEPS; ++step)
    {
        const float segmentStart = SkyQuadraticStep(0.0f, distance, step, SKY_CLOUD_AIR_STEPS);
        const float segmentEnd = SkyQuadraticStep(0.0f, distance, step + 1u, SKY_CLOUD_AIR_STEPS);
        const float segmentLength = segmentEnd - segmentStart;
        const float at = 0.5f * (segmentStart + segmentEnd);
        const float altitudeAt = max(SkyAltitudeAt(atmosphere, ray, at), 0.0f);
        const float radiusAt = atmosphere.bottomRadius + altitudeAt;
        const float rho2At = max(SkyRadiusSquaredAboveGround(ray, at), 0.0f);
        const float cosSunAt = CosSunZenithAt(ray, at, radiusAt, directionToSun);
        const SkyMediumSample medium = SampleSkyMedium(atmosphere, altitudeAt);
        const float3 segmentTransmittance = exp(-medium.extinction * segmentLength);
        const float3 sunTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At, cosSunAt);
        const float3 multipleScattering = SampleSkyMultipleScattering(atmosphere, multipleScatteringLut, lutSampler, altitudeAt,
            cosSunAt) * atmosphere.multipleScatteringFactor;
        const float3 source = (medium.rayleighScattering * rayleighPhase + medium.mieScattering * miePhase) * sunTransmittance +
            medium.scattering * multipleScattering;
        const float3 segment = SkySegmentIntegral(medium.extinction, segmentTransmittance, segmentLength);
        scattered += throughput * source * segment;

        if(moon)
        {
            const float cosMoonAt = CosSunZenithAt(ray, at, radiusAt, directionToMoon);
            const float3 moonTransmittance = SunTransmittanceAt(atmosphere, radiusAt, rho2At, cosMoonAt);
            moonScattered += throughput * (medium.rayleighScattering * moonRayleighPhase + medium.mieScattering * moonMiePhase) *
                moonTransmittance * segment;
        }

        throughput *= segmentTransmittance;
    }

    const float3 light = (scattered * sunIrradiance + moonScattered * moonIrradiance) * skyLuminanceFactor;
    return all(isfinite(light)) ? max(light, 0.0f) : float3(0.0f, 0.0f, 0.0f);
}

// A marched result as a cloud map texel, composited as background * a + rgb:
//   a   = T_cloud, the clouds' own transmittance of whatever lies behind them;
//   rgb = the clouds' light through the air in front of them, plus the air's in-scatter in front of them over the share the clouds
//         hide, (1 - T_cloud) L_air(0, depth).
// The background (the sky-view map, an HDRI, far geometry) already holds the air in front of the clouds, L_air(0, depth), and its
// light beyond them; the clouds hide only the part beyond: L = T_cloud L_bg + T_air L_cloud + (1 - T_cloud) L_air(0, depth), exact
// for a cloud at one depth. Sun behind clouds (2026-11-25): before, a = 1 - (1 - T_cloud) T_air stood in for the air in front by a
// share 1 - T_air of the background itself, so behind an opaque cloud the background kept 1 - T_air (about 5-12% for a cloud a few
// km away), and with it the sun's disk (display-clamped to 60,000 against a sunlit cloud near 1): the disk was drawn over the clouds.
float4 SkyCloudTexel(SkyAtmosphereParameters atmosphere, float originAltitude, float3 direction, SkyCloudResult clouds)
{
    if(clouds.transmittance >= 0.9999f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float3 air = SkyCloudAirTransmittance(atmosphere, originAltitude, direction, clouds.depth);
    const float3 airLight = SkyCloudAirInScattering(atmosphere, originAltitude, direction, clouds.depth);
    float3 light = clouds.scattered * air + (1.0f - clouds.transmittance) * airLight;
    light = all(isfinite(light)) ? max(light, 0.0f) : float3(0.0f, 0.0f, 0.0f);
    return float4(light, saturate(clouds.transmittance));
}

// ---- Passes -------------------------------------------------------------------------------------------------------------------

groupshared float3 g_cloudAmbient[64];

// The clear sky's mean radiance over the upper (texel 0) and lower (texel 1) hemispheres, from 64 directions each (sun, moon and
// airglow, no clouds, no sky luminance factor).
[numthreads(64, 1, 1)]
void CSCloudAmbient(uint threadIndex : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const bool lower = group.x == 1u;
    // Fibonacci directions over the hemisphere, uniform in solid angle.
    const float y = (float(threadIndex) + 0.5f) / 64.0f;
    const float radius = sqrt(saturate(1.0f - y * y));
    const float angle = float(threadIndex) * 2.39996323f;
    const float3 direction = float3(cos(angle) * radius, lower ? -y : y, sin(angle) * radius);
    float3 radiance = SkyRadianceFromCamera(atmosphere, direction, directionToSun) * sunIrradiance;

    if(moonEnabled != 0u)
    {
        radiance += SkyRadianceFromCameraFor(atmosphere, direction, directionToMoon, false) * moonIrradiance;
    }

    radiance += SkyNightEmission(atmosphere, direction, true);
    g_cloudAmbient[threadIndex] = all(isfinite(radiance)) ? max(radiance, 0.0f) : float3(0.0f, 0.0f, 0.0f);
    GroupMemoryBarrierWithGroupSync();

    if(threadIndex == 0u)
    {
        float3 sum = float3(0.0f, 0.0f, 0.0f);

        for(uint index = 0u; index < 64u; ++index)
        {
            sum += g_cloudAmbient[index];
        }

        RWTexture2D<float4> output = ResourceDescriptorHeap[cloudAmbientIndex];
        output[uint2(group.x, 0u)] = float4(sum / 64.0f, 1.0f);
    }
}

// Rebuild stage 0: the set's cloud map (equirect directions, the bake's camera and wind).
[numthreads(8, 8, 1)]
void CSCloudEnvironment(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= cloudEnvironmentOutputWidth || dispatchThread.y >= cloudEnvironmentOutputHeight)
    {
        return;
    }

    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / float2(float(cloudEnvironmentOutputWidth), float(cloudEnvironmentOutputHeight));
    const float3 direction = normalize(EquirectangularUVToDirection(uv));
    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const SkyCloudResult clouds = SkyMarchClouds(atmosphere, cameraAltitude, direction, 0.5f, 64u);
    RWTexture2D<float4> output = ResourceDescriptorHeap[cloudEnvironmentOutputIndex];
    output[dispatchThread.xy] = SkyCloudTexel(atmosphere, cameraAltitude, direction, clouds);
}

// The sun's transmittance through every shadow-casting layer along the sun's line through each point of a horizontal plane at the
// reference height. 1 where the sun is lower than about 1 degree.
// Sun behind clouds (2026-11-25): the same density as the camera's view (detail erosion included; 24 steps per volumetric layer, and
// a 2D layer's slant capped as the view caps it), so the ground is shadowed where the camera sees the disk hidden, and lit where it
// sees the disk through a cloud's eroded edge. Shape only (as before) left the Cloudy preset's ground at a mean transmittance of
// 0.25 under a view of 0.39 (30 degree sun, CPU replica): its median optical depth along the sun was 13.8 against the view's 7.0.
static const uint SKY_CLOUD_SHADOW_STEPS = 24u;

[numthreads(8, 8, 1)]
void CSCloudShadow(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= cloudShadowSize || dispatchThread.y >= cloudShadowSize)
    {
        return;
    }

    RWTexture2D<float> output = ResourceDescriptorHeap[cloudShadowOutputIndex];

    if(directionToSun.y <= 0.02f)
    {
        output[dispatchThread.xy] = 1.0f;
        return;
    }

    const float2 uv = (float2(dispatchThread.xy) + 0.5f) / float(cloudShadowSize);
    // The plane point in world metres, then in the clouds' world (km: x, altitude, z).
    const float2 worldXz = cloudShadowOrigin + (uv - 0.5f) * cloudShadowExtent;
    const float3 planePoint = float3(worldXz.x / 1000.0f, (cloudShadowReferenceY - cloudSeaLevel) / 1000.0f, worldXz.y / 1000.0f);
    float opticalDepth = 0.0f;

    for(uint index = 0u; index < min(cloudLayerCount, SKY_CLOUD_MAXIMUM_LAYERS); ++index)
    {
        const SkyCloudLayer layer = cloudLayers[index];

        if(layer.castShadows == 0u)
        {
            continue;
        }

        const float bottom = layer.bottomRadius - bottomRadius;
        const float top = layer.topRadius - bottomRadius;

        if(layer.kind == SKY_CLOUD_KIND_LAYER)
        {
            const float3 at = planePoint + directionToSun * ((bottom - planePoint.y) / directionToSun.y);
            opticalDepth += SkyCloudDensity(layer, at, 0.5f, true) * layer.density / max(directionToSun.y, 0.05f);
            continue;
        }

        const float start = (bottom - planePoint.y) / directionToSun.y;
        const float end = (top - planePoint.y) / directionToSun.y;
        const float stepLength = (end - start) / float(SKY_CLOUD_SHADOW_STEPS);

        for(uint step = 0u; step < SKY_CLOUD_SHADOW_STEPS; ++step)
        {
            const float3 at = planePoint + directionToSun * (start + (float(step) + 0.5f) * stepLength);
            const float heightFraction = saturate((at.y - bottom) / max(top - bottom, 1.0e-3f));
            opticalDepth += SkyCloudDensity(layer, at, heightFraction, true) * layer.density * stepLength;
        }
    }

    output[dispatchThread.xy] = exp(-opticalDepth);
}

// The 4x4 Bayer order: the traced pixel of every block in frame n is index n % 16.
static const uint SKY_CLOUD_BAYER[16] = {0u, 10u, 2u, 8u, 5u, 15u, 7u, 13u, 1u, 11u, 3u, 9u, 4u, 14u, 6u, 12u};

uint2 SkyCloudTracedPixel(uint2 block, uint frame)
{
    const uint index = SKY_CLOUD_BAYER[frame % 16u];
    return block * 4u + uint2(index & 3u, index >> 2u);
}

// The view direction through a cloud pixel: a point on its ray through the camera-relative inverse (the camera at the origin; depth
// 0.5, inside the frustum for standard and reversed depth alike).
float3 SkyCloudViewDirection(float2 pixel, float2 size)
{
    const float2 uv = pixel / size;
    const float4 onRay = mul(cloudInverseViewProjection, float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.5f, 1.0f));
    return normalize(onRay.xyz / onRay.w);
}

// One pixel of each 4x4 block of the quarter-resolution cloud image, marched with a per-frame jitter.
[numthreads(8, 8, 1)]
void CSCloudTrace(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= cloudTraceWidth || dispatchThread.y >= cloudTraceHeight)
    {
        return;
    }

    const uint2 pixel = min(SkyCloudTracedPixel(dispatchThread.xy, cloudFrame), uint2(cloudWidth - 1u, cloudHeight - 1u));
    const float3 direction = SkyCloudViewDirection(float2(pixel) + 0.5f, float2(float(cloudWidth), float(cloudHeight)));
    const float jitter = frac(float(SkyCloudHash(pixel.x * 73856093u ^ pixel.y * 19349663u ^ cloudFrame * 83492791u) >> 8) /
        16777216.0f);
    const SkyAtmosphereParameters atmosphere = LoadSkyAtmosphere();
    const SkyCloudResult clouds = SkyMarchClouds(atmosphere, cameraAltitude, direction, jitter, 64u);
    RWTexture2D<float4> output = ResourceDescriptorHeap[cloudTraceOutputIndex];
    RWTexture2D<float> depthOutput = ResourceDescriptorHeap[cloudTraceDepthOutputIndex];
    output[dispatchThread.xy] = SkyCloudTexel(atmosphere, cameraAltitude, direction, clouds);
    depthOutput[dispatchThread.xy] = clouds.depth;
}

// Every quarter-resolution pixel: this frame's traced pixel of its block, or the history reprojected by its cloud depth (bilinear),
// or, when the history is invalid or the point falls outside the previous view, the block's traced pixel.
[numthreads(8, 8, 1)]
void CSCloudReproject(uint3 dispatchThread : SV_DispatchThreadID)
{
    if(dispatchThread.x >= cloudWidth || dispatchThread.y >= cloudHeight)
    {
        return;
    }

    const uint2 pixel = dispatchThread.xy;
    const uint2 block = pixel / 4u;
    Texture2D<float4> trace = ResourceDescriptorHeap[cloudTraceIndex];
    Texture2D<float> traceDepth = ResourceDescriptorHeap[cloudTraceDepthIndex];
    const float4 traced = trace.Load(int3(int2(block), 0));
    const float tracedDepth = traceDepth.Load(int3(int2(block), 0));
    float4 value = traced;
    float depth = tracedDepth;

    if(any(pixel != SkyCloudTracedPixel(block, cloudFrame)) && cloudHistoryValid != 0u)
    {
        Texture2D<float4> history = ResourceDescriptorHeap[cloudHistoryIndex];
        Texture2D<float> historyDepth = ResourceDescriptorHeap[cloudHistoryDepthIndex];
        const float previousDepth = historyDepth.Load(int3(int2(pixel), 0));
        const float3 direction = SkyCloudViewDirection(float2(pixel) + 0.5f, float2(float(cloudWidth), float(cloudHeight)));
        // The cloud point relative to the previous frame's camera.
        const float3 relative = direction * min(previousDepth, 1.0e4f) * 1000.0f + cloudCameraMove;
        const float4 clip = mul(cloudPreviousViewProjection, float4(relative, 1.0f));

        if(clip.w > 0.0f)
        {
            const float2 previousUv = float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);

            if(all(previousUv >= 0.0f) && all(previousUv <= 1.0f))
            {
                const float2 texel = previousUv * float2(float(cloudWidth), float(cloudHeight)) - 0.5f;
                const int2 base = int2(floor(texel));
                const float2 weight = texel - float2(base);
                const int2 last = int2(int(cloudWidth) - 1, int(cloudHeight) - 1);
                const int2 p00 = clamp(base, int2(0, 0), last);
                const int2 p11 = clamp(base + 1, int2(0, 0), last);
                const float4 top = lerp(history.Load(int3(p00, 0)), history.Load(int3(int2(p11.x, p00.y), 0)), weight.x);
                const float4 bottom = lerp(history.Load(int3(int2(p00.x, p11.y), 0)), history.Load(int3(p11, 0)), weight.x);
                value = lerp(top, bottom, weight.y);
                value.rgb *= cloudHistoryScale;
                depth = previousDepth;
            }
        }
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[cloudOutputIndex];
    RWTexture2D<float> depthOutput = ResourceDescriptorHeap[cloudDepthOutputIndex];
    output[pixel] = all(isfinite(value)) ? float4(max(value.rgb, 0.0f), saturate(value.a)) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    depthOutput[pixel] = depth;
}
