#pragma once

#include "Renderer/ShaderData.h"
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

    static bool Valid(const GPUMaterial& material)
    {
        if(material.model > 2 || material.preferARM > 1 || material.surfaceDetailMode > 2)
        {
            return false;
        }

        const float scalars[] = {material.roughness, material.ior, material.metallic,
            material.heightScale, material.heightBias};

        for(float value : scalars)
        {
            if(!std::isfinite(value))
            {
                return false;
            }
        }

        for(size_t index = 0; index < 3; ++index)
        {
            if(!std::isfinite(material.baseColor[index]) || !std::isfinite(material.conductorN[index]) ||
               !std::isfinite(material.conductorK[index]) || !std::isfinite(material.averageConductorFresnel[index]))
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
        ++revision;
        return true;
    }
};
