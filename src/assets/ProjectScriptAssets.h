#pragma once

#include "Scripting/ScriptHost.h"
#include "assets/AssetID.h"

class ProjectPaths;
class ProjectAssetRegistry;

namespace ProjectScriptAssets
{
// Explicit Editor/project-load scan. Missing records are tombstones, never reassigned.
bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string& error);

// Pins project sources through validation; publishes immutable bytes/revision, never a module to execute.
Scripting::Result Validate(Scripting::ScriptHost& host, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetID& root, Scripting::SourceLimits limits = {});

// Explicit content-based freshness check; no file watcher or timestamp-only cache.
bool IsCurrent(const Scripting::SourceSnapshot& snapshot, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, std::string& error);
}
