#pragma once

#include "Renderer/Material.h"
#include "assets/MaterialAssetData.h"
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace MaterialParameterPropagation
{
    inline void Apply(Material& material, const MaterialAssetData& data) noexcept
    {
        material.model = data.model;
        std::copy(data.baseColor.begin(), data.baseColor.end(), material.baseColor);
        std::copy(data.conductorN.begin(), data.conductorN.end(), material.conductorN);
        std::copy(data.conductorK.begin(), data.conductorK.end(), material.conductorK);
        material.roughness = data.roughness;
        material.metallic = data.metallic;
        material.ior = data.ior;
        material.surfaceDetailMode = data.surfaceDetailMode;

        material.heightScale = data.heightScale;
        material.heightBias = data.heightBias;
        material.preferARM = data.preferARM;
    }

    inline bool SameTextures(const MaterialAssetData& left, const MaterialAssetData& right)
    {
        return left.baseColorTexture == right.baseColorTexture && left.normalTexture == right.normalTexture &&
            left.roughnessTexture == right.roughnessTexture && left.metallicTexture == right.metallicTexture &&
            left.armTexture == right.armTexture && left.aoTexture == right.aoTexture && left.heightTexture == right.heightTexture;
    }

    // Called synchronously inside the checked catalog operation. Targets cannot escape this call.
    // Publisher must have a strong rejection guarantee; once it accepts, CPU copies cannot fail.
    inline bool Publish(const std::vector<Material*>& targets, const MaterialAssetData& data,
        std::vector<GPUMaterial> table, const std::function<bool(const std::vector<GPUMaterial>&)>& publisher,
        std::string* error)
    {
        std::vector<Material> replacements;
        replacements.reserve(targets.size());
        std::vector<bool> visited(table.size(), false);

        for(auto* target : targets)
        {
            if(!target || target->gpuIndex >= table.size() || visited[target->gpuIndex])
            {
                if(error)
                {
                    *error = "Invalid or duplicate runtime material index.";
                }

                return false;
            }

            visited[target->gpuIndex] = true;
            Material candidate = *target;
            Apply(candidate, data);
            replacements.push_back(candidate);
            auto& gpu = table[target->gpuIndex];
            // Preserve all descriptor fields and the existing GPU layout.
            gpu.model = static_cast<unsigned int>(candidate.model);
            gpu.roughness = candidate.roughness;
            gpu.metallic = candidate.metallic;
            gpu.ior = candidate.ior;

            gpu.surfaceDetailMode = static_cast<unsigned int>(candidate.surfaceDetailMode);
            gpu.heightScale = candidate.heightScale;
            gpu.heightBias = candidate.heightBias;
            gpu.preferARM = candidate.preferARM ? 1u : 0u;

            for(size_t component = 0; component < 3; ++component)
            {
                gpu.baseColor[component] = candidate.baseColor[component];
                gpu.conductorN[component] = candidate.conductorN[component];
                gpu.conductorK[component] = candidate.conductorK[component];
                gpu.averageConductorFresnel[component] = Material::CalculateAverageConductorFresnel(
                    candidate.conductorN[component], candidate.conductorK[component]);
            }
        }

        if(targets.empty() || !publisher(table))
        {
            return false;
        }

        static_assert(std::is_nothrow_copy_assignable_v<Material>);

        for(size_t index = 0; index < targets.size(); ++index)
        {
            *targets[index] = replacements[index];
        }

        return true;
    }
}
