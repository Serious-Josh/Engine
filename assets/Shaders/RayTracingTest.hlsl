struct RayPayload
{
    float3 color;
};

struct CameraData
{
    row_major float4x4 inverseViewProjection;
    float3 cameraPosition;
    float padding;
};

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> outputTexture : register(u0);
ConstantBuffer<CameraData> camera : register(b0);

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pixelIndex = DispatchRaysIndex().xy;
    uint2 dimensions = DispatchRaysDimensions().xy;

    float2 uv =
        (float2(pixelIndex) + 0.5f) /
        float2(dimensions);

    float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f
    );

    float4 worldFar = mul(
        camera.inverseViewProjection,
        float4(ndc, 1.0f, 1.0f)
    );

    worldFar.xyz /= worldFar.w;

    RayDesc ray;
    ray.Origin = camera.cameraPosition;
    ray.Direction = normalize(
        worldFar.xyz - camera.cameraPosition
    );
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    RayPayload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);

    TraceRay(
        scene,
        RAY_FLAG_NONE,
        0xFF,
        0,
        1,
        0,
        ray,
        payload
    );

    outputTexture[pixelIndex] =
        float4(payload.color, 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.color = float3(0.1f, 0.2f, 0.4f);
}

[shader("closesthit")]
void ClosestHit(
    inout RayPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.color = float3(1.0f, 0.0f, 1.0f);
}