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
        AOHash(pixel.y ^ 0x63D83595u));
    // An odd multiplier is coprime to 16: this permutes every stratum once.
    uint temporalSample = sampleIndex % 16u;
    uint stride = (AOHash(seed ^ 0x369DEA0Fu) & 15u) | 1u;
    uint stratum = (temporalSample * stride + (seed & 15u)) & 15u;
    uint jitterSeed = AOHash(seed ^ AOHash(stratum ^ 0xD1B54A35u));
    // Use 20 bits so adding jitter to a stratum coordinate cannot round to 4.
    float jitterX = float(AOHash(jitterSeed ^ 0xA24BAED5u) >> 12u) * (1.0f / 1048576.0f);
    float jitterY = float(AOHash(jitterSeed ^ 0x9FB21C65u) >> 12u) * (1.0f / 1048576.0f);
    float u1 = (float(stratum & 3u) + jitterX) * 0.25f;
    float u2 = (float(stratum >> 2u) + jitterY) * 0.25f;

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
    const uint2 aoPixel = DispatchRaysIndex().xy;
    aoTexture[aoPixel] = 1.0f;

    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[surfaceIndex];

    uint2 fullResolution;
    depthTexture.GetDimensions(fullResolution.x, fullResolution.y);

    if(any(fullResolution == 0u))
    {
        return;
    }

    // Preserve full-resolution dispatch; otherwise select the center of each 2x2 footprint.
    const uint2 fullPixel = all(DispatchRaysDimensions().xy == fullResolution)
        ? aoPixel
        : min(aoPixel * 2u + 1u, fullResolution - 1u);

    const float depth = depthTexture.Load(int3(fullPixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return;
    }

    // Match GI: pixel centers, flipped NDC Y, D3D [0, 1] depth,
    // and a row-major inverse view-projection multiplying a column vector.
    const float2 uv = (float2(fullPixel) + 0.5f) / float2(fullResolution);
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
    const float3 surfaceNormal = surfaceTexture.Load(int3(fullPixel, 0)).rgb;
    const float normalLengthSquared = dot(surfaceNormal, surfaceNormal);

    if(!all(isfinite(surfaceNormal)) || !isfinite(normalLengthSquared) ||
       normalLengthSquared < 1.0e-8f)
    {
        return;
    }

    const float3 normal = normalize(surfaceNormal);

    RayDesc ray;
    ray.Origin = worldPosition.xyz + normal * originBias;
    ray.Direction = SampleAOCosineHemisphere(normal, aoPixel, frameIndex);
    ray.TMin = rayTMin;
    ray.TMax = maxDistance;

    AOPayload payload;
    payload.visible = 0u; // A hit preserves zero because closest-hit is skipped.
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF, 0, 1, 0, ray, payload);

    aoTexture[aoPixel] = float(payload.visible);
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
