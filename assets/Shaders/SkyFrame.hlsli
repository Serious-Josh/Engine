#ifndef ENGINE_SKY_FRAME
#define ENGINE_SKY_FRAME

// Physical sky S5 (Physical_Sky_Design_Proposal.md §14.1, §14.3): what the frame's readers need from the physical sky beyond the
// environment: the per-pixel star field and the moon's disk (camera rays and delta misses), the cloud shadow on the sun, and the
// published set's cloud map (delta misses: the camera background is the cloud-free sky-view map, so camera rays get their clouds
// from the full-resolution composite only). One persistent StructuredBuffer element, written once per frame by SkyAtmosphere; its
// index is 0xFFFFFFFF while the physical sky does not light the frame. Every lookup is manual bilinear (no sampler).

#include "SkyAtmosphereCommon.hlsli"

struct SkyFrameInfoData
{
    uint flags;
    uint transmittanceIndex;
    uint transmittanceWidth;
    uint transmittanceHeight;

    // Kilometres.
    float bottomRadius;
    float topRadius;
    float cameraAltitude;
    // Render units per cd/m^2 of the star field (the render scale times the night sky intensity).
    float starRadiance;

    // Radians about +Y.
    float planetRotation;
    // A star's disk radius, radians.
    float starDiskRadius;
    uint cloudShadowIndex;
    uint cloudEnvironmentIndex;

    // The shadow plane: centre's world x and z (metres), 1 / extent, world y.
    float2 cloudShadowOrigin;
    float cloudShadowInvExtent;
    float cloudShadowReferenceY;

    float3 directionToSun;
    uint cloudShadowSize;

    float3 directionToMoon;
    float moonCosRadius;

    // The moon's disk at the camera (render units, display-clamped).
    float3 moonDiskRadiance;
    uint cloudEnvironmentWidth;

    uint cloudEnvironmentHeight;
    // S5B: the clouds' shadow plane along the moon.
    uint moonCloudShadowIndex;
    uint2 skyFramePadding;
};

static const uint SKY_FRAME_STARS = 1u;
static const uint SKY_FRAME_MOON = 2u;
static const uint SKY_FRAME_CLOUD_SHADOW = 4u;
static const uint SKY_FRAME_CLOUD_ENVIRONMENT = 8u;
static const uint SKY_FRAME_MOON_CLOUD_SHADOW = 16u;

// The star field (§14.1): cells of an equal-area grid on the celestial sphere (2048 in longitude by 1024 in sine of latitude), about
// 9,000 stars with N(<m) proportional to 10^(0.43 m) down to magnitude 6.5, each a disk of starDiskRadius whose radiance carries
// its illuminance E(m) = 2.54e-6 * 10^(-0.4 m) lux. SkyAtmosphere bakes the same field's mean radiance into the equirect.
static const float SKY_STAR_CELL_PROBABILITY = 9000.0f / (2048.0f * 1024.0f);

uint SkyFrameHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float SkyFrameUnit(uint hash)
{
    return float(hash >> 8) * (1.0f / 16777216.0f);
}

SkyFrameInfoData LoadSkyFrameInfo(uint index)
{
    StructuredBuffer<SkyFrameInfoData> buffer = ResourceDescriptorHeap[index];
    return buffer[0];
}

