#include "ProjectAssetReconciliation.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/ProjectAssetResolver.h"
#include "Project/ProjectPaths.h"
#include "Scene/SceneDocument.h"

#include <Windows.h>
#include <set>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;
    namespace Transaction = AssetReconciliationTransaction;

    Json Parse(const std::string& bytes)
    {
        std::vector<std::set<std::string>> keys;
        return Json::parse(bytes, [&](int, Json::parse_event_t event, Json& value)
        {
            if(event == Json::parse_event_t::object_start)
            {
                keys.emplace_back();
            }

            if(event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            {
                throw std::runtime_error("Duplicate JSON key; reconciliation cannot safely preserve this document.");
            }

            if(event == Json::parse_event_t::object_end)
            {
                keys.pop_back();
            }

            return true;
        });
    }

    bool SafeRegisteredPath(const ProjectPaths& paths, const fs::path& relative)
    {
        for(const auto& part : relative)
        {
            const auto text = part.native();

            if(text.empty() || text.find_first_of(L":*?\"<>|") != std::wstring::npos ||
               text.back() == L'.' || text.back() == L' ')
            {
                return false;
            }
        }

        auto current = paths.GetRoot();

        for(const auto& part : relative)
        {

            current /= part;
            const auto attributes = GetFileAttributesW(current.c_str());

            if(attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto code = GetLastError();
                return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
            }

            if(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                return false;
            }
        }

        return true;
    }

    bool Matches(const Json& value, const std::set<std::string>& missing)
    {
        return value.is_string() && missing.contains(value.get<std::string>());
    }
}

