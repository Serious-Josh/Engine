#pragma once

#include "assets/AssetRecord.h"
#include "assets/ProjectAssetRegistry.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

class ProjectPaths;

// M7.1: finds probe assets (M8.2: and baked reflection assets) that nothing references, and removes them only on request. See SteamAudioIntegration.md, section 33.
//
// A probe asset is unused only when no .scene file anywhere under the project and no live reference holds it. Live references are the
// open document and the Editor's undo and redo history, so neither unsaved work nor a saved scene alone can decide it. One scene that
// can't be read makes the whole scan inconclusive, because what that scene references is unknown.
namespace ProbeAssetCleanup
{
struct Report
{
    // False when something could not be read; then `unused` is empty and `reason` says what.
    bool conclusive = false;
    std::string reason;
    std::vector<AssetID> unused;
    std::uint64_t unusedBytes = 0;
    std::size_t referenced = 0;
    std::size_t scenesRead = 0;
    // .probebatch and .bakedreflections files with no registration. Reported, never deleted: a registry restored from elsewhere may claim them.
    std::vector<std::filesystem::path> unregisteredFiles;
};

Report Scan(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::set<std::string>& liveReferences);

// Scans again and removes only what is both in `confirmed` and still unused. Returns how many went; `error` holds any failure.
std::size_t RemoveConfirmed(const ProjectPaths& paths, ProjectAssetRegistry& registry, const std::set<std::string>& liveReferences,
    const std::vector<AssetID>& confirmed, std::string& error);
}
