#pragma once

#include "Renderer/ShaderData.h"

class Pipeline;
class Texture;

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

    GPUMaterial GetGPUData() const
    {
        GPUMaterial data{};

        data.model = static_cast<unsigned int>(model);
        data.roughness = roughness;
        data.ior = ior;
        data.metallic = metallic;

        data.conductorN[0] = conductorN[0];
        data.conductorN[1] = conductorN[1];
        data.conductorN[2] = conductorN[2];

        data.conductorK[0] = conductorK[0];
        data.conductorK[1] = conductorK[1];
        data.conductorK[2] = conductorK[2];

        return data;
    }
};