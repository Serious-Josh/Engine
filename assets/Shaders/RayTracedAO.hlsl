struct AOPayload
{
    uint visible;
};

cbuffer AOConstants : register(b0)
{
    row_major float4x4 inverseViewProjection;

    uint depthIndex;
    uint surfaceIndex;
    uint frameIndex;
    uint padding0;

    float maxDistance;
    float originBias;
    float rayTMin;
    float padding1;
};

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float> aoTexture : register(u0);

uint AOHash(uint value)
{
    // Match GI's 32-bit MurmurHash3 finalizer.
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

float3 SampleAOCosineHemisphere(float3 normal, uint2 pixel, uint sampleIndex)
{
    uint seed = AOHash(
        AOHash(pixel.x ^ 0xA511E9B3u) ^
        AOHash(pixel.y ^ 0x63D83595u) ^
        AOHash(sampleIndex ^ 0x369DEA0Fu));
    // Match GI's dimension salts and upper-24-bit mapping to [0, 1).
    float u1 = float(AOHash(seed ^ 0xA24BAED5u) >> 8u) * (1.0f / 16777216.0f);
    float u2 = float(AOHash(seed ^ 0x9FB21C65u) >> 8u) * (1.0f / 16777216.0f);

    float r = sqrt(u1);
    float phi = 6.28318530718f * u2;
    float3 localDirection = float3(
        r * cos(phi),
        r * sin(phi),
        sqrt(max(0.0f, 1.0f - u1)));

    float3 tangent = normalize(abs(normal.z) < 0.999f
        ? cross(float3(0.0f, 0.0f, 1.0f), normal)
        : cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDirection.x + bitangent * localDirection.y +
        normal * localDirection.z);
}

[shader("raygeneration")]
void AORayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    aoTexture[pixel] = 1.0f;

    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[surfaceIndex];

    const float depth = depthTexture.Load(int3(pixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return;
    }

    // Match GI: pixel centers, flipped NDC Y, D3D [0, 1] depth,
    // and a row-major inverse view-projection multiplying a column vector.
    const float2 uv = (float2(pixel) + 0.5f) / float2(DispatchRaysDimensions().xy);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(inverseViewProjection, float4(ndc, depth, 1.0f));

    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
    {
        return;
    }

    worldPosition /= worldPosition.w;

    if(!all(isfinite(worldPosition)))
    {
        return;
    }

    // Surface RGB stores the world-space normal directly, without remapping.
    const float3 surfaceNormal = surfaceTexture.Load(int3(pixel, 0)).rgb;
    const float normalLengthSquared = dot(surfaceNormal, surfaceNormal);

    if(!all(isfinite(surfaceNormal)) || !isfinite(normalLengthSquared) ||
       normalLengthSquared < 1.0e-8f)
    {
        return;
    }

    const float3 normal = normalize(surfaceNormal);

    RayDesc ray;
    ray.Origin = worldPosition.xyz + normal * originBias;
    ray.Direction = SampleAOCosineHemisphere(normal, pixel, frameIndex);
    ray.TMin = rayTMin;
    ray.TMax = maxDistance;

    AOPayload payload;
    payload.visible = 0u; // A hit preserves zero because closest-hit is skipped.
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF, 0, 1, 0, ray, payload);

    aoTexture[pixel] = float(payload.visible);
}

[shader("miss")]
void AOMiss(inout AOPayload payload)
{
    payload.visible = 1u;
}

[shader("closesthit")]
void AOClosestHit(
    inout AOPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    // Export required by AOHitGroup; visibility rays skip execution.
    payload.visible = 0u;
}
