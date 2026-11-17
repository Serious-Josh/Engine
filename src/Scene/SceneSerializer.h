#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/PhysicalLightingMigration.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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

// W1.2/W1.3: the byte-level codec behind the file functions, for scene discovery and migration. ParseSceneDocument reads every
// supported schema exactly as LoadSceneDocument does (historical documents upgraded in memory, ids unchanged);
// SerializeSceneDocument applies ValidateSceneDocumentWrite and produces exactly the bytes SaveSceneDocument writes.
// `storedSchemaVersion`, when given, receives the file's own schemaVersion (0 when absent or not an unsigned integer) even
// when parsing fails, so a caller can tell a future schema from a malformed document without parsing twice.
bool ParseSceneDocument(std::string_view bytes, SceneDocument& document, std::string* errorMessage = nullptr,
    std::uint64_t* storedSchemaVersion = nullptr);
bool SerializeSceneDocument(const SceneDocument& document, std::string& bytes, std::string* errorMessage = nullptr);

// Strict, bounded authoring input: rejects unknown elements instead of dropping them.
bool LoadSceneDocumentForCooking(const std::filesystem::path& path, SceneDocument& document, size_t maxBytes, std::string* errorMessage = nullptr);
