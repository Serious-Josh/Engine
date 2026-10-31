#include "NRD/NRD.hlsli"

struct ToneMapConstants
{
    uint sceneColorIndex;
    // Raw camera exposure state (AutoExposure.hlsl): word 1 is the multiplier 2^(EV_ref - EV_eff) from GPU values to
    // tone-mapper input, the same value DLSS receives as its exposure texture.
    uint exposureStateIndex;
    uint debugTextureIndex;
    uint debugView;
    float nrdDenoisingRange;

    uint selectionMaskIndex;
    uint outlineThickness;
    uint debugDepthIndex;
    float3 outlineColor;
    uint toneMapOperator; // ToneMapOperator: 0 Existing (AgX), 1 GT7.
};

ConstantBuffer<ToneMapConstants> toneMapConstants : register(b0);

// HLSL fills float3x3 by rows and the shader applies mul(M, color), so each literal row below is one output channel.
// Every row sums to 1 (neutral colours stay neutral). Published AgX sources list these coefficients column-major;
// they are transposed here (FOUNDATION.2).
static const float3x3 AgXInsetMatrix = float3x3(
    0.856627153315983, 0.095121240538159, 0.048251606145858,
    0.137318972929847, 0.761241990602591, 0.101439036467562,
    0.111898212999950, 0.076799418603190, 0.811302368396859
);

static const float3x3 AgXOutsetMatrix = float3x3(
     1.127100581814437, -0.110606643096603, -0.016493938717835,
    -0.141329763498438,  1.157823702216272, -0.016493938717835,
    -0.141329763498438, -0.110606643096603,  1.251936406595041
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

// -------------------------------------------------------------------------------------------------
// GT7 tone mapping: an HLSL port of Polyphony Digital's reference sample, gt7_tone_mapping.cpp v1.0 (2025-08-10,
// MIT License, Copyright (c) 2025 Polyphony Digital Inc.), from "Driving Toward Reality: Physically Based Tone Mapping
// and Perceptual Fidelity in Gran Turismo 7" (SIGGRAPH 2025 Physically Based Shading course). The sample's SDR mode
// with its default ICtCp UCS: GT Tone Mapping curve V2 (alpha 0.25, mid point 0.538, linear section 0.444, toe 1.28)
// at the 250 cd/m^2 SDR paper white, chroma fade 0.98..1.16, blend ratio 0.6. Frame-buffer 1.0 is 100 cd/m^2, as in
// the sample; Engine's linear values enter at that scale unchanged.
// -------------------------------------------------------------------------------------------------
static const float GT7ReferenceLuminance = 100.0f;  // cd/m^2 <-> frame-buffer 1.0
static const float GT7SdrPaperWhite = 250.0f;       // cd/m^2
static const float GT7Peak = GT7SdrPaperWhite / GT7ReferenceLuminance;
static const float GT7Alpha = 0.25f;
static const float GT7MidPoint = 0.538f;
static const float GT7LinearSection = 0.444f;
static const float GT7ToeStrength = 1.280f;
static const float GT7BlendRatio = 0.6f;
static const float GT7FadeStart = 0.98f;
static const float GT7FadeEnd = 1.16f;

// Engine's linear colour is Rec.709/sRGB primaries; the GT7 operator works in linear Rec.2020 (ITU-R BT.2087).
static const float3x3 Rec709ToRec2020 = float3x3(
    0.6274038959, 0.3292830384, 0.0433130657,
    0.0690972894, 0.9195403951, 0.0113623156,
    0.0163914389, 0.0880133079, 0.8955952532
);

static const float3x3 Rec2020ToRec709 = float3x3(
     1.6604910021, -0.5876411388, -0.0728498633,
    -0.1245504745,  1.1328998971, -0.0083494226,
    -0.0181507634, -0.1005788980,  1.1187296614
);

// The sample's smoothStep(x, edge0, edge1).
float GT7SmoothStep(float x, float edge0, float edge1)
{
    float t = (x - edge0) / (edge1 - edge0);

    if(x < edge0)
    {
        return 0.0f;
    }

    if(x > edge1)
    {
        return 1.0f;
    }

    return t * t * (3.0f - 2.0f * t);
}

// GTToneMappingCurveV2::evaluateCurve with the SDR initialisation (peak intensity = paper white).
float GT7Curve(float x)
{
    const float k = (GT7LinearSection - 1.0f) / (GT7Alpha - 1.0f);
    const float kA = GT7Peak * GT7LinearSection + GT7Peak * k;
    const float kB = -GT7Peak * k * exp(GT7LinearSection / k);
    const float kC = -1.0f / (k * GT7Peak);

    if(x < 0.0f)
    {
        return 0.0f;
    }

    float weightLinear = GT7SmoothStep(x, 0.0f, GT7MidPoint);
    float weightToe = 1.0f - weightLinear;
    float shoulder = kA + kB * exp(x * kC);

    if(x < GT7LinearSection * GT7Peak)
    {
        float toeMapped = GT7MidPoint * pow(x / GT7MidPoint, GT7ToeStrength);
        return weightToe * toeMapped + weightLinear * x;
    }

    return shoulder;
}

// SMPTE ST 2084 (PQ) EOTF: normalised signal to frame-buffer linear.
float3 GT7EotfSt2084(float3 n)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float pqC = 10000.0f;

    n = saturate(n);
    float3 np = pow(n, 1.0f / m2);
    float3 l = max(np - c1, 0.0f);
    l = l / (c2 - c3 * np);
    l = pow(l, 1.0f / m1);
    return l * pqC / GT7ReferenceLuminance;
}

