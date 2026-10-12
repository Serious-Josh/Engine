#pragma once

#include "Scene/SceneDocument.h"

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
