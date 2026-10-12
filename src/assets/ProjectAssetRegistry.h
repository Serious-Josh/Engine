#pragma once

#include "assets/AssetRecord.h"

#include <optional>
#include <vector>

class ProjectAssetRegistry
{
    public:
        bool Register(const AssetRecord& record);

        const AssetRecord* Find(const AssetID& id) const noexcept;

        std::vector<AssetRecord> GetAssets() const;
        std::vector<AssetRecord> GetAssets(AssetType type) const;

        bool UpdatePath(const AssetID& id, const std::filesystem::path& newRelativePath);
        bool Unregister(const AssetID& id);

        void Clear() noexcept;

    private:
        std::vector<AssetRecord> m_records;
};