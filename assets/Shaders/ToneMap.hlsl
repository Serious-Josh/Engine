struct ToneMapConstants
{
    uint sceneColorIndex;
    float exposure;
};

ConstantBuffer<ToneMapConstants> toneMapConstants : register(b0);

static const float3x3 AgXInsetMatrix = float3x3(
    0.856627153315983, 0.137318972929847, 0.111898212999950,
    0.095121240538159, 0.761241990602591, 0.076799418603190,
    0.048251606145858, 0.101439036467562, 0.811302368396859
);

static const float3x3 AgXOutsetMatrix = float3x3(
     1.127100581814437, -0.141329763498438, -0.141329763498438,
    -0.110606643096603,  1.157823702216272, -0.110606643096603,
    -0.016493938717835, -0.016493938717835,  1.251936406595041
);

float3 AgXContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    float3 x6 = x4 * x2;

    return -17.86 * x6 * x
         + 78.01 * x6
         - 126.7 * x4 * x
         + 92.06 * x4
         - 28.72 * x2 * x
         + 4.361 * x2
         - 0.1718 * x
         + 0.002857;
}

float3 AgX(float3 color)
{
    color = max(color, 0.0);

    color = mul(AgXInsetMatrix, color);

    color = max(color, 1e-10);
    color = log2(color);

    const float minEV = -12.47393;
    const float maxEV = 4.026069;

    color = saturate(
        (color - minEV) / (maxEV - minEV)
    );

    color = AgXContrastApprox(color);

    // Neutral/default AgX look.
    // Golden/Punchy looks can be added later as optional controls.

    color = mul(AgXOutsetMatrix, color);

    // AgX output linearization.
    color = pow(max(color, 0.0), 2.2);

    return color;
}

float3 LinearToSRGB(float3 color)
{
    color = max(color, 0.0);

    float3 low = color * 12.92;

    float3 high =
        1.055 * pow(color, 1.0 / 2.4) - 0.055;

    return lerp(
        high,
        low,
        step(color, 0.0031308)
    );
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 position;

    if(vertexID == 0){
        position = float2(-1.0, -1.0);
    }
    else if(vertexID == 1){
        position = float2(-1.0, 3.0);
    }
    else{
        position = float2(3.0, -1.0);
    }

    output.position = float4(position, 0.0, 1.0);

    output.uv = float2(
        position.x * 0.5 + 0.5,
        0.5 - position.y * 0.5
    );

    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    Texture2D<float4> sceneColor =
        ResourceDescriptorHeap[toneMapConstants.sceneColorIndex];

    uint2 pixel = uint2(input.position.xy);

    float3 hdrColor =
        sceneColor.Load(int3(pixel, 0)).rgb;

    hdrColor *= toneMapConstants.exposure;

    float3 displayLinear = AgX(hdrColor);

    float3 displaySRGB = LinearToSRGB(displayLinear);

    return float4(displaySRGB, 1.0);
}