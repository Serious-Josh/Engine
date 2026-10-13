#pragma once

#include <string>
#include <vector>
#include "assets/AssetReconciliationTransaction.h"

class ProjectPaths;
class ProjectAssetRegistry;

namespace ProjectAssetReconciliation
{
    struct Report
    {
        size_t missingDetected = 0;
        size_t registrationsRemoved = 0;
        size_t materialReferencesCleared = 0;
        size_t sceneReferencesCleared = 0;

        std::vector<std::string> filesModified;
        std::vector<std::string> skipped;
    };

    // Project-load only: recover first, then load/validate the registry, then reconcile.
    bool Run(
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry,
        Report& report,
        std::string& error);
}
