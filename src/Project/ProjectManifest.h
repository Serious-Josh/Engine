#pragma once

#include "assets/AssetID.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Project-relative content directories declared by a manifest. Format version 1 supports only
// the standard layout that ProjectPaths implements; other values are rejected, not remapped.
struct ProjectContentPaths
{
    std::filesystem::path scenes = "Scenes";
    std::filesystem::path materials = "Materials";
    std::filesystem::path meshes = "Meshes";
    std::filesystem::path textures = "Textures";
};

// In-memory form of a .engineproj file.
//
// The manifest never stores an absolute path: the project root is always the directory that
// contains the manifest, so relocating the directory keeps every path valid. projectId is the
// stable identity and is independent of where the project currently lives.
//
// Unknown top-level and nested object members are retained in m_document so that a future
// rewrite does not discard data written by a newer Engine. Opening a project never writes.
class ProjectManifest
{
public:
    static constexpr std::string_view kFormatName = "EngineProject";
    static constexpr int kCurrentFormatVersion = 1;
    static constexpr std::string_view kFileExtension = ".engineproj";

    // Strict parse. Returns nullopt and sets errorMessage for malformed JSON, missing or
    // mistyped fields, unsupported versions, invalid identifiers or invalid relative paths.
    static std::optional<ProjectManifest> Parse(
        std::string_view text,
        std::string& errorMessage);

    // Known fields written over the retained document, so unknown members survive.
    nlohmann::json Serialize() const;

    const AssetID& GetProjectID() const noexcept;
    const std::string& GetDisplayName() const noexcept;
    const ProjectContentPaths& GetContentPaths() const noexcept;

    // Project-relative .scene path for packaged builds; empty when not configured.
    const std::filesystem::path& GetStartupScene() const noexcept;

private:
    ProjectManifest(nlohmann::json document, AssetID projectID);

    nlohmann::json m_document;
    AssetID m_projectID;
    std::string m_displayName;
    ProjectContentPaths m_contentPaths;
    std::filesystem::path m_startupScene;
};

// Lexical check for a manifest path that must stay inside the project: non-empty, relative,
// no root name or root directory, and no ".." component after normalization.
bool IsContainedRelativePath(const std::filesystem::path& path);

// True when candidate equals container or is nested below it. Components are compared with
// Windows ordinal case-insensitive semantics; callers pass canonical paths when links matter.
bool IsPathWithin(const std::filesystem::path& candidate, const std::filesystem::path& container);
