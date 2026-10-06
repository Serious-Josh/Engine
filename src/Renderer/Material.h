#pragma once

#include "Renderer/ShaderData.h"
#include "Renderer/Texture.h"

#include <climits>
#include <algorithm>
#include <cmath>

class Pipeline;

enum class MaterialModel
{
    MetallicRoughness,
    Dielectric,
    Conductor
};

struct Material
{
    Pipeline* pipeline = nullptr;
    Texture* texture = nullptr;

    MaterialModel model = MaterialModel::Dielectric;

    float baseColor[3] = {1.0f, 1.0f, 1.0f};

    float roughness = 0.5f;
    float ior = 1.536f;

    float metallic = 0.0f;

    float conductorN[3] = {1.0f, 1.0f, 1.0f};
    float conductorK[3] = {0.0f, 0.0f, 0.0f};

    unsigned int gpuIndex = 0;

    static float EvaluateConductorFresnel(
        float cosTheta,
        float n,
        float k)
    {
        cosTheta =
            std::clamp(
                cosTheta,
                0.0f,
                1.0f
            );

        float cosThetaSquared =
            cosTheta * cosTheta;

        float sinThetaSquared =
            1.0f - cosThetaSquared;

        float nSquared =
            n * n;

        float kSquared =
            k * k;

        float t0 =
            nSquared -
            kSquared -
            sinThetaSquared;

        float a2PlusB2 =
            std::sqrt(
                t0 * t0 +
                4.0f * nSquared * kSquared
            );

        float t1 =
            a2PlusB2 +
            cosThetaSquared;

        float a =
            std::sqrt(
                0.5f *
                (a2PlusB2 + t0)
            );

        float t2 =
            2.0f *
            cosTheta *
            a;

        float Rs =
            (t1 - t2) /
            (t1 + t2);

        float t3 =
            cosThetaSquared *
            a2PlusB2 +
            sinThetaSquared *
            sinThetaSquared;

        float t4 =
            t2 *
            sinThetaSquared;

        float Rp =
            Rs *
            (t3 - t4) /
            (t3 + t4);

        return
            0.5f *
            (Rp + Rs);
    }

    static float CalculateAverageConductorFresnel(
        float n,
        float k)
    {
        constexpr unsigned int sampleCount = 256;

        float integral = 0.0f;

        for(unsigned int sample = 0; sample < sampleCount; ++sample)
        {
            // Midpoint integration over mu = cos(theta).
            float mu =
                (static_cast<float>(sample) + 0.5f) /
                static_cast<float>(sampleCount);

            float fresnel =
                EvaluateConductorFresnel(
                    mu,
                    n,
                    k
                );

            integral +=
                fresnel *
                mu;
        }

        return
            2.0f *
            integral /
            static_cast<float>(sampleCount);
    }

    GPUMaterial GetGPUData() const
    {
        GPUMaterial data{};

        data.model = static_cast<unsigned int>(model);
        data.roughness = roughness;
        data.ior = ior;
        data.metallic = metallic;

        data.baseColor[0] = baseColor[0];
        data.baseColor[1] = baseColor[1];
        data.baseColor[2] = baseColor[2];

        data.textureIndex = texture ? texture->GetDescriptorIndex() : UINT_MAX;

        data.conductorN[0] = conductorN[0];
        data.conductorN[1] = conductorN[1];
        data.conductorN[2] = conductorN[2];

        data.conductorK[0] = conductorK[0];
        data.conductorK[1] = conductorK[1];
        data.conductorK[2] = conductorK[2];

        data.averageConductorFresnel[0] =
        CalculateAverageConductorFresnel(
                conductorN[0],
                conductorK[0]
            );

        data.averageConductorFresnel[1] =
            CalculateAverageConductorFresnel(
                conductorN[1],
                conductorK[1]
            );

        data.averageConductorFresnel[2] =
            CalculateAverageConductorFresnel(
                conductorN[2],
                conductorK[2]
            );

        return data;
    }
};