// The air's transmittance from the camera to the top of the atmosphere along a direction; 0 where the planet hides the direction.
float3 SkyFrameViewTransmittance(SkyFrameInfoData info, float3 direction)
{
    SkyAtmosphereParameters atmosphere = (SkyAtmosphereParameters)0;
    atmosphere.bottomRadius = info.bottomRadius;
    atmosphere.topRadius = info.topRadius;
    const SkyRay ray = MakeSkyRay(atmosphere, info.cameraAltitude, direction);

    if(SkyDistanceToGround(ray) >= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float2 size = float2(float(info.transmittanceWidth), float(info.transmittanceHeight));
    const float2 texel = SkyTransmittanceLutUv(atmosphere, ray.radius, ray.rho2, direction.y, size) * size - 0.5f;
    const int2 base = int2(floor(texel));
    const float2 weight = texel - float2(base);
    const int2 last = int2(int(info.transmittanceWidth) - 1, int(info.transmittanceHeight) - 1);
    const int2 p00 = clamp(base, int2(0, 0), last);
    const int2 p11 = clamp(base + 1, int2(0, 0), last);
    Texture2D<float4> lut = ResourceDescriptorHeap[info.transmittanceIndex];
    const float3 top = lerp(lut.Load(int3(p00, 0)).rgb, lut.Load(int3(int2(p11.x, p00.y), 0)).rgb, weight.x);
    const float3 bottom = lerp(lut.Load(int3(int2(p00.x, p11.y), 0)).rgb, lut.Load(int3(p11, 0)).rgb, weight.x);
    return lerp(top, bottom, weight.y);
}

// The stars seen along a direction (render units), through the atmosphere.
// S5B: footprint (radians) is the angular size of the pixel the direction is drawn in (0 where samples accumulate, as in the path
// tracer). A star is drawn as a cone kernel of radius max(starDiskRadius, 1.5 footprint) carrying its whole illuminance
// (3 E / (pi R^2) at the centre, falling linearly to 0 at R), so a star smaller than a pixel is spread over the pixels around it
// instead of appearing and vanishing as the view moves (the shimmer without anti-aliasing). The 3x3 cells around the direction
// are searched, since a kernel can reach past its own cell.
float3 SkyFrameStars(SkyFrameInfoData info, float3 direction, float footprint)
{
    if((info.flags & SKY_FRAME_STARS) == 0u || info.starRadiance <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float c = cos(info.planetRotation);
    const float s = sin(info.planetRotation);
    const float3 d = normalize(float3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z));
    const float u = atan2(d.z, d.x) / (2.0f * PI) + 0.5f;
    const float v = d.y * 0.5f + 0.5f;
    const int2 cell = int2(min(uint2(u * 2048.0f, v * 1024.0f), uint2(2047u, 1023u)));
    const float kernelRadius = max(info.starDiskRadius, 1.5f * footprint);
    const float kernelNormalization = 3.0f / (PI * kernelRadius * kernelRadius);
    const float3 warm = float3(1.0f, 0.85f, 0.7f);
    const float3 cool = float3(0.8f, 0.9f, 1.1f);
    float3 radiance = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for(int offset = 0; offset < 9; ++offset)
    {
        const int2 neighbour = int2(cell.x + offset % 3 - 1, cell.y + offset / 3 - 1);

        if(neighbour.y < 0 || neighbour.y > 1023)
        {
            continue;
        }

        const uint2 at = uint2((neighbour.x + 2048) % 2048, neighbour.y);
        const uint hash = SkyFrameHash(at.x * 1973u ^ SkyFrameHash(at.y * 9277u + 26699u));

        if(SkyFrameUnit(hash) >= SKY_STAR_CELL_PROBABILITY)
        {
            continue;
        }

        const uint h1 = SkyFrameHash(hash);
        const uint h2 = SkyFrameHash(h1);
        const uint h3 = SkyFrameHash(h2);
        const uint h4 = SkyFrameHash(h3);
        const float starU = (float(at.x) + 0.2f + 0.6f * SkyFrameUnit(h1)) / 2048.0f;
        const float starV = (float(at.y) + 0.2f + 0.6f * SkyFrameUnit(h2)) / 1024.0f;
        const float phi = (starU - 0.5f) * 2.0f * PI;
        const float y = starV * 2.0f - 1.0f;
        const float r = sqrt(saturate(1.0f - y * y));
        const float3 star = float3(r * cos(phi), y, r * sin(phi));
        // The angle to the star (chord length: within 1e-7 of the arc at these sizes).
        const float distance = length(d - star);

        if(distance >= kernelRadius)
        {
            continue;
        }

        // The fraction of stars brighter than this one sets its magnitude.
        const float magnitude = max(6.5f + log10(max(SkyFrameUnit(h3), 1.0e-6f)) / 0.43f, -1.5f);
        const float illuminance = 2.54e-6f * pow(10.0f, -0.4f * magnitude);
        float3 tint = lerp(warm, cool, SkyFrameUnit(h4));
        tint /= dot(tint, float3(0.2126f, 0.7152f, 0.0722f));
        radiance += tint * (illuminance * kernelNormalization * (1.0f - distance / kernelRadius));
    }

    return radiance * info.starRadiance * SkyFrameViewTransmittance(info, direction);
}

// The moon's disk along a direction (render units), or 0. Sun disk edge (2026-11-25): its edge covered over the pixel's footprint
// (radians; 0 the hard edge), as the sun's, so jitter does not switch edge pixels on and off. No limb darkening (the moon has none
// worth modelling).
float3 SkyFrameMoonDisk(SkyFrameInfoData info, float3 direction, float footprint)
{
    if((info.flags & SKY_FRAME_MOON) == 0u || !(info.moonCosRadius < 1.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 view = normalize(direction);
    const float angle = atan2(length(cross(view, info.directionToMoon)), dot(view, info.directionToMoon));
    const float radius = asin(sqrt(saturate((1.0f - info.moonCosRadius) * (1.0f + info.moonCosRadius))));
    return info.moonDiskRadiance * DiskEdgeCoverage(angle, radius, footprint);
}

// The night sky drawn on camera rays and delta misses: the stars and the moon's disk. footprint: SkyFrameStars' (S5B);
// diskFootprint: the moon's edge filter (negative: footprint).
float3 SkyFrameNightBackground(uint infoIndex, float3 direction, float footprint = 0.0f, float diskFootprint = -1.0f)
{
    if(infoIndex == 0xFFFFFFFFu)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const SkyFrameInfoData info = LoadSkyFrameInfo(infoIndex);
    return SkyFrameStars(info, direction, footprint) + SkyFrameMoonDisk(info, direction, diskFootprint < 0.0f ? footprint : diskFootprint);
}

// One shadow plane (the sun's or, S5B, the moon's) along its light's direction: bilinear, 1 outside the plane or with the light
// lower than about 1 degree.
float SkyFrameShadowPlane(SkyFrameInfoData info, uint shadowIndex, float3 toLight, float3 worldPosition)
{
    if(toLight.y <= 0.02f)
    {
        return 1.0f;
    }

    const float3 onPlane = worldPosition + toLight * ((info.cloudShadowReferenceY - worldPosition.y) / toLight.y);
    const float2 uv = (onPlane.xz - info.cloudShadowOrigin) * info.cloudShadowInvExtent + 0.5f;

    if(any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1.0f;
    }

    Texture2D<float> shadow = ResourceDescriptorHeap[shadowIndex];
    const float2 texel = uv * float(info.cloudShadowSize) - 0.5f;
    const int2 base = int2(floor(texel));
    const float2 weight = texel - float2(base);
    const int2 last = int2(int(info.cloudShadowSize) - 1, int(info.cloudShadowSize) - 1);
    const int2 p00 = clamp(base, int2(0, 0), last);
    const int2 p11 = clamp(base + 1, int2(0, 0), last);
    const float top = lerp(shadow.Load(int3(p00, 0)), shadow.Load(int3(int2(p11.x, p00.y), 0)), weight.x);
    const float bottom = lerp(shadow.Load(int3(int2(p00.x, p11.y), 0)), shadow.Load(int3(p11, 0)), weight.x);
    return saturate(lerp(top, bottom, weight.y));
}

// The clouds' transmittance of the sun at a world position (metres): the cloud shadow plane, read along the sun's line. 1 without
// shadows, outside the plane, or with the sun lower than about 1 degree.
float SkyFrameCloudShadow(uint infoIndex, float3 worldPosition)
{
    if(infoIndex == 0xFFFFFFFFu)
    {
        return 1.0f;
    }

    const SkyFrameInfoData info = LoadSkyFrameInfo(infoIndex);
    return (info.flags & SKY_FRAME_CLOUD_SHADOW) == 0u ? 1.0f :
        SkyFrameShadowPlane(info, info.cloudShadowIndex, info.directionToSun, worldPosition);
}

// S5B: the clouds' shadow on the moon (its table light), from the moon's own plane.
float SkyFrameMoonCloudShadow(uint infoIndex, float3 worldPosition)
{
    if(infoIndex == 0xFFFFFFFFu)
    {
        return 1.0f;
    }

    const SkyFrameInfoData info = LoadSkyFrameInfo(infoIndex);
    return (info.flags & SKY_FRAME_MOON_CLOUD_SHADOW) == 0u ? 1.0f :
        SkyFrameShadowPlane(info, info.moonCloudShadowIndex, info.directionToMoon, worldPosition);
}

// The published set's cloud map along a direction: (in-scattered light, background transmittance); (0, 1) without clouds.
float4 SkyFrameCloudEnvironment(uint infoIndex, float3 direction)
{
    if(infoIndex == 0xFFFFFFFFu)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const SkyFrameInfoData info = LoadSkyFrameInfo(infoIndex);

    if((info.flags & SKY_FRAME_CLOUD_ENVIRONMENT) == 0u)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    Texture2D<float4> clouds = ResourceDescriptorHeap[info.cloudEnvironmentIndex];
    const float2 texel = DirectionToEquirectangularUV(direction) * float2(float(info.cloudEnvironmentWidth),
        float(info.cloudEnvironmentHeight)) - 0.5f;
    const float2 base = floor(texel);
    const float2 weight = texel - base;
    const int width = int(info.cloudEnvironmentWidth);
    const int column0 = (int(base.x) % width + width) % width;
    const int column1 = (column0 + 1) % width;
    const int row0 = clamp(int(base.y), 0, int(info.cloudEnvironmentHeight) - 1);
    const int row1 = clamp(int(base.y) + 1, 0, int(info.cloudEnvironmentHeight) - 1);
    const float4 top = lerp(clouds.Load(int3(column0, row0, 0)), clouds.Load(int3(column1, row0, 0)), weight.x);
    const float4 bottom = lerp(clouds.Load(int3(column0, row1, 0)), clouds.Load(int3(column1, row1, 0)), weight.x);
    return lerp(top, bottom, weight.y);
}

#endif