// Inverse PQ EOTF: frame-buffer linear to normalised signal.
float3 GT7InverseEotfSt2084(float3 v)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float pqC = 10000.0f;

    float3 y = v * GT7ReferenceLuminance / pqC;
    float3 ym = pow(y, m1);
    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0f + c3 * ym)));
}

// ICtCp (ITU-T T.302) from linear Rec.2020, as in the sample.
float3 GT7RgbToICtCp(float3 rgb)
{
    float3 lms = float3(
        dot(rgb, float3(1688.0f, 2146.0f, 262.0f)),
        dot(rgb, float3(683.0f, 2951.0f, 462.0f)),
        dot(rgb, float3(99.0f, 309.0f, 3688.0f))) / 4096.0f;
    float3 pq = GT7InverseEotfSt2084(lms);

    return float3(
        2048.0f * pq.x + 2048.0f * pq.y,
        6610.0f * pq.x - 13613.0f * pq.y + 7003.0f * pq.z,
        17933.0f * pq.x - 17390.0f * pq.y - 543.0f * pq.z) / 4096.0f;
}

float3 GT7ICtCpToRgb(float3 ictCp)
{
    float3 pq = float3(
        ictCp.x + 0.00860904f * ictCp.y + 0.11103f * ictCp.z,
        ictCp.x - 0.00860904f * ictCp.y - 0.11103f * ictCp.z,
        ictCp.x + 0.560031f * ictCp.y - 0.320627f * ictCp.z);
    float3 lms = GT7EotfSt2084(pq);

    return max(float3(
        3.43661f * lms.x - 2.50645f * lms.y + 0.0698454f * lms.z,
        -0.79133f * lms.x + 1.9836f * lms.y - 0.192271f * lms.z,
        -0.0259499f * lms.x - 0.0989137f * lms.y + 1.12486f * lms.z), 0.0f);
}