bool ProjectAssetReconciliation::Run(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    Report& report,
    std::string& error)
{
    report = {};
    error.clear();

    try
    {
        std::string registryBytes;

        if(!Transaction::ReadExisting(paths, "Assets.registry.json", registryBytes, error))
        {
            throw std::runtime_error(error);
        }

        auto registryJson = Parse(registryBytes);
        ProjectAssetRegistry candidate;

        if(!AssetRegistrySerializer::Deserialize(registryJson, candidate, &error))
        {
            throw std::runtime_error(error);
        }

        if(AssetRegistrySerializer::Serialize(candidate) != AssetRegistrySerializer::Serialize(registry))
        {
            throw std::runtime_error("Registry changed after project-load validation.");
        }

        std::set<std::string> missing;

        for(const auto& record : registry.GetAssets())
        {
            const auto resolution = ProjectAssetResolver::Resolve(paths, registry, record.id);
            const bool safe = SafeRegisteredPath(paths, record.relativePath);

            if(safe && resolution.status == AssetAvailability::Missing)
            {
                missing.insert(record.id.ToReference());
                candidate.Unregister(record.id);
            }
            else if(!safe || resolution.status != AssetAvailability::Available)
            {
                report.skipped.push_back(record.id.ToReference() + " (" + record.relativePath.generic_string() +
                    "): unsafe, inaccessible or indeterminate; registration retained.");
            }
        }

        report.missingDetected = missing.size();

        if(missing.empty())
        {
            return true;
        }

        std::vector<Transaction::Replacement> replacements;
        size_t materialReferences = 0;
        size_t sceneReferences = 0;
        fs::recursive_directory_iterator iterator(paths.GetRoot());

        for(const auto& entry : iterator)
        {
            const auto relative = entry.path().lexically_relative(paths.GetRoot());
            const auto attributes = GetFileAttributesW(entry.path().c_str());

            if(attributes == INVALID_FILE_ATTRIBUTES)
            {
                throw std::runtime_error("Cannot inspect project entry: " + entry.path().string());
            }

            if(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                throw std::runtime_error("Linked project entry prevents complete reference discovery: " + entry.path().string());
            }

            if(attributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }

            const auto extension = entry.path().extension().wstring();
            const bool material = _wcsicmp(extension.c_str(), L".material") == 0;
            const bool scene = _wcsicmp(extension.c_str(), L".scene") == 0;

            if(!material && !scene)
            {
                continue;
            }

            std::string bytes;

            if(!Transaction::ReadExisting(paths, relative, bytes, error))
            {
                throw std::runtime_error(entry.path().string() + ": " + error);
            }

            Json document;

            try
            {
                document = Parse(bytes);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error(entry.path().string() + ": " + exception.what());
            }

            const auto& schemaVersion = document.at("schemaVersion");
            const bool supportedSchema = schemaVersion.is_number_integer() &&
                (material ? schemaVersion == 1 :
                    schemaVersion >= SceneDocument::kLegacySchemaVersion &&
                    schemaVersion <= SceneDocument::kSchemaVersion);

            if(!document.is_object() || !supportedSchema)
            {
                throw std::runtime_error("Unsupported document schema: " + entry.path().string());
            }

            size_t changed = 0;

            if(material)
            {
                MaterialAssetData validated;

                if(!MaterialAssetSerializer::Deserialize(document, validated, &error))
                {
                    throw std::runtime_error(entry.path().string() + ": " + error);
                }

                auto& textures = document.at("textures");

                for(const char* slot : {"baseColor", "normal", "roughness", "metallic", "arm", "ao", "height"})
                {
                    if(Matches(textures.at(slot), missing))
                    {
                        textures[slot] = nullptr;
                        ++changed;
                    }
                }

                materialReferences += changed;
            }
            else
            {
                auto& objects = document.at("objects");

                if(!objects.is_array())
                {
                    throw std::runtime_error("Invalid scene object collection: " + entry.path().string());
                }

                for(auto& object : objects)
                {
                    const bool meshRemoved = Matches(object.at("meshReference"), missing);

                    if(object.contains("materialSlotBindings"))
                    {
                        auto& bindings = object.at("materialSlotBindings");
                        std::set<std::string> slotIDs;

                        if(!bindings.is_array())
                        {
                            throw std::runtime_error("Scene material-slot bindings must be an array.");
                        }

                        for(const auto& binding : bindings)
                        {
                            const auto slotID = binding.at("slotID").get<std::string>();
                            const auto reference = binding.at("materialReference").get<std::string>();

                            if(slotID.empty() || !slotIDs.insert(slotID).second || !AssetID::ParseReference(reference))
                            {
                                throw std::runtime_error("Invalid or duplicate scene material-slot binding.");
                            }
                        }

                        // Explicit missing-reference reconciliation removes overrides
                        // to restore fallback. Removing a mesh also removes its bindings.
                        for(auto binding = bindings.begin(); binding != bindings.end();)
                        {
                            if(meshRemoved || Matches(binding->at("materialReference"), missing))
                            {
                                binding = bindings.erase(binding);
                                ++changed;
                            }
                            else
                            {
                                ++binding;
                            }
                        }
                    }

                    for(const char* binding : {"meshReference", "materialReference"})
                    {
                        auto& value = object.at(binding);

                        if(!value.is_string())
                        {
                            throw std::runtime_error("Invalid scene binding: " + entry.path().string());
                        }

                        if(Matches(value, missing))
                        {
                            value = "";
                            ++changed;
                        }
                    }
                }

                sceneReferences += changed;
            }

            if(changed)
            {
                replacements.push_back({relative, document.dump(2) + "\n", bytes});
            }
        }

        auto& records = registryJson.at("assets");

        for(auto it = records.begin(); it != records.end();)
        {
            const auto id = AssetID::Parse(it->at("id").get<std::string>());

            if(id && missing.contains(id->ToReference()))
            {
                it = records.erase(it);
            }
            else
            {
                ++it;
            }
        }

        replacements.push_back({"Assets.registry.json", registryJson.dump(2) + "\n", registryBytes});

        for(const auto& record : registry.GetAssets())
        {
            if(missing.contains(record.id.ToReference()) &&
               (!SafeRegisteredPath(paths, record.relativePath) ||
                ProjectAssetResolver::Resolve(paths, registry, record.id).status != AssetAvailability::Missing))
            {
                throw std::runtime_error("Missing-asset status changed during preparation: " + record.relativePath.string());
            }
        }

        if(!Transaction::Commit(paths, replacements, error))
        {
            throw std::runtime_error("Reconciliation transaction failed: " + error);
        }

        std::string recoveryError;

        if(!Transaction::Recover(paths, recoveryError))
        {
            throw std::runtime_error("Committed reconciliation still requires recovery: " + recoveryError);
        }

        registry = std::move(candidate);
        report.registrationsRemoved = missing.size();
        report.materialReferencesCleared = materialReferences;
        report.sceneReferencesCleared = sceneReferences;

        for(const auto& replacement : replacements)
        {
            report.filesModified.push_back(replacement.relativePath.generic_string());
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
