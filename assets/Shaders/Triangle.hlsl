struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
    float4 currentClipPosition : TEXCOORD1;
    float4 previousClipPosition : TEXCOORD2;
    float2 viewDepths : TEXCOORD3;
};

struct PSOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 motion : SV_Target2;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
    float roughness;
    uint materialIndex;
    float padding;

    float3 baseColor;
    float padding2;
}

cbuffer CameraConstants : register(b1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 view;
    row_major float4x4 previousView;
}

cbuffer ObjectConstants : register(b2)
{
    row_major float4x4 model;
    row_major float4x4 previousModel;
    row_major float4x4 normalMatrix;
}

SamplerState sampler0 : register(s0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    float4 worldPosition =
        mul(
            model,
            float4(
                input.position,
                1.0f
            )
        );

    output.position =
        mul(
            viewProjection,
            worldPosition
        );

    output.currentClipPosition = output.position;

    float4 previousWorldPosition =
        mul(
            previousModel,
            float4(input.position, 1.0f)
        );

    output.previousClipPosition =
        mul(
            previousViewProjection,
            previousWorldPosition
        );

    output.viewDepths = float2(
        mul(view, worldPosition).z,
        mul(previousView, previousWorldPosition).z
    );

    output.worldNormal = mul((float3x3)normalMatrix, input.normal);

    output.uv =
        input.uv;

    return output;
}

PSOutput PSMain(VSOutput input)
{
    PSOutput output;

    Texture2D<float4> baseColorTexture =
        ResourceDescriptorHeap[
            textureIndex
        ];

    float3 sampledBaseColor =
        baseColorTexture.Sample(
            sampler0,
            input.uv
        ).rgb;

    float3 finalBaseColor =
        baseColor *
        sampledBaseColor;

    output.material =
        float4(
            finalBaseColor,
            roughness
        );

    output.surface =
        float4(
            normalize(
                input.worldNormal
            ),
            float(materialIndex)
        );

    output.motion = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if(all(isfinite(input.currentClipPosition)) &&
       all(isfinite(input.previousClipPosition)) &&
       all(isfinite(input.viewDepths)) &&
       abs(input.currentClipPosition.w) > 1.0e-6f &&
       abs(input.previousClipPosition.w) > 1.0e-6f)
    {
        float2 currentNDC =
            input.currentClipPosition.xy / input.currentClipPosition.w;
        float2 previousNDC =
            input.previousClipPosition.xy / input.previousClipPosition.w;

        float2 currentUV =
            currentNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        float2 previousUV =
            previousNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

        float2 motion = currentUV - previousUV;

        float4 motionVector = float4(
            motion,
            input.viewDepths.y - input.viewDepths.x,
            0.0f
        );

        if(all(isfinite(motionVector)))
        {
            output.motion = motionVector;
        }
    }

    return output;
}