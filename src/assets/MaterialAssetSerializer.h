#pragma once

#include "assets/MaterialAssetData.h"

#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

class MaterialAssetSerializer
{
    public:
        static nlohmann::json Serialize(const MaterialAssetData& material);

        // Schema 1 is still read (migrated to OpenPBR's schema 2 on load) but never written, so a schema 1 document cannot
        // be compared with its re-serialization. These are exactly the top-level keys and "textures" keys Deserialize reads
        // from a schema 1 document; Build Game's cooker rejects a schema 1 source with any other key, which the migration
        // would silently drop (CookedSurface::CookDependencies).
        static std::span<const std::string_view> SchemaOneDocumentKeys();
        static std::span<const std::string_view> SchemaOneTextureKeys();

        static bool Deserialize(
            const nlohmann::json& document,
            MaterialAssetData& material,
            std::string* errorMessage = nullptr);

        static bool SaveToFile(
            const MaterialAssetData& material,
            const std::filesystem::path& path,
            std::string* errorMessage = nullptr);

        static bool LoadFromFile(
            const std::filesystem::path& path,
            MaterialAssetData& material,
            std::string* errorMessage = nullptr);
};