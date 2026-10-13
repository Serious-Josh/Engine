#pragma once

#include "assets/AssetID.h"
#include "assets/MaterialAssetData.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

// Owned save receipt. Exact serialized bytes are the source fingerprint; sequence
// orders receipts from this Inspector session, not revisions across Engine restarts.
struct MaterialSavedRevision
{
    std::filesystem::path projectIdentity;

    AssetID assetID;

    std::filesystem::path relativePath;

    std::string savedBytes;

    std::uint64_t sequence = 0;
};

inline void CoalesceMaterialSavedRevision(std::vector<MaterialSavedRevision>& revisions, MaterialSavedRevision revision)
{
    for(auto& current : revisions)
    {
        if(current.projectIdentity == revision.projectIdentity && current.assetID == revision.assetID)
        {
            if(revision.sequence > current.sequence)
            {
                current = std::move(revision);
            }

            return;
        }
    }

    revisions.push_back(std::move(revision));
}

// Independent of preview orientation, texture refresh, and disk-save sequence.
struct MaterialWorkingRevision
{
    enum class Action { Draft, Restore, Saved, Leave, Invalid };
    std::filesystem::path projectIdentity;

    AssetID assetID;

    std::filesystem::path relativePath;

    std::uint64_t sequence = 0;
    std::uint64_t saveSequence = 0;

    Action action = Action::Invalid;

    std::optional<MaterialAssetData> parameters;
};
