#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/PhysicalLightingMigration.h"

#include <filesystem>
#include <string>

bool SaveSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage = nullptr);

bool LoadSceneDocument(
    const std::filesystem::path& path,
    SceneDocument& document,
    std::string* errorMessage = nullptr);

// As LoadSceneDocument, choosing how a document in legacy light units (schema < 14) is converted.
bool LoadSceneDocument(
    const std::filesystem::path& path,
    SceneDocument& document,
    std::string* errorMessage,
    const LegacyLightingMigrationOptions& migration);
// Collision is distinct from validation, write, and other filesystem errors.
enum class CreateSceneDocumentResult
{
    Created,
    Collision,
    Error
};

CreateSceneDocumentResult CreateSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage = nullptr);

// Strict, bounded authoring input: rejects unknown elements instead of dropping them.
bool LoadSceneDocumentForCooking(const std::filesystem::path& path, SceneDocument& document, size_t maxBytes, std::string* errorMessage = nullptr);
