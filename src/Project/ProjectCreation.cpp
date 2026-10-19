#include "Project/ProjectCreation.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#if defined(ENGINE_PROJECT_CREATION_TESTING)
// Standalone local CPU fault injection. Production builds contain no callback or test dependency.
void VerifyProjectCreationStage(const std::filesystem::path& staging);
#endif

namespace
{
    // Remove only exact entries created by this attempt, in reverse order. In particular,
    // never recursively remove a tree that another process may have added files to.
    struct CreationArtifacts
    {
        std::vector<std::filesystem::path> paths;
        ~CreationArtifacts()
        {
            for(auto entry = paths.rbegin(); entry != paths.rend(); ++entry)
            {
                std::error_code ignored;
                std::filesystem::remove(*entry, ignored);
            }
        }
    };

    bool WriteExclusive(const std::filesystem::path& path, const std::string& text,
        CreationArtifacts& artifacts, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

        if(file == INVALID_HANDLE_VALUE)
        {
            error = "Cannot exclusively create project file (Windows error " + std::to_string(GetLastError()) + ").";
            return false;
        }

        artifacts.paths.push_back(path);
        DWORD written = 0;
        const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
            written == text.size() && FlushFileBuffers(file);
        CloseHandle(file);

        if(!success)
        {
            error = "Cannot write or flush project file.";
        }

        return success;
    }
}

bool ProjectCreation::Validate(const std::string& name, const std::filesystem::path& destination, std::string& error)
{
    error.clear();

    if(name.empty() || name.size() > 180 ||
        std::any_of(name.begin(), name.end(), [](unsigned char character)
        {
            return character < 32 || std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos;
        }) || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
    {
        error = "Enter a project name without leading spaces, trailing dots/spaces, or reserved filename characters.";
        return false;
    }

    try
    {
        const auto filename = std::filesystem::path(std::u8string(name.begin(), name.end()));
        auto base = filename.wstring();
        base.resize(base.find(L'.') == std::wstring::npos ? base.size() : base.find(L'.'));

        while(!base.empty() && base.back() == L' ')
        {
            base.pop_back();
        }

        std::transform(base.begin(), base.end(), base.begin(), towupper);
        const bool deviceNumber = base.size() == 4 &&
            (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
            ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3');

        if(base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
            base == L"CONIN$" || base == L"CONOUT$" || deviceNumber ||
            filename == L"." || filename == L"..")
        {
            error = "The project name is reserved by Windows.";
            return false;
        }

        if(!destination.is_absolute() || destination.native().starts_with(L"\\\\?\\") ||
            destination.native().starts_with(L"\\\\.\\"))
        {
            error = "Choose an absolute filesystem destination.";
            return false;
        }

        for(const auto& component : destination)
        {
            if(component == L"..")
            {
                error = "The destination cannot contain directory traversal.";
                return false;
            }
        }

        std::error_code filesystemError;
        const auto parent = std::filesystem::weakly_canonical(destination, filesystemError);

        if(filesystemError || parent.empty() ||
            std::filesystem::exists(parent / filename, filesystemError) || filesystemError)
        {
            error = "The destination is inaccessible or a file/directory with this project name already exists." +
                (filesystemError ? " " + filesystemError.message() : "");
            return false;
        }

        auto existing = parent;

        while(!std::filesystem::exists(existing, filesystemError) && !filesystemError)
        {
            existing = existing.parent_path();
        }

        if(filesystemError || !std::filesystem::is_directory(existing, filesystemError))
        {
            error = "The destination must be a directory on an accessible filesystem.";
            return false;
        }
    }
    catch(const std::exception& exception)
    {
        error = std::string("Invalid project name or destination: ") + exception.what();
        return false;
    }

    return true;
}

std::optional<OpenedProject> ProjectCreation::Create(
    const std::string& name, const std::filesystem::path& destination, std::string& error)
{
    if(!Validate(name, destination, error))
    {
        return std::nullopt;
    }

    CreationArtifacts artifacts;

    try
    {
        const auto projectID = AssetID::Generate();
        const auto transactionID = AssetID::Generate();

        if(!projectID || !transactionID)
        {
            error = "Cannot generate project identifiers.";
            return std::nullopt;
        }

        const auto parent = std::filesystem::weakly_canonical(destination);
        std::vector<std::filesystem::path> missingParents;
        auto ancestor = parent;

        while(!std::filesystem::exists(ancestor))
        {
            missingParents.push_back(ancestor);
            ancestor = ancestor.parent_path();
        }

        for(auto entry = missingParents.rbegin(); entry != missingParents.rend(); ++entry)
        {
            if(std::filesystem::create_directory(*entry))
            {
                artifacts.paths.push_back(*entry);
            }
        }

        const auto filename = std::filesystem::path(std::u8string(name.begin(), name.end()));
        const auto finalRoot = parent / filename;
        const auto staging = parent / (L".engine-create-" + std::filesystem::path(transactionID->ToString()).wstring());

        if(!std::filesystem::create_directory(staging))
        {
            error = "Cannot exclusively create the project staging directory.";
            return std::nullopt;
        }

        artifacts.paths.push_back(staging);

        for(const auto* directory : {L"Scenes", L"Materials", L"Meshes", L"Textures"})
        {
            const auto path = staging / directory;

            if(!std::filesystem::create_directory(path))
            {
                error = "Cannot exclusively create a project content directory.";
                return std::nullopt;
            }

            artifacts.paths.push_back(path);
        }

        nlohmann::json document = {
            {"format", ProjectManifest::kFormatName},
            {"formatVersion", ProjectManifest::kCurrentFormatVersion},
            {"projectId", projectID->ToString()}, {"name", name}};
        const auto manifest = ProjectManifest::Parse(document.dump(), error);

        if(!manifest)
        {
            return std::nullopt;
        }

        const auto manifestName = filename.wstring() + L".engineproj";

        if(!WriteExclusive(staging / manifestName, manifest->Serialize().dump(4) + "\n", artifacts, error))
        {
            return std::nullopt;
        }

#if defined(ENGINE_PROJECT_CREATION_TESTING)
        VerifyProjectCreationStage(staging);
#endif

        if(!WriteExclusive(staging / L".gitignore", ".engine/\n", artifacts, error))
        {
            return std::nullopt;
        }

        // Validate through the real loader before publication. Release the old file handle
        // before renaming its parent; final ownership is acquired with the unchanged protocol.
        auto verified = ProjectLoader::Open(staging / manifestName, error);

        if(!verified)
        {
            return std::nullopt;
        }

        verified.reset();

        if(!MoveFileExW(staging.c_str(), finalRoot.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            error = "Cannot publish project; the destination may already exist (Windows error " +
                std::to_string(GetLastError()) + ").";
            return std::nullopt;
        }

        // Published projects are never removed if another Editor wins the subsequent lock.
        artifacts.paths.clear();
        auto opened = ProjectLoader::Open(finalRoot / manifestName, error);

        if(!opened)
        {
            error = "Project was created, but could not be opened: " + error;
        }

        return opened;
    }
    catch(const std::exception& exception)
    {
        error = std::string("Project creation failed: ") + exception.what();
        return std::nullopt;
    }
}
