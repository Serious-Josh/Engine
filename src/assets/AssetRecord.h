#pragma once

#include "assets/AssetID.h"
#include "assets/MeshImportUnits.h"

#include <cstdint>
#include <filesystem>

enum class AssetType : std::uint8_t
{
    Material,
    Mesh,
    Texture,
    Script,
    // Audio.1B: an FMOD Studio bank built by FMOD Studio into Content/Audio/Banks (never imported or converted).
    AudioBank,
    // M4: a reusable acoustic material, in AcousticMaterials/.
    AcousticMaterial,
    // M7: generated probes of one Probe Volume, in Content/Audio/Probes/.
    ProbeBatch
};

struct AssetRecord
{
    AssetID id;
    AssetType type;
    std::filesystem::path relativePath;
    MeshImportUnits meshImportUnits;
};

bool ValidateAssetRecord(const AssetRecord& record);
