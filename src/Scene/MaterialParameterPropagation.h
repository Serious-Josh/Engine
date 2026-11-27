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
        std::copy(data.baseColor.begin(), data.baseColor.end(), material.baseColor);
        material.baseWeight = data.baseWeight;
        material.metallic = data.metallic;
        material.diffuseRoughness = data.diffuseRoughness;

        material.roughness = data.roughness;
        material.ior = data.ior;
        material.specularWeight = data.specularWeight;
        std::copy(data.specularColor.begin(), data.specularColor.end(), material.specularColor);
        material.anisotropy = data.anisotropy;
        // The asset stores degrees; the shaders rotate the tangent by radians.
        material.anisotropyRotation = data.anisotropyRotationDegrees * (3.14159265358979323846f / 180.0f);

        material.coatWeight = data.coatWeight;
        std::copy(data.coatColor.begin(), data.coatColor.end(), material.coatColor);
        material.coatRoughness = data.coatRoughness;
        material.coatIor = data.coatIor;
        material.coatDarkening = data.coatDarkening;
        material.coatFollowsBaseNormal = data.coatFollowsBaseNormal;

        material.fuzzWeight = data.fuzzWeight;
        std::copy(data.fuzzColor.begin(), data.fuzzColor.end(), material.fuzzColor);
        material.fuzzRoughness = data.fuzzRoughness;

        material.thinFilmWeight = data.thinFilmWeight;
        material.thinFilmThickness = data.thinFilmThickness;
        material.thinFilmIor = data.thinFilmIor;

        material.aoDiffuse = data.aoDiffuse;
        material.aoSpecularWeight = data.aoSpecularWeight;

        material.surfaceDetailMode = data.surfaceDetailMode;

        material.heightScale = data.heightScale;
        material.heightBias = data.heightBias;
        material.parallaxSilhouette = data.parallaxSilhouette;
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
            // Preserve all descriptor fields and the existing GPU layout: only the parameter fields (and their
            // derived terms) change.
            candidate.WriteParameters(table[target->gpuIndex]);
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
