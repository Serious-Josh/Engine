#pragma once

#include "Scene/PhysicalSkySettings.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Physical sky S5 (Physical_Sky_Design_Proposal.md §14.2): cloud layer presets as data, assets/Sky/CloudPresets.json. A preset is a
// named CloudLayerSettings (its kind, altitudes, coverage, density, shape, lighting and wind); the Sky window's Preset choice copies
// it into a layer, keeping the layer's control mode, shadow flag and coverage map. Scenes store the expanded values only.
struct CloudPreset
{
    std::string name;
    CloudLayerSettings layer;
};

namespace CloudPresets
{
    // Parses the presets document. Every preset must validate as a single-layer sky; on failure `error` says why and the result is
    // empty.
    std::vector<CloudPreset> Parse(std::string_view text, std::string* error = nullptr);
    std::vector<CloudPreset> Load(const std::filesystem::path& path, std::string* error = nullptr);

    // The preset's values copied into `layer` (its control mode, shadow flag and coverage map kept; its preset label set).
    void Apply(const CloudPreset& preset, CloudLayerSettings& layer);
}
