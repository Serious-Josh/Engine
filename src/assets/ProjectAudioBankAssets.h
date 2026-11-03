#pragma once

#include <cstddef>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;

// Audio.1B: FMOD Studio banks are discovered, never imported. FMOD Studio builds them into the project's
// Content/Audio/Banks directory (any subfolder, typically Desktop/); each .bank file there becomes an AudioBank asset
// in the project's one asset registry.
namespace ProjectAudioBankAssets
{
struct ReconcileReport
{
    std::size_t registered = 0;
    std::size_t available = 0;
    // Registered banks whose file is gone. Their identity is kept (an FMOD Studio rebuild restores the file), never
    // reassigned and never removed by discovery.
    std::size_t missing = 0;
};

// Explicit scan (project load). Registers new bank files, keeps missing ones, and publishes the registry only when it
// changed. A project without the bank directory has no banks and is not an error. On failure the registry is unchanged.
bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, ReconcileReport& report, std::string& error);
}
