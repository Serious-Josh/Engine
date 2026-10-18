#pragma once

#include "assets/AssetID.h"
#include "assets/MeshImportUnits.h"

#include <cstdint>
#include <filesystem>

enum class AssetType : std::uint8_t
{
    Material,
    Mesh,
    Texture
};

struct AssetRecord
{
    AssetID id;
    AssetType type;
    std::filesystem::path relativePath;
    MeshImportUnits meshImportUnits;
};

bool ValidateAssetRecord(const AssetRecord& record);
