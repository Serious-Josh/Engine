struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
    float4 currentClipPosition : TEXCOORD1;
    float4 previousClipPosition : TEXCOORD2;
    float2 viewDepths : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
};

struct PSOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 motion : SV_Target2;
    float4 geometricNormal : SV_Target3;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
    float roughness;
    uint materialIndex;
    uint normalTextureIndex;

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
    output.worldTangent = float4(
        normalize(mul((float3x3)normalMatrix, input.tangent.xyz)),
        input.tangent.w
    );

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

    float3 N = normalize(input.worldNormal);
    float3 mappedNormal = N;

    if(normalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalTextureIndex];
        float3 sampledNormal = normalTexture.Sample(sampler0, input.uv).rgb * 2.0f - 1.0f;
        float3 tangent = input.worldTangent.xyz - N * dot(N, input.worldTangent.xyz);
        float tangentLengthSquared = dot(tangent, tangent);
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);
        float handedness = input.worldTangent.w;
        if(all(isfinite(N)) && all(isfinite(tangent)) &&
           isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
           all(isfinite(sampledNormal)) && isfinite(sampledLengthSquared) &&
           sampledLengthSquared > 1.0e-8f && isfinite(handedness) && abs(handedness) > 1.0e-8f)
        {
            float3 T = normalize(tangent);
            float3 B = cross(N, T) * handedness;
            float3 worldMappedNormal = T * sampledNormal.x + B * sampledNormal.y + N * sampledNormal.z;
            float mappedLengthSquared = dot(worldMappedNormal, worldMappedNormal);
            if(all(isfinite(B)) && all(isfinite(worldMappedNormal)) &&
               isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
                mappedNormal = normalize(worldMappedNormal);
        }
    }

    output.surface = float4(mappedNormal, float(materialIndex));

    output.geometricNormal = float4(normalize(input.worldNormal), 0.0f);

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
