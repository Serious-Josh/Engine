#pragma once

#include "BRDF.hlsli"

struct GPUMaterial
{
    uint model;
    float roughness;
    float ior;
    float metallic;

    float3 baseColor;
    uint textureIndex;

    float3 conductorN;
    float padding0;

    float3 conductorK;
    float padding1;

    float3 averageConductorFresnel;
    float padding2;
};

float3 EvaluateMetallicRoughnessDirectBRDF(
    GPUMaterial material,
    float3 baseColor,
    float3 normal,
    float3 directionToLight,
    float3 viewDirection,
    float directionalAlbedoLight,
    float directionalAlbedoView,
    float averageAlbedo)
{
    float metallic =
        saturate(
            material.metallic
        );

    float3 dielectricF0 =
        float3(
            0.04f,
            0.04f,
            0.04f
        );

    float3 F0 =
        lerp(
            dielectricF0,
            baseColor,
            metallic
        );

    float3 diffuseColor =
        baseColor *
        (1.0f - metallic);

    float3 fresnel =
        EvaluateGGXFresnel(
            directionToLight,
            viewDirection,
            F0
        );

    float3 diffuseBRDF =
        EvaluateEONDiffuse(
            diffuseColor,
            material.roughness,
            normal,
            directionToLight,
            viewDirection
        );

    float3 specularBRDF =
        EvaluateGGXSpecular(
            material.roughness,
            normal,
            directionToLight,
            viewDirection,
            F0
        );

    specularBRDF +=
        EvaluateGGXMultipleScatteringSchlick(
            directionalAlbedoLight,
            directionalAlbedoView,
            averageAlbedo,
            F0
        );

    float3 diffuseWeight =
        1.0f - fresnel;

    return
        diffuseWeight *
        diffuseBRDF +
        specularBRDF;
}

float3 EvaluateDielectricDirectBRDF(
    GPUMaterial material,
    float3 baseColor,
    float3 normal,
    float3 directionToLight,
    float3 viewDirection,
    float directionalAlbedoLight,
    float directionalAlbedoView,
    float averageAlbedo)
{
    float iorDifference =
        material.ior - 1.0f;

    float iorSum =
        material.ior + 1.0f;

    float dielectricF0 =
        (iorDifference * iorDifference) /
        (iorSum * iorSum);

    float3 F0 =
        float3(
            dielectricF0,
            dielectricF0,
            dielectricF0
        );

    float3 fresnel =
        EvaluateGGXFresnel(
            directionToLight,
            viewDirection,
            F0
        );

    float3 diffuseBRDF =
        EvaluateEONDiffuse(
            baseColor,
            material.roughness,
            normal,
            directionToLight,
            viewDirection
        );

    float3 specularBRDF =
        EvaluateGGXSpecular(
            material.roughness,
            normal,
            directionToLight,
            viewDirection,
            F0
        );

    specularBRDF +=
        EvaluateGGXMultipleScatteringSchlick(
            directionalAlbedoLight,
            directionalAlbedoView,
            averageAlbedo,
            F0
        );

    float3 diffuseWeight =
        1.0f - fresnel;

    return
        diffuseWeight *
        diffuseBRDF +
        specularBRDF;
}

float3 EvaluateConductorDirectBRDF(
    GPUMaterial material,
    float3 normal,
    float3 directionToLight,
    float3 viewDirection,
    float directionalAlbedoLight,
    float directionalAlbedoView,
    float averageAlbedo)
{
    float3 specularBRDF =
        EvaluateGGXConductor(
            material.roughness,
            normal,
            directionToLight,
            viewDirection,
            material.conductorN,
            material.conductorK
        );

    specularBRDF +=
        EvaluateGGXMultipleScattering(
            directionalAlbedoLight,
            directionalAlbedoView,
            averageAlbedo,
            material.averageConductorFresnel
        );

    return specularBRDF;
}
