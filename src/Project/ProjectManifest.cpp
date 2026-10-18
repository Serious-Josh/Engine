#include "Project/ProjectManifest.h"

#include "assets/ProjectAssetRegistry.h"

#include <array>
#include <exception>
#include <utility>

namespace
{
    constexpr std::string_view kNilUUID = "00000000-0000-0000-0000-000000000000";

    struct ContentPathField
    {
        const char* key;
        std::filesystem::path ProjectContentPaths::* member;
    };

    constexpr std::array kContentPathFields = {
        ContentPathField{"scenes", &ProjectContentPaths::scenes},
        ContentPathField{"materials", &ProjectContentPaths::materials},
        ContentPathField{"meshes", &ProjectContentPaths::meshes},
        ContentPathField{"textures", &ProjectContentPaths::textures}
    };

    std::filesystem::path PathFromUTF8(const std::string& text)
    {
        const std::u8string utf8(text.begin(), text.end());
        return std::filesystem::path(utf8);
    }

    std::string PathToUTF8(const std::filesystem::path& path)
    {
        const auto utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // Lexical normal form without a trailing separator, so "Scenes/" and "Scenes" compare equal.
    std::filesystem::path NormalizeRelativePath(const std::filesystem::path& path)
    {
        auto normalized = path.lexically_normal();

        if(!normalized.has_filename())
        {
            normalized = normalized.parent_path();
        }

        return normalized;
    }

    bool ReadRequiredString(
        const nlohmann::json& document,
        const char* key,
        std::string& value,
        std::string& errorMessage)
    {
        const auto found = document.find(key);

        if(found == document.end())
        {
            errorMessage = std::string("Missing required field \"") + key + "\".";
            return false;
        }

        if(!found->is_string())
        {
            errorMessage = std::string("Field \"") + key + "\" must be a string.";
            return false;
        }

        value = found->get<std::string>();
        return true;
    }

    bool ReadContentPaths(
        const nlohmann::json& document,
        ProjectContentPaths& contentPaths,
        std::string& errorMessage)
    {
        const auto found = document.find("contentPaths");

        if(found == document.end())
        {
            return true;
        }

        if(!found->is_object())
        {
            errorMessage = "Field \"contentPaths\" must be an object.";
            return false;
        }

        ProjectContentPaths configured;

        for(const auto& field : kContentPathFields)
        {
            const auto value = found->find(field.key);

            if(value == found->end())
            {
                continue;
            }

            if(!value->is_string())
            {
                errorMessage = std::string("contentPaths.") + field.key + " must be a string.";
                return false;
            }

            const auto path = PathFromUTF8(value->get<std::string>());

            if(!IsContainedRelativePath(path))
            {
                errorMessage = std::string("contentPaths.") + field.key +
                    " must be a project-relative path that stays inside the project directory.";
                return false;
            }

            configured.*field.member = NormalizeRelativePath(path);
        }

        // Distinct content kinds must never share or nest directories.
        for(std::size_t first = 0; first < kContentPathFields.size(); ++first)
        {
            for(std::size_t second = first + 1; second < kContentPathFields.size(); ++second)
            {
                const auto& firstPath = configured.*kContentPathFields[first].member;
                const auto& secondPath = configured.*kContentPathFields[second].member;

                if(IsPathWithin(firstPath, secondPath) || IsPathWithin(secondPath, firstPath))
                {
                    errorMessage = std::string("contentPaths.") + kContentPathFields[first].key + " and contentPaths." +
                        kContentPathFields[second].key + " conflict: content directories must be distinct.";
                    return false;
                }
            }
        }

        // Format version 1 records the layout but does not remap it; ProjectPaths implements
        // only the standard directories.
        const ProjectContentPaths standardLayout;

        for(const auto& field : kContentPathFields)
        {
            if(!ProjectAssetRegistry::PathsEqual(configured.*field.member, standardLayout.*field.member))
            {
                errorMessage = std::string("contentPaths.") + field.key + " = \"" +
                    PathToUTF8(configured.*field.member) + "\" is not supported by format version 1; use \"" +
                    PathToUTF8(standardLayout.*field.member) + "\".";
                return false;
            }
        }

        contentPaths = standardLayout;
        return true;
    }

    bool ReadSettings(
        const nlohmann::json& document,
        std::filesystem::path& startupScene,
        std::string& errorMessage)
    {
        const auto found = document.find("settings");

        if(found == document.end())
        {
            return true;
        }

        if(!found->is_object())
        {
            errorMessage = "Field \"settings\" must be an object.";
            return false;
        }

        const auto scene = found->find("startupScene");

        if(scene == found->end() || scene->is_null())
        {
            return true;
        }

        if(!scene->is_string())
        {
            errorMessage = "settings.startupScene must be a string or null.";
            return false;
        }

        const auto path = PathFromUTF8(scene->get<std::string>());

        if(!IsContainedRelativePath(path) || !ProjectAssetRegistry::PathsEqual(path.extension(), ".scene"))
        {
            errorMessage = "settings.startupScene must be a project-relative .scene path inside the project.";
            return false;
        }

        startupScene = NormalizeRelativePath(path);
        return true;
    }
}

bool IsContainedRelativePath(const std::filesystem::path& path)
{
    if(path.empty() || path.has_root_name() || path.has_root_directory() || path.is_absolute())
    {
        return false;
    }

    const auto normalized = NormalizeRelativePath(path);

    if(normalized.empty() || normalized == ".")
    {
        return false;
    }

    for(const auto& component : normalized)
    {
        if(component == "..")
        {
            return false;
        }
    }

    return true;
}

// Component-wise, ordinal case-insensitive (Windows semantics). Equal paths are within each other.
bool IsPathWithin(const std::filesystem::path& candidate, const std::filesystem::path& container)
{
    auto candidateComponent = candidate.begin();

    for(const auto& containerComponent : container)
    {
        if(candidateComponent == candidate.end() ||
            !ProjectAssetRegistry::PathsEqual(*candidateComponent, containerComponent))
        {
            return false;
        }

        ++candidateComponent;
    }

    return true;
}

ProjectManifest::ProjectManifest(nlohmann::json document, AssetID projectID)
    : m_document(std::move(document))
    , m_projectID(std::move(projectID))
{
}

std::optional<ProjectManifest> ProjectManifest::Parse(
    std::string_view text,
    std::string& errorMessage)
{
    errorMessage.clear();
    nlohmann::json document;

    try
    {
        document = nlohmann::json::parse(text);
    }
    catch(const std::exception& exception)
    {
        errorMessage = std::string("Manifest is not valid JSON: ") + exception.what();
        return std::nullopt;
    }

    if(!document.is_object())
    {
        errorMessage = "Manifest root must be a JSON object.";
        return std::nullopt;
    }

    std::string formatName;

    if(!ReadRequiredString(document, "format", formatName, errorMessage))
    {
        return std::nullopt;
    }

    if(formatName != kFormatName)
    {
        errorMessage = "Field \"format\" must be \"" + std::string(kFormatName) + "\".";
        return std::nullopt;
    }

    // Version is checked before other fields so a newer manifest reports the version
    // mismatch instead of a confusing field error.
    const auto version = document.find("formatVersion");

    if(version == document.end() || !version->is_number_integer())
    {
        errorMessage = "Field \"formatVersion\" is required and must be an integer.";
        return std::nullopt;
    }

    const auto formatVersion = version->get<long long>();

    if(formatVersion > kCurrentFormatVersion)
    {
        errorMessage = "Manifest format version " + std::to_string(formatVersion) +
            " is newer than this Engine supports (" + std::to_string(kCurrentFormatVersion) + ").";
        return std::nullopt;
    }

    if(formatVersion < kCurrentFormatVersion)
    {
        errorMessage = "Manifest format version " + std::to_string(formatVersion) + " is not a valid version.";
        return std::nullopt;
    }

    std::string projectIDText;

    if(!ReadRequiredString(document, "projectId", projectIDText, errorMessage))
    {
        return std::nullopt;
    }

    const auto projectID = AssetID::Parse(projectIDText);

    if(!projectID || projectIDText == kNilUUID)
    {
        errorMessage = "Field \"projectId\" must be a canonical lowercase UUID and not the nil UUID.";
        return std::nullopt;
    }

    ProjectManifest manifest(document, *projectID);

    if(!ReadRequiredString(document, "name", manifest.m_displayName, errorMessage))
    {
        return std::nullopt;
    }

    if(manifest.m_displayName.empty())
    {
        errorMessage = "Field \"name\" must not be empty.";
        return std::nullopt;
    }

    if(!ReadContentPaths(document, manifest.m_contentPaths, errorMessage) ||
        !ReadSettings(document, manifest.m_startupScene, errorMessage))
    {
        return std::nullopt;
    }

    return manifest;
}

nlohmann::json ProjectManifest::Serialize() const
{
    nlohmann::json document = m_document;

    document["format"] = kFormatName;
    document["formatVersion"] = kCurrentFormatVersion;
    document["projectId"] = m_projectID.ToString();
    document["name"] = m_displayName;

    auto& contentPaths = document["contentPaths"];

    if(!contentPaths.is_object())
    {
        contentPaths = nlohmann::json::object();
    }

    for(const auto& field : kContentPathFields)
    {
        contentPaths[field.key] = PathToUTF8(m_contentPaths.*field.member);
    }

    auto& settings = document["settings"];

    if(!settings.is_object())
    {
        settings = nlohmann::json::object();
    }

    if(m_startupScene.empty())
    {
        settings["startupScene"] = nullptr;
    }
    else
    {
        settings["startupScene"] = PathToUTF8(m_startupScene);
    }

    return document;
}

const AssetID& ProjectManifest::GetProjectID() const noexcept
{
    return m_projectID;
}

const std::string& ProjectManifest::GetDisplayName() const noexcept
{
    return m_displayName;
}

const ProjectContentPaths& ProjectManifest::GetContentPaths() const noexcept
{
    return m_contentPaths;
}

const std::filesystem::path& ProjectManifest::GetStartupScene() const noexcept
{
    return m_startupScene;
}
