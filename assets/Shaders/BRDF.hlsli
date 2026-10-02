#ifndef BRDF_HLSLI
#define BRDF_HLSLI

#include "Common.hlsli"

static const float INV_PI = 1.0f / PI;

static const float EON_CONSTANT_1 = 0.5f - 2.0f / (3.0f * PI);
static const float EON_CONSTANT_2 = 2.0f / 3.0f - 28.0f / (15.0f * PI);

float EvaluateFONDirectionalAlbedo(float mu, float roughness)
{
    const float epsilon = 1.0e-6f;

    mu = clamp(mu, epsilon, 1.0f);

    float A = 1.0f / (1.0f + EON_CONSTANT_1 * roughness);
    float B = roughness * A;

    float sinTheta = sqrt(max(0.0f, 1.0f - mu * mu));

    float G =
        sinTheta * (acos(mu) - sinTheta * mu) +
        (2.0f / 3.0f) *
        ((sinTheta / mu) * (1.0f - sinTheta * sinTheta * sinTheta) - sinTheta);

    return A + (B * INV_PI) * G;
}

float3 EvaluateEONDiffuse(
    float3 albedo,
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    roughness = saturate(roughness);

    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection));

    if(NdotL <= 0.0f || NdotV <= 0.0f){
        return float3(0.0f, 0.0f, 0.0f);
    }

    float s = dot(lightDirection, viewDirection) - NdotL * NdotV;

    float sOverT;

    if(s > 0.0f){
        sOverT = s / max(NdotL, NdotV);
    }
    else{
        sOverT = s;
    }

    float A = 1.0f / (1.0f + EON_CONSTANT_1 * roughness);

    float3 singleScatter =
        albedo * INV_PI *
        A *
        (1.0f + roughness * sOverT);

    float lightAlbedo =
        EvaluateFONDirectionalAlbedo(NdotL, roughness);

    float viewAlbedo =
        EvaluateFONDirectionalAlbedo(NdotV, roughness);

    float averageAlbedo =
        A * (1.0f + EON_CONSTANT_2 * roughness);

    float3 multipleScatterAlbedo =
        (albedo * albedo) *
        averageAlbedo /
        max(
            float3(1.0e-6f, 1.0e-6f, 1.0e-6f),
            1.0f - albedo * (1.0f - averageAlbedo)
        );

    float3 multipleScatter =
        multipleScatterAlbedo *
        INV_PI *
        max(1.0e-6f, 1.0f - viewAlbedo) *
        max(1.0e-6f, 1.0f - lightAlbedo) /
        max(1.0e-6f, 1.0f - averageAlbedo);

    return singleScatter + multipleScatter;
}

float DistributionGGX(
    float3 normal,
    float3 halfVector,
    float roughness)
{
    float perceptualRoughness = max(roughness, 0.045f);
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSquared = alpha * alpha;

    float NdotH = saturate(dot(normal, halfVector));
    float NdotHSquared = NdotH * NdotH;

    float denominator =
        NdotHSquared * (alphaSquared - 1.0f) + 1.0f;

    denominator =
        PI * denominator * denominator;

    return alphaSquared / max(denominator, 1.0e-6f);
}

float GeometrySmithGGX(
    float NdotL,
    float NdotV,
    float roughness)
{
    float perceptualRoughness = max(roughness, 0.045f);
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSquared = alpha * alpha;

    float lambdaL =
        NdotV *
        sqrt(max(
            0.0f,
            NdotL * NdotL * (1.0f - alphaSquared) +
            alphaSquared
        ));

    float lambdaV =
        NdotL *
        sqrt(max(
            0.0f,
            NdotV * NdotV * (1.0f - alphaSquared) +
            alphaSquared
        ));

    return
        (2.0f * NdotL * NdotV) /
        max(lambdaL + lambdaV, 1.0e-6f);
}

float3 FresnelSchlick(
    float VdotH,
    float3 F0)
{
    float factor = pow(1.0f - saturate(VdotH), 5.0f);

    return F0 + (1.0f - F0) * factor;
}

float3 FresnelConductor(
    float cosTheta,
    float3 n,
    float3 k)
{
    cosTheta = saturate(cosTheta);

    float cosThetaSquared = cosTheta * cosTheta;
    float sinThetaSquared = 1.0f - cosThetaSquared;

    float3 nSquared = n * n;
    float3 kSquared = k * k;

    float3 t0 =
        nSquared -
        kSquared -
        sinThetaSquared;

    float3 a2PlusB2 =
        sqrt(
            t0 * t0 +
            4.0f * nSquared * kSquared
        );

    float3 t1 =
        a2PlusB2 +
        cosThetaSquared;

    float3 a =
        sqrt(
            0.5f *
            (a2PlusB2 + t0)
        );

    float3 t2 =
        2.0f *
        cosTheta *
        a;

    float3 Rs =
        (t1 - t2) /
        (t1 + t2);

    float3 t3 =
        cosThetaSquared *
        a2PlusB2 +
        sinThetaSquared *
        sinThetaSquared;

    float3 t4 =
        t2 *
        sinThetaSquared;

    float3 Rp =
        Rs *
        (t3 - t4) /
        (t3 + t4);

    return 0.5f * (Rp + Rs);
}

float3 EvaluateGGXFresnel(
    float3 lightDirection,
    float3 viewDirection,
    float3 F0)
{
    float3 halfVector = normalize(lightDirection + viewDirection);
    float VdotH = saturate(dot(viewDirection, halfVector));

    return FresnelSchlick(VdotH, F0);
}

float3 EvaluateGGXSpecular(
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection,
    float3 F0)
{
    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection));

    if(NdotL <= 0.0f || NdotV <= 0.0f){
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 halfVector = normalize(lightDirection + viewDirection);

    float VdotH = saturate(dot(viewDirection, halfVector));

    float D = DistributionGGX(
        normal,
        halfVector,
        roughness
    );

    float G = GeometrySmithGGX(
        NdotL,
        NdotV,
        roughness
    );

    float3 F = FresnelSchlick(
        VdotH,
        F0
    );

    float denominator =
        4.0f * NdotL * NdotV;

    return
        D * G * F /
        max(denominator, 1.0e-6f);
}

float3 EvaluateGGXConductor(
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection,
    float3 n,
    float3 k)
{
    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection));

    if(NdotL <= 0.0f || NdotV <= 0.0f){
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 halfVector =
        normalize(lightDirection + viewDirection);

    float VdotH =
        saturate(dot(viewDirection, halfVector));

    float D = DistributionGGX(
        normal,
        halfVector,
        roughness
    );

    float G = GeometrySmithGGX(
        NdotL,
        NdotV,
        roughness
    );

    float3 F = FresnelConductor(
        VdotH,
        n,
        k
    );

    float denominator =
        4.0f * NdotL * NdotV;

    return
        D * G * F /
        max(denominator, 1.0e-6f);
}

#endif