// GT7ToneMapping::applyToneMapping in SDR mode. Input: linear Rec.709 scene colour (frame-buffer scale). Output: linear
// Rec.709 display colour in [0, 1], ready for the sRGB encoding.
float3 GT7(float3 color)
{
    // Negative, NaN and infinite input (max/min return the non-NaN operand) become finite before the PQ transforms;
    // 65504 lies far beyond the curve's convergence, so it does not change finite results.
    float3 rgb = min(max(mul(Rec709ToRec2020, max(color, 0.0f)), 0.0f), 65504.0f);

    float3 ucs = GT7RgbToICtCp(rgb);
    float3 skewedRgb = float3(GT7Curve(rgb.r), GT7Curve(rgb.g), GT7Curve(rgb.b));
    float3 skewedUcs = GT7RgbToICtCp(skewedRgb);

    const float targetUcs = GT7RgbToICtCp(GT7Peak.xxx).x;
    float chromaScale = 1.0f - GT7SmoothStep(ucs.x / targetUcs, GT7FadeStart, GT7FadeEnd);
    float3 scaledRgb = GT7ICtCpToRgb(float3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale));

    float3 blended = (1.0f - GT7BlendRatio) * skewedRgb + GT7BlendRatio * scaledRgb;
    const float sdrCorrectionFactor = GT7ReferenceLuminance / GT7SdrPaperWhite;
    float3 display2020 = sdrCorrectionFactor * min(blended, GT7Peak);

    // Back to the Rec.709 display primaries; out-of-gamut Rec.2020 colours are clipped to the sRGB cube.
    return saturate(mul(Rec2020ToRec709, display2020));
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
        {
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        }

        if(depth >= 1.0f)
        {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }

        Texture2D<float4> gBufferTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float3 value = gBufferTexture.Load(int3(pixel, 0)).rgb;

        if(!all(isfinite(value)))
        {
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        }

        if(toneMapConstants.debugView == 17)
        {
            // The surface target stores signed world-space XYZ.
            float lengthSquared = dot(value, value);

            if(!isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
            {
                return float4(1.0f, 0.0f, 1.0f, 1.0f);
            }

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
        {
            return float4(0.0f, 0.0f, 1.0f, 1.0f);
        }

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
        {
            return float4(1, 0, 0, 1); // Invalid depth.
        }

        if(z > toneMapConstants.nrdDenoisingRange)
        {
            return float4(1, 0, 1, 1);
        }

        float gray = z / (z + 10.0f);
        return float4(gray.xxx, 1);
    }
    if(toneMapConstants.debugView >= 3 && toneMapConstants.debugView <= 7)
    {
        Texture2D<float4> preparedTexture = ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float4 prepared = preparedTexture.Load(int3(pixel, 0));

        if(!all(isfinite(prepared)))
        {
            return float4(1, 0, 1, 1);
        }

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
        {
            return float4(saturate(decoded.www), 1);
        }

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

    if(toneMapConstants.debugView == 11 || toneMapConstants.debugView == 13 ||
       toneMapConstants.debugView == 31)
    {
        Texture2D<float4> rawGI =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        uint sourceWidth, sourceHeight;
        rawGI.GetDimensions(sourceWidth, sourceHeight);

        if(sourceWidth == 0u || sourceHeight == 0u)
        {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }

        // Raw and signed reconstructed GI represent full-resolution pixels (2*x, 2*y).
        // Denoised GI has already been reconstructed to full resolution.
        uint2 sourcePixel = toneMapConstants.debugView == 13 ? pixel : pixel / 2u;
        sourcePixel = min(sourcePixel, uint2(sourceWidth, sourceHeight) - 1u);
        float4 giValue = rawGI.Load(int3(sourcePixel, 0));
        // Primary visibility is already normalized; bypass HDR mapping and color conversion.

        if(toneMapConstants.debugView == 11 && giValue.a == -2.0f)
        {
            return float4(all(isfinite(giValue.rgb)) ? giValue.rgb : float3(1, 0, 1), 1.0f);
        }

        // Negative alpha identifies pre-encoded RTGI diagnostics, excluded from NRD.

        if(toneMapConstants.debugView == 11 && giValue.a < 0.0f)
        {
            return float4(all(isfinite(giValue.rgb)) ? LinearToSRGB(giValue.rgb) : float3(1, 0, 1), 1.0f);
        }

        float3 irradiance = giValue.rgb;

        if(!all(isfinite(irradiance)))
        {
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        }

        // Fixed diagnostic scale: 1 irradiance unit maps to 0.5.
        // All three views bypass exposure and AgX, then encode identically for display.
        float3 color = max(irradiance, 0.0f);
        float3 displayLinear = color / (1.0f + color);
        return float4(LinearToSRGB(displayLinear), 1.0f);
    }

    if(toneMapConstants.debugView == 12)
    {
        Texture2D<float> giHitDistance =
            ResourceDescriptorHeap[toneMapConstants.debugTextureIndex];
        float distance = giHitDistance.Load(int3(pixel, 0));

        if(distance == -1.0f)
        {
            return float4(0.0f, 0.0f, 1.0f, 1.0f);
        }

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

    if(toneMapConstants.debugView == 24u || toneMapConstants.debugView == 25u ||
       toneMapConstants.debugView == 27u || toneMapConstants.debugView == 32u || toneMapConstants.debugView == 33u)
    {
        return float4(all(isfinite(hdrColor)) ? hdrColor : float3(1.0f, 0.0f, 1.0f), 1.0f);
    }

    // Camera exposure applies to the normal image only; diagnostic lighting views stay at the reference exposure.
    if(toneMapConstants.debugView == 0u)
    {
        ByteAddressBuffer exposureState = ResourceDescriptorHeap[toneMapConstants.exposureStateIndex];
        hdrColor *= asfloat(exposureState.Load(4u));
    }

    if(((toneMapConstants.debugView >= 19u && toneMapConstants.debugView <= 23u) || toneMapConstants.debugView == 26u ||
        (toneMapConstants.debugView >= 28u && toneMapConstants.debugView <= 30u)) && !all(isfinite(hdrColor)))
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    float3 displayLinear;

    if(toneMapConstants.toneMapOperator == 1u)
    {
        displayLinear = GT7(hdrColor);
    }
    else
    {
        displayLinear = AgX(hdrColor);
    }

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
                    {
                        continue;
                    }

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
            {
                displaySRGB = toneMapConstants.outlineColor;
            }
        }
    }

    return float4(displaySRGB, 1.0f);
}
