#pragma once

#include "Editor/ViewportGridSnap.h"
#include "Project/PersonalSettingsFile.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Viewport settings the Editor already owns (G.1-G.3). The defaults are the Editor's own.
struct ViewportPreferences
{
    bool snapEnabled = false;
    float translationSnapInterval = ViewportGridSnap::kDefaultInterval;
    float rotationSnapIntervalDegrees = ViewportGridSnap::kDefaultRotationIntervalDegrees;
    bool gridVisible = true;

    bool operator==(const ViewportPreferences&) const = default;
};

// A project the user opened, for the PR.6 Project Browser. The manifest path is absolute (this
// is personal, per-machine state); the project ID lets PR.6 recognise a relocated project.
struct RecentProject
{
    std::filesystem::path manifestPath;
    std::string projectID;
    std::string displayName;
    std::string lastOpenedUtc;

    bool operator==(const RecentProject&) const = default;
};

// Global personal Editor preferences: %LOCALAPPDATA%/Engine/Editor/Preferences.json, or the
// portable Editor's user directory. Never project configuration, never written to a manifest.
class EditorPreferences
{
public:
    static constexpr std::string_view kFormatName = "EngineEditorPreferences";
    static constexpr int kCurrentFormatVersion = 1;
    static constexpr std::size_t kMaximumRecentProjects = 10;

    struct LoadResult
    {
        PersonalSettingsFile::Status status = PersonalSettingsFile::Status::Missing;
        std::string message; // Malformed/unreadable details and per-field recoveries.
    };

    // Never fails: any problem yields defaults (per invalid field, or entirely) plus a status.
    static EditorPreferences Load(const std::filesystem::path& path, LoadResult& result);
    static EditorPreferences Parse(std::string_view text, LoadResult& result);

    // Known fields written over the retained document, so unknown members survive.
    std::string Serialize() const;

    const ViewportPreferences& GetViewport() const noexcept;
    void SetViewport(const ViewportPreferences& viewport);

    const std::vector<RecentProject>& GetRecentProjects() const noexcept;

    // Moves the project to the front, replacing any entry for the same manifest (Windows path
    // identity) and trimming to the maximum. Missing entries are never removed implicitly.
    void RecordRecentProject(RecentProject project);
    void RemoveRecentProject(const std::filesystem::path& manifest);

    // Same manifest file as Windows resolves it: canonical when it exists, otherwise the
    // normalized absolute path, compared ordinal case-insensitively.
    static bool SameManifest(const std::filesystem::path& left, const std::filesystem::path& right);

private:
    nlohmann::json m_document = nlohmann::json::object();
    ViewportPreferences m_viewport;
    std::vector<RecentProject> m_recentProjects;
};
