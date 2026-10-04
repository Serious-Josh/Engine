struct ShadowPayload
{
    uint visible;
};

struct ShadowConstants
{
    row_major float4x4 inverseViewProjection;

    float3 lightDirection;
    uint depthIndex;

    uint surfaceIndex;
    float3 padding;
};

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float> visibilityTexture :
    register(u0);

ConstantBuffer<ShadowConstants> shadowConstants :
    register(b0);

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pixelIndex =
        DispatchRaysIndex().xy;

    uint2 dimensions =
        DispatchRaysDimensions().xy;

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            shadowConstants.depthIndex
        ];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            shadowConstants.surfaceIndex
        ];

    float depth =
        depthTexture.Load(
            int3(
                pixelIndex,
                0
            )
        );

    if(depth >= 1.0f)
    {
        visibilityTexture[pixelIndex] =
            1.0f;

        return;
    }

    float2 uv =
        (float2(pixelIndex) + 0.5f) /
        float2(dimensions);

    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            depth,
            1.0f
        );

    float4 worldPosition =
        mul(
            shadowConstants.inverseViewProjection,
            clipPosition
        );

    worldPosition /=
        worldPosition.w;

    float3 normal =
        normalize(
            surfaceTexture.Load(
                int3(
                    pixelIndex,
                    0
                )
            ).rgb
        );

    float3 directionToLight =
        normalize(
            -shadowConstants.lightDirection
        );

    RayDesc ray;

    ray.Origin =
        worldPosition.xyz +
        normal * 0.001f;

    ray.Direction =
        directionToLight;

    ray.TMin =
        0.001f;

    ray.TMax =
        10000.0f;

    ShadowPayload payload;
    payload.visible = 0;

    TraceRay(
        scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        0,
        1,
        0,
        ray,
        payload
    );

    visibilityTexture[pixelIndex] =
        payload.visible != 0
            ? 1.0f
            : 0.0f;
}

[shader("miss")]
void Miss(
    inout ShadowPayload payload)
{
    payload.visible = 1;
}

[shader("closesthit")]
void ClosestHit(
    inout ShadowPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.visible = 0;
}