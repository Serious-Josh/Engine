#include "Editor/EditorPreferences.h"

#include "assets/ProjectAssetRegistry.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <system_error>
#include <utility>

namespace
{
    std::filesystem::path PathFromUTF8(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string PathToUTF8(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    void AppendMessage(std::string& message, const std::string& addition)
    {
        if(!message.empty())
        {
            message += " ";
        }

        message += addition;
    }

    void ReadBool(const nlohmann::json& object, const char* key, bool& value, std::string& message)
    {
        const auto found = object.find(key);

        if(found == object.end())
        {
            return;
        }

        if(!found->is_boolean())
        {
            AppendMessage(message, std::string("viewport.") + key + " is not a boolean; using the default.");
            return;
        }

        value = found->get<bool>();
    }

    // Invalid, zero, negative, NaN or infinite intervals keep the default.
    void ReadInterval(const nlohmann::json& object, const char* key, float& value, std::string& message)
    {
        const auto found = object.find(key);

        if(found == object.end())
        {
            return;
        }

        const bool number = found->is_number();
        const double candidate = number ? found->get<double>() : 0.0;
        const auto asFloat = static_cast<float>(candidate);

        if(!number || !std::isfinite(candidate) || !ViewportGridSnap::IsValidInterval(asFloat))
        {
            AppendMessage(message, std::string("viewport.") + key + " is not a positive finite number; using the default.");
            return;
        }

        value = asFloat;
    }

    bool ReadRecentProject(const nlohmann::json& entry, RecentProject& project)
    {
        if(!entry.is_object())
        {
            return false;
        }

        const auto manifest = entry.find("manifest");

        if(manifest == entry.end() || !manifest->is_string() || manifest->get<std::string>().empty())
        {
            return false;
        }

        project.manifestPath = PathFromUTF8(manifest->get<std::string>());

        if(!project.manifestPath.is_absolute())
        {
            return false;
        }

        const auto readOptional = [&](const char* key, std::string& value)
        {
            const auto found = entry.find(key);

            if(found != entry.end() && found->is_string())
            {
                value = found->get<std::string>();
            }
        };

        readOptional("projectId", project.projectID);
        readOptional("name", project.displayName);
        readOptional("lastOpenedUtc", project.lastOpenedUtc);
        return true;
    }
}

EditorPreferences EditorPreferences::Load(const std::filesystem::path& path, LoadResult& result)
{
    std::string text;
    std::string error;
    result = {};
    result.status = PersonalSettingsFile::Read(path, text, error);

    if(result.status != PersonalSettingsFile::Status::Loaded)
    {
        result.message = error;
        return EditorPreferences();
    }

    return Parse(text, result);
}

EditorPreferences EditorPreferences::Parse(std::string_view text, LoadResult& result)
{
    EditorPreferences preferences;
    result.status = PersonalSettingsFile::Status::Loaded;
    result.message.clear();
    nlohmann::json document;

    try
    {
        document = nlohmann::json::parse(text);
    }
    catch(const std::exception& exception)
    {
        result.status = PersonalSettingsFile::Status::Malformed;
        result.message = std::string("Preferences are not valid JSON: ") + exception.what();
        return EditorPreferences();
    }

    const auto format = document.is_object() ? document.find("format") : document.end();
    const auto version = document.is_object() ? document.find("formatVersion") : document.end();

    const bool formatValid = format != document.end() && format->is_string() && format->get<std::string>() == kFormatName;

    if(!document.is_object() || !formatValid ||
        version == document.end() || !version->is_number_integer())
    {
        result.status = PersonalSettingsFile::Status::Malformed;
        result.message = "Preferences lack a valid format and formatVersion.";
        return EditorPreferences();
    }

    if(version->get<long long>() != kCurrentFormatVersion)
    {
        const bool newer = version->get<long long>() > kCurrentFormatVersion;
        result.status = newer ? PersonalSettingsFile::Status::UnsupportedVersion : PersonalSettingsFile::Status::Malformed;
        result.message = "Preferences format version " + std::to_string(version->get<long long>()) + " is not supported.";
        return EditorPreferences();
    }

    preferences.m_document = document;
    const auto viewport = document.find("viewport");

    if(viewport != document.end() && viewport->is_object())
    {
        ReadBool(*viewport, "snapEnabled", preferences.m_viewport.snapEnabled, result.message);
        ReadInterval(*viewport, "translationSnapInterval", preferences.m_viewport.translationSnapInterval, result.message);
        ReadInterval(*viewport, "rotationSnapIntervalDegrees", preferences.m_viewport.rotationSnapIntervalDegrees,
            result.message);
        ReadBool(*viewport, "gridVisible", preferences.m_viewport.gridVisible, result.message);
    }

    const auto recent = document.find("recentProjects");

    if(recent != document.end() && recent->is_array())
    {
        for(const auto& entry : *recent)
        {
            RecentProject project;

            if(!ReadRecentProject(entry, project))
            {
                AppendMessage(result.message, "Skipped an invalid recent-project entry.");
                continue;
            }

            const bool duplicate = std::any_of(preferences.m_recentProjects.begin(), preferences.m_recentProjects.end(),
                [&](const RecentProject& existing)
                {
                    return SameManifest(existing.manifestPath, project.manifestPath);
                });

            if(!duplicate && preferences.m_recentProjects.size() < kMaximumRecentProjects)
            {
                preferences.m_recentProjects.push_back(std::move(project));
            }
        }
    }

    return preferences;
}

std::string EditorPreferences::Serialize() const
{
    nlohmann::json document = m_document;
    document["format"] = kFormatName;
    document["formatVersion"] = kCurrentFormatVersion;

    auto& viewport = document["viewport"];

    if(!viewport.is_object())
    {
        viewport = nlohmann::json::object();
    }

    viewport["snapEnabled"] = m_viewport.snapEnabled;
    viewport["translationSnapInterval"] = m_viewport.translationSnapInterval;
    viewport["rotationSnapIntervalDegrees"] = m_viewport.rotationSnapIntervalDegrees;
    viewport["gridVisible"] = m_viewport.gridVisible;

    nlohmann::json recent = nlohmann::json::array();

    for(const auto& project : m_recentProjects)
    {
        recent.push_back({
            {"manifest", PathToUTF8(project.manifestPath)},
            {"projectId", project.projectID},
            {"name", project.displayName},
            {"lastOpenedUtc", project.lastOpenedUtc}
        });
    }

    document["recentProjects"] = std::move(recent);
    return document.dump(4) + "\n";
}

const ViewportPreferences& EditorPreferences::GetViewport() const noexcept
{
    return m_viewport;
}

void EditorPreferences::SetViewport(const ViewportPreferences& viewport)
{
    m_viewport = viewport;
}

const std::vector<RecentProject>& EditorPreferences::GetRecentProjects() const noexcept
{
    return m_recentProjects;
}

void EditorPreferences::RemoveRecentProject(const std::filesystem::path& manifest)
{
    std::erase_if(m_recentProjects, [&](const RecentProject& recent)
    {
        return SameManifest(recent.manifestPath, manifest);
    });
}

void EditorPreferences::RecordRecentProject(RecentProject project)
{
    std::erase_if(m_recentProjects, [&](const RecentProject& existing)
    {
        return SameManifest(existing.manifestPath, project.manifestPath);
    });

    m_recentProjects.insert(m_recentProjects.begin(), std::move(project));

    if(m_recentProjects.size() > kMaximumRecentProjects)
    {
        m_recentProjects.resize(kMaximumRecentProjects);
    }
}

bool EditorPreferences::SameManifest(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const auto identity = [](const std::filesystem::path& path)
    {
        std::error_code error;
        auto resolved = std::filesystem::canonical(path, error);

        if(error)
        {
            resolved = std::filesystem::absolute(path, error).lexically_normal();
        }

        return resolved;
    };

    return ProjectAssetRegistry::PathsEqual(identity(left), identity(right));
}
