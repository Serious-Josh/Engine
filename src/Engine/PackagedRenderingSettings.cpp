#include "Engine/PackagedRenderingSettings.h"

namespace PackagedRenderingSettings
{
    bool SelectBuildSource(const LoadedProjectSettings& saved, ProjectSettings& source, std::string& error)
    {
        using Status = PersonalSettingsFile::Status;
        error.clear();

        switch(saved.status)
        {
        case Status::Missing:
            source = ProjectSettings{};
            return true;
        case Status::Loaded:
            if(saved.WasRecovered())
            {
                error = "ProjectSettings.json has values that were repaired when the project opened (" +
                    saved.warnings.front() + "). Open Edit > Project Settings and Save to confirm them, "
                    "then build again.";
                return false;
            }

            source = saved.settings;
            return true;
        case Status::Malformed:
            error = "ProjectSettings.json is malformed, so the Editor is using default settings. Open Edit > "
                "Project Settings and Save to replace it (the original is kept as ProjectSettings.json.invalid), "
                "then build again.";
            return false;
        case Status::UnsupportedVersion:
            error = "ProjectSettings.json was written by a newer Engine version and cannot be packaged by this one.";
            return false;
        default:
            error = "ProjectSettings.json could not be read, so its saved settings cannot be packaged. Check the "
                "file's permissions, reopen the project, then build again.";
            return false;
        }
    }

    std::vector<std::byte> Encode(const ProjectSettings& settings)
    {
        const std::string text = ProjectSettingsFile::Serialize(settings, nlohmann::json::object());
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return {bytes, bytes + text.size()};
    }

    std::string_view Describe(Source source)
    {
        switch(source)
        {
        case Source::Missing:
            return "none in the archive (built before Project Settings were packaged); using defaults";
        case Source::Cooked:
            return "cooked project settings";
        case Source::Recovered:
            return "cooked project settings, with repaired values";
        case Source::Malformed:
        default:
            return "invalid cooked project settings; using defaults";
        }
    }

    Loaded Decode(std::span<const std::byte> payload)
    {
        using Status = PersonalSettingsFile::Status;
        const LoadedProjectSettings parsed = ProjectSettingsFile::Parse(
            std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));

        if(parsed.status == Status::UnsupportedVersion)
        {
            throw UnsupportedVersion("The game's rendering settings were written by a newer Engine version; "
                "rebuild the game with this Engine.");
        }

        Loaded loaded;

        // PS.1's malformed-file guidance is about the project file; nothing is moved or saved here.
        if(parsed.status != Status::Loaded)
        {
            loaded.source = Source::Malformed;
            loaded.warnings.push_back("The cooked rendering settings are not a valid Project Settings document; "
                "using the default Legacy configuration. Rebuild the game to repair it.");
            return loaded;
        }

        loaded.warnings = parsed.warnings;

        loaded.rendering = parsed.settings.rendering;
        loaded.source = parsed.warnings.empty() ? Source::Cooked : Source::Recovered;
        return loaded;
    }

    Loaded Admit(const EngineAssetPak::Entry* entry, const std::function<std::vector<std::byte>()>& read)
    {
        if(!entry)
        {
            return {};
        }

        if(entry->type == EngineAssetPak::Type::Resource && entry->payloadVersion > PayloadVersion)
        {
            throw UnsupportedVersion("The game's rendering settings entry has payload version " +
                std::to_string(entry->payloadVersion) + "; this Engine reads version " +
                std::to_string(PayloadVersion) + ". Rebuild the game with this Engine.");
        }

        Loaded malformed;
        malformed.source = Source::Malformed;

        if(entry->type != EngineAssetPak::Type::Resource || entry->payloadVersion != PayloadVersion ||
            entry->uncompressedBytes > MaximumPayloadBytes)
        {
            malformed.warnings.push_back("The rendering settings entry has an unexpected type, version or size.");
            return malformed;
        }

        std::vector<std::byte> bytes;

        try
        {
            bytes = read();
        }
        catch(const std::exception& e)
        {
            malformed.warnings.push_back(std::string("The rendering settings entry cannot be read: ") + e.what());
            return malformed;
        }

        return Decode(bytes);
    }
}
