#ifndef COMMON_HLSLI
#define COMMON_HLSLI

static const float PI = 3.14159265f;

float2 DirectionToEquirectangularUV(float3 direction)
{
    direction = normalize(direction);

    float longitude =
        atan2(direction.z, direction.x);

    float latitude =
        asin(clamp(direction.y, -1.0f, 1.0f));

    float2 uv;

    uv.x =
        longitude / (2.0f * PI) +
        0.5f;

    uv.y =
        0.5f -
        latitude / PI;

    return uv;
}

// Physical sky S3: the sky-view map (SkyAtmosphere's per-set background). Columns follow the equirect's longitude; rows map
// latitude l by v = 0.5 - 0.5 sign(l) sqrt(|l| / (pi / 2)) (Hillaire 2020), so texels crowd at the horizon, where the sky
// changes fastest. v = 0 is the zenith, 0.5 the horizon, 1 the nadir.
float2 DirectionToSkyViewUV(float3 direction)
{
    direction = normalize(direction);
    float latitude = asin(clamp(direction.y, -1.0f, 1.0f));
    float row = sqrt(abs(latitude) / (0.5f * PI));
    return float2(atan2(direction.z, direction.x) / (2.0f * PI) + 0.5f, 0.5f - 0.5f * (latitude < 0.0f ? -row : row));
}

float3 SkyViewUVToDirection(float2 uv)
{
    float signedRow = (0.5f - uv.y) * 2.0f;
    float latitude = (signedRow < 0.0f ? -1.0f : 1.0f) * signedRow * signedRow * 0.5f * PI;
    float longitude = (uv.x - 0.5f) * 2.0f * PI;
    float cosLatitude = cos(latitude);
    return float3(cosLatitude * cos(longitude), sin(latitude), cosLatitude * sin(longitude));
}

// The sky background toward a direction from a sky-view map: bilinear by hand, so columns wrap at the longitude seam whatever the
// caller's sampler, and rows clamp at the poles.
float3 SampleSkyView(uint skyViewIndex, float3 direction)
{
    Texture2D<float4> skyView = ResourceDescriptorHeap[skyViewIndex];
    uint width;
    uint height;
    skyView.GetDimensions(width, height);
    float2 texel = DirectionToSkyViewUV(direction) * float2(float(width), float(height)) - 0.5f;
    float2 base = floor(texel);
    float2 weight = texel - base;
    int column0 = (int(base.x) % int(width) + int(width)) % int(width);
    int column1 = (column0 + 1) % int(width);
    int row0 = clamp(int(base.y), 0, int(height) - 1);
    int row1 = clamp(int(base.y) + 1, 0, int(height) - 1);
    float3 top = lerp(skyView.Load(int3(column0, row0, 0)).rgb, skyView.Load(int3(column1, row0, 0)).rgb, weight.x);
    float3 bottom = lerp(skyView.Load(int3(column0, row1, 0)).rgb, skyView.Load(int3(column1, row1, 0)).rgb, weight.x);
    return lerp(top, bottom, weight.y);
}

// Physical sky S2: the largest luminance a visible sun disk shows (camera rays, delta-mirror chains, the Legacy background), so
// it stays inside the RGBA16F scene colour (65504). Lighting uses the analytic sun, so no energy changes; today's 2,000 lux
// default sun (4.0e4 at EV_ref 10) is below it.
static const float SUN_DISK_DISPLAY_MAXIMUM = 60000.0f;

float3 ClampSunDiskRadiance(float3 radiance)
{
    float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
    return luminance > SUN_DISK_DISPLAY_MAXIMUM ? radiance * (SUN_DISK_DISPLAY_MAXIMUM / luminance) : radiance;
}

// Sun disk edge and limb (2026-11-25): Eddington's grey limb-darkening law, I(mu) / I(1) = 1 - u (1 - mu) with u = 0.6 (the visible
// Sun's linear coefficient). Its mean over the disk is 1 - u / 3, so dividing by that keeps the disk's irradiance.
static const float SUN_LIMB_DARKENING = 0.6f;

// A pixel's coverage of a disk of angular radius `radius` whose centre is `angle` away (radians): a linear ramp one footprint wide
// across the edge, the box-filtered edge of a pixel `footprint` radians wide. Footprint 0 is the hard edge.
float DiskEdgeCoverage(float angle, float radius, float footprint)
{
    return saturate((radius - angle) / max(footprint, 1.0e-7f) + 0.5f);
}

// The visible sun disk toward `direction` (camera rays, delta-mirror chains, the Legacy background): uniformRadiance is the disk's
// mean radiance, E / (pi sin^2 alpha), and cosRadius = cos alpha.
// - Limb darkening (SUN_LIMB_DARKENING), normalized: the disk carries the same irradiance as the uniform disk, so no lighting changes
//   (the analytic sun lights the scene; this is the disk seen).
// - Edge: the pixel's coverage over its angular footprint (DiskEdgeCoverage), so sub-pixel jitter moves the edge smoothly instead of
//   switching edge pixels between the sky and 60,000.
// - The display maximum (SUN_DISK_DISPLAY_MAXIMUM) on the disk's centre, the brightest point, so the profile survives it.
float3 SunDiskDisplayRadiance(float3 uniformRadiance, float3 directionToSun, float3 direction, float cosRadius, float footprint)
{
    if(!(cosRadius < 1.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 view = normalize(direction);
    const float angle = atan2(length(cross(view, directionToSun)), dot(view, directionToSun));
    const float sinRadius = sqrt(saturate((1.0f - cosRadius) * (1.0f + cosRadius)));
    const float radius = asin(sinRadius);
    const float coverage = DiskEdgeCoverage(angle, radius, footprint);

    if(coverage <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float normalization = 1.0f / (1.0f - SUN_LIMB_DARKENING / 3.0f);
    const float onDisk = saturate(sin(angle) / max(sinRadius, 1.0e-12f));
    const float mu = sqrt(saturate(1.0f - onDisk * onDisk));
    const float profile = (1.0f - SUN_LIMB_DARKENING * (1.0f - mu)) * normalization;
    const float centreLuminance = dot(uniformRadiance, float3(0.2126f, 0.7152f, 0.0722f)) * normalization;
    const float scale = centreLuminance > SUN_DISK_DISPLAY_MAXIMUM ? SUN_DISK_DISPLAY_MAXIMUM / centreLuminance : 1.0f;
    return uniformRadiance * (scale * profile * coverage);
}

// Physical sky S1: the last mip of a bound prefiltered or multiscatter cube, from the cube itself. Roughness r reads mip
// r * (mips - 1): the HDRI's 1024 cube has 11 mips (10, as before), the physical sky's 256 cube 9, both multiscatter cubes 7.
float EnvironmentCubeMaximumMip(TextureCube<float4> cube)
{
    uint width;
    uint height;
    uint mipCount;
    cube.GetDimensions(0u, width, height, mipCount);
    return float(max(mipCount, 1u) - 1u);
}

float3 EquirectangularUVToDirection(float2 uv)
{
    float longitude =
        (uv.x - 0.5f) *
        2.0f *
        PI;

    float latitude =
        (0.5f - uv.y) *
        PI;

    float cosLatitude =
        cos(latitude);

    return
        float3(
            cosLatitude * cos(longitude),
            sin(latitude),
            cosLatitude * sin(longitude)
        );
}

#endif