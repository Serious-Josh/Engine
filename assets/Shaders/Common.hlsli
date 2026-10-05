#ifndef COMMON_HLSLI
#define COMMON_HLSLI

static const float PI = 3.14159265f;

float3 ApplyTestTint(float3 color)
{
    return color * float3(1.0f, 0.5f, 0.5f);
}

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