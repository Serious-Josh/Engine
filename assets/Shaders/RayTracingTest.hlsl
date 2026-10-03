struct RayPayload
{
    float3 color;
};

struct CameraData
{
    float4x4 inverseViewProjection;
    float3 cameraPosition;
    float padding;
};

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> outputTexture : register(u0);
ConstantBuffer<CameraData> camera : register(b0);

[shader("raygeneration")]
void RayGeneration()
{
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