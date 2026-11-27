#pragma once

#include "Renderer/Material.h"
#include "Renderer/ShaderData.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// CPU bookkeeping only. Slot completion/acquisition remains CommandContext's responsibility.
template<size_t SlotCount>
struct MaterialTablePublication
{
    std::vector<GPUMaterial> desired;

    std::uint64_t revision = 0;

    std::array<std::uint64_t, SlotCount> published{};

    std::uint64_t lastSubmitted = 0;

    // OpenPBR tiers: whether a desired material has a coat, fuzz, thin film or anisotropy, so ReSTIR PT must run its
    // layered kernels (ReSTIRPTPass::SetLayeredMaterials).
    bool layeredLobes = false;

    // POM B2: whether a desired material renders with parallax occlusion mapping, so ReSTIR PT must run its parallax
    // kernels (the relief's own shadow at the raster primary, ReSTIRPTPass::SetMaterialTier).
    bool parallax = false;

    static bool UsesParallax(const std::vector<GPUMaterial>& materials)
    {
        return std::any_of(materials.begin(), materials.end(),
            [](const GPUMaterial& material) { return SurfaceDetailUsesParallax(material); });
    }

    static bool UsesLayeredLobes(const std::vector<GPUMaterial>& materials)
    {
        constexpr unsigned int layered = GpuMaterialLobeFlags::Coat | GpuMaterialLobeFlags::Fuzz |
            GpuMaterialLobeFlags::ThinFilm | GpuMaterialLobeFlags::Anisotropy;

        for(const GPUMaterial& material : materials)
        {
            if((material.lobeFlags & layered) != 0u)
            {
                return true;
            }
        }

        return false;
    }

    static bool Valid(const GPUMaterial& material)
    {
        constexpr unsigned int knownLobeFlags = GpuMaterialLobeFlags::Coat | GpuMaterialLobeFlags::Fuzz |
            GpuMaterialLobeFlags::ThinFilm | GpuMaterialLobeFlags::Anisotropy |
            GpuMaterialLobeFlags::CoatFollowsBaseNormal | GpuMaterialLobeFlags::PathTracedAO |
            GpuMaterialLobeFlags::ParallaxSilhouette;

        if((material.lobeFlags & ~knownLobeFlags) != 0u || material.preferARM > 1 || material.surfaceDetailMode > 2)
        {
            return false;
        }

        const float scalars[] = {material.roughness, material.ior, material.metallic, material.heightScale,
            material.heightBias, material.baseWeight, material.diffuseRoughness, material.specularWeight,
            material.dielectricF0, material.anisotropy, material.anisotropyRotation, material.coatWeight,
            material.coatRoughness, material.coatIor, material.coatDarkening, material.coatF0,
            material.coatInternalReflection, material.fuzzWeight, material.fuzzRoughness, material.thinFilmWeight,
            material.thinFilmThickness, material.thinFilmIor, material.aoDiffuse, material.aoSpecularWeight,
            material.diffuseOcclusion};

        for(float value : scalars)
        {
            if(!std::isfinite(value))
            {
                return false;
            }
        }

        for(size_t index = 0; index < 3; ++index)
        {
            if(!std::isfinite(material.baseColor[index]) || !std::isfinite(material.specularColor[index]) ||
               !std::isfinite(material.coatColor[index]) || !std::isfinite(material.fuzzColor[index]))
            {
                return false;
            }
        }

        return true;
    }

    bool SetSnapshot(const std::vector<GPUMaterial>& records, bool allowTextureBindings = false)
    {
        if(records.empty() || (!desired.empty() && records.size() != desired.size()) ||
           revision == (std::numeric_limits<std::uint64_t>::max)())
        {
            return false;
        }

        for(size_t index = 0; index < records.size(); ++index)
        {
            if(!Valid(records[index]))
            {
                return false;
            }

            if(!desired.empty() && !allowTextureBindings)
            {
                const auto& old = desired[index];
                const auto& next = records[index];
                // Parameter publication only: binding/resource retirement is a later checkpoint.
                if(old.textureIndex != next.textureIndex || old.normalTextureIndex != next.normalTextureIndex ||
                   old.roughnessTextureIndex != next.roughnessTextureIndex || old.metallicTextureIndex != next.metallicTextureIndex ||
                   old.armTextureIndex != next.armTextureIndex || old.aoTextureIndex != next.aoTextureIndex ||
                   old.heightTextureIndex != next.heightTextureIndex)
                {
                    return false;
                }
            }
        }

        if(!desired.empty() && std::memcmp(records.data(), desired.data(), records.size() * sizeof(GPUMaterial)) == 0)
        {
            return true;
        }

        auto copy = records; // Strong guarantee: allocation failure cannot partially replace the desired table.
        desired.swap(copy);
        layeredLobes = UsesLayeredLobes(desired);
        parallax = UsesParallax(desired);
        ++revision;
        return true;
    }

    void ConfirmSubmission(size_t slot, std::uint64_t recordedRevision)
    {
        if(slot < SlotCount && recordedRevision != 0 && recordedRevision <= revision)
        {
            published[slot] = recordedRevision;
            lastSubmitted = recordedRevision;
        }
    }

    // Admission is append-only; parameter/texture revisions keep their fixed-count API.
    bool Append(const GPUMaterial& record, size_t limit)
    {
        if(desired.empty() || desired.size() >= limit || !Valid(record) || revision == UINT64_MAX) return false;
        auto copy = desired;
        copy.push_back(record);
        desired.swap(copy);
        layeredLobes = UsesLayeredLobes(desired);
        parallax = UsesParallax(desired);
        ++revision;
        return true;
    }
};
