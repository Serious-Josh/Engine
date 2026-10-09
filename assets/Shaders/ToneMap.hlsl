#include "NRD/NRD.hlsli"

struct ToneMapConstants
{
    uint sceneColorIndex;
    float exposure;
    uint debugTextureIndex;
    uint debugView;
    float nrdDenoisingRange;

    uint selectionMaskIndex;
    uint outlineThickness;
    uint debugDepthIndex;
    float3 outlineColor;
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
    uint2 pixel = uint2(input.position.xy);
    if(toneMapConstants.debugView == 17 || toneMapConstants.debugView == 18)
    {
        Texture2D<float> depthTexture = ResourceDescriptorHeap[toneMapConstants.debugDepthIndex];
        float depth = depthTexture.Load(int3(pixel, 0));
        if(!isfinite(depth) || depth < 0.0f)
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        if(depth >= 1.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        Texture2D<float4> gBufferTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float3 value = gBufferTexture.Load(int3(pixel, 0)).rgb;
        if(!all(isfinite(value)))
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        if(toneMapConstants.debugView == 17)
        {
            // The surface target stores signed world-space XYZ.
            float lengthSquared = dot(value, value);
            if(!isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
                return float4(1.0f, 0.0f, 1.0f, 1.0f);
            return float4(normalize(value) * 0.5f + 0.5f, 1.0f);
        }
        // G-buffer RGB is linear after sRGB albedo sampling. Encode only for display.
        return float4(LinearToSRGB(value), 1.0f);
    }
    if(toneMapConstants.debugView == 1)
    {
        Texture2D<float> trackingDistance =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float distance = trackingDistance.Load(int3(uint2(input.position.xy), 0));
        if(distance == -1.0f)
            return float4(0.0f, 0.0f, 1.0f, 1.0f);
        if(distance > 0.0f)
        {
            float gray = distance / (distance + 5.0f);
            return float4(gray, gray, gray, 1.0f);
        }
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    if(toneMapConstants.debugView == 2)
    {
        Texture2D<float> viewZTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float z = viewZTexture.Load(int3(pixel, 0));
        if(!isfinite(z) || z <= 0.0f)
            return float4(1, 0, 0, 1); // Invalid depth.
        if(z > toneMapConstants.nrdDenoisingRange)
            return float4(1, 0, 1, 1);
        float gray = z / (z + 10.0f);
        return float4(gray.xxx, 1);
    }
    if(toneMapConstants.debugView >= 3 && toneMapConstants.debugView <= 7)
    {
        Texture2D<float4> preparedTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float4 prepared = preparedTexture.Load(int3(pixel, 0));
        if(!all(isfinite(prepared)))
            return float4(1, 0, 1, 1);
        if(toneMapConstants.debugView == 3 || toneMapConstants.debugView == 4)
        {
            float4 decoded = NRD_FrontEnd_UnpackNormalAndRoughness(prepared);
            return float4(toneMapConstants.debugView == 3 ? decoded.xyz * 0.5f + 0.5f : decoded.www, 1);
        }
        if(toneMapConstants.debugView == 5)
        {
            // Exact zero is black. Amplify absolute world-space XYZ components;
            // any nonzero component gets a visible minimum intensity.
            float3 components = abs(prepared.xyz);
            float3 display = max(saturate(components * 100.0f), step(1.0e-30f, components) * 0.25f);
            return float4(display, 1);
        }
        // The preparation helper stores YCoCg, not ordinary RGB. The REBLUR
        // backend unpack helper is also the inverse for this frontend packing.
        float4 decoded = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(prepared);
        if(toneMapConstants.debugView == 7)
            return float4(saturate(decoded.www), 1);
        return float4(LinearToSRGB(AgX(max(decoded.xyz, 0.0f))), 1);
    }

    if(toneMapConstants.debugView == 9)
    {
        Texture2D<float4> motionTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float3 motion = motionTexture.Load(int3(pixel, 0)).xyz;
        float3 display = saturate(0.5f + motion * float3(2000.0f, 2000.0f, 10.0f));
        return float4(display, 1.0f);
    }

    if(toneMapConstants.debugView == 10)
    {
        Texture2D<float> selectionMask =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];

        float mask = selectionMask.Load(int3(pixel, 0));

        return float4(mask, mask, mask, 1.0f);
    }

    if(toneMapConstants.debugView == 11 || toneMapConstants.debugView == 13)
    {
        Texture2D<float4> rawGI =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        return float4(rawGI.Load(int3(pixel, 0)).rgb, 1.0f);
    }

    if(toneMapConstants.debugView == 12)
    {
        Texture2D<float> giHitDistance =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float distance = giHitDistance.Load(int3(pixel, 0));
        if(distance == -1.0f)
            return float4(0.0f, 0.0f, 1.0f, 1.0f);
        if(isfinite(distance) && distance > 0.0f)
        {
            float gray = distance / (distance + 5.0f);
            return float4(gray, gray, gray, 1.0f);
        }
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    if(toneMapConstants.debugView == 14 ||
       toneMapConstants.debugView == 15 ||
       toneMapConstants.debugView == 16)
    {
        Texture2D<float> aoTexture =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float visibility = aoTexture.Load(int3(pixel, 0));
        return float4(visibility, visibility, visibility, 1.0f);
    }

    Texture2D<float4> sceneColor =
        // Mode 8 is already unpacked, remodulated linear HDR reflection RGB.
        ResourceDescriptorHeap[toneMapConstants.debugView == 8
            ? toneMapConstants.debugTextureIndex : toneMapConstants.sceneColorIndex];

    float3 hdrColor =
        sceneColor.Load(int3(pixel, 0)).rgb;

    if(toneMapConstants.debugView == 24u || toneMapConstants.debugView == 25u || toneMapConstants.debugView == 27u)
        return float4(all(isfinite(hdrColor)) ? hdrColor : float3(1.0f, 0.0f, 1.0f), 1.0f);

    hdrColor *= toneMapConstants.exposure;
    if(((toneMapConstants.debugView >= 19u && toneMapConstants.debugView <= 23u) || toneMapConstants.debugView == 26u ||
        (toneMapConstants.debugView >= 28u && toneMapConstants.debugView <= 30u)) && !all(isfinite(hdrColor)))
        return float4(1.0f, 0.0f, 1.0f, 1.0f);

    float3 displayLinear = AgX(hdrColor);

    float3 displaySRGB = LinearToSRGB(displayLinear);

    if(toneMapConstants.debugView == 0 && toneMapConstants.outlineThickness > 0)
    {
        Texture2D<float> selectionMask =
            ResourceDescriptorHeap[toneMapConstants.selectionMaskIndex];

        uint width, height;
        selectionMask.GetDimensions(width, height);

        int2 position = int2(pixel);

        if(selectionMask.Load(int3(position, 0)) > 0.5f)
        {
            bool isOutline = false;
            int radius = int(min(toneMapConstants.outlineThickness, 8u));

            for(int y = -radius; y <= radius && !isOutline; ++y)
            {
                for(int x = -radius; x <= radius; ++x)
                {
                    // Use a circular neighborhood.
                    if(x * x + y * y > radius * radius)
                        continue;

                    int2 neighbor = position + int2(x, y);

                    if(any(neighbor < int2(0, 0)) ||
                    any(neighbor >= int2(width, height)) ||
                    selectionMask.Load(int3(neighbor, 0)) <= 0.5f)
                    {
                        isOutline = true;
                        break;
                    }
                }
            }

            if(isOutline)
                displaySRGB = toneMapConstants.outlineColor;
        }
    }

    return float4(displaySRGB, 1.0f);
}
