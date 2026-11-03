#include "assets/AssetRecord.h"
#include <algorithm>
#include <string_view>

namespace
{
    // Canonical project content path for discovered assets (Script, AudioBank): below a fixed Content directory,
    // with the expected extension, forward slashes only and no Windows-reserved or traversal components.
    bool IsCanonicalContentPath(const std::filesystem::path& relativePath, std::string_view prefix,
        const wchar_t* extension)
    {
        const auto text = relativePath.generic_u8string();
        const std::string path(text.begin(), text.end());

        if(!path.starts_with(prefix) || path.size() <= prefix.size() ||
            _wcsicmp(relativePath.extension().c_str(), extension) != 0 ||
            path.find_first_of("\\:<>\"|?*") != std::string::npos || path.find("//") != std::string::npos ||
            std::any_of(path.begin(), path.end(), [](unsigned char value) { return value < 32; }))
        {
            return false;
        }

        for(const auto& part : relativePath)
        {
            if(part.empty() || part == "." || part == ".." || part.native().back() == L'.' ||
                part.native().back() == L' ')
            {
                return false;
            }

            const auto value = part.native();
            const auto stem = value.substr(0, value.find(L'.'));

            if(_wcsicmp(stem.c_str(), L"CON") == 0 || _wcsicmp(stem.c_str(), L"PRN") == 0 ||
                _wcsicmp(stem.c_str(), L"AUX") == 0 || _wcsicmp(stem.c_str(), L"NUL") == 0 ||
                (stem.size() == 4 && stem[3] >= L'1' && stem[3] <= L'9' &&
                    (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 || _wcsnicmp(stem.c_str(), L"LPT", 3) == 0)))
            {
                return false;
            }
        }

        return true;
    }
}

bool ValidateAssetRecord(const AssetRecord& record)
{
    if(!record.meshImportUnits.IsValid() ||
        (record.type != AssetType::Mesh && record.meshImportUnits != MeshImportUnits{}))
    {
        return false;
    }

    std::filesystem::path expectedDirectory;

    switch(record.type){
        case AssetType::Material:
            expectedDirectory = "Materials";
            break;
        case AssetType::Mesh:
            expectedDirectory = "Meshes";
            break;
        case AssetType::Texture:
            // Texture identity is independent of organizational folders.
            break;
        case AssetType::Script:
            return IsCanonicalContentPath(record.relativePath, "Content/Scripts/", L".as");
        case AssetType::AudioBank:
            return IsCanonicalContentPath(record.relativePath, "Content/Audio/Banks/", L".bank");

        default:
            return false;
    }

    const auto& path = record.relativePath;

    if(path.empty() ||
       path.is_absolute() ||
       path.has_root_name() ||
       path.has_root_directory() ||
       !path.has_filename() ||
       path.filename() == "." ||
       path.filename() == "..")
    {
        return false;
    }

    for(const auto& component : path){
        if(component.empty() ||
           component == "." ||
           component == ".."){
            return false;
        }
    }

    if(record.type == AssetType::Texture)
    {
        return true;
    }

    auto it = path.begin();

    if(it == path.end() || *it != expectedDirectory){
        return false;
    }

    ++it;

    if(it == path.end()){
        return false;
    }

    return true;
}
