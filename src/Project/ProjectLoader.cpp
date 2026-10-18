#include "Project/ProjectLoader.h"

#include "assets/ProjectAssetRegistry.h"

#include <array>
#include <system_error>
#include <utility>

namespace
{
    std::string PathText(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // The directory must exist and, after resolving links and junctions, stay inside the root.
    bool ValidateContentDirectory(
        const std::filesystem::path& canonicalRoot,
        const std::filesystem::path& relativePath,
        std::string& errorMessage)
    {
        const auto directory = canonicalRoot / relativePath;
        std::error_code error;
        const auto status = std::filesystem::status(directory, error);

        if(!std::filesystem::exists(status))
        {
            errorMessage = "Required project directory is missing or inaccessible: " + PathText(directory);
            return false;
        }

        if(!std::filesystem::is_directory(status))
        {
            errorMessage = "Required project path is not a directory: " + PathText(directory);
            return false;
        }

        const auto canonicalDirectory = std::filesystem::canonical(directory, error);

        if(error)
        {
            errorMessage = "Cannot resolve project directory " + PathText(directory) + ": " + error.message();
            return false;
        }

        if(!IsPathWithin(canonicalDirectory, canonicalRoot))
        {
            errorMessage = "Project directory " + PathText(directory) + " resolves outside the project: " +
                PathText(canonicalDirectory);
            return false;
        }

        return true;
    }

    // The registry is optional (Editor initialization creates an empty one, as it always has),
    // but an existing entry must be an ordinary file inside the project.
    bool ValidateRegistryFile(
        const std::filesystem::path& canonicalRoot,
        const ProjectPaths& paths,
        std::string& errorMessage)
    {
        const auto registry = paths.GetAssetRegistryPath();
        std::error_code error;
        const auto status = std::filesystem::status(registry, error);

        if(status.type() == std::filesystem::file_type::not_found)
        {
            return true;
        }

        if(error)
        {
            errorMessage = "Cannot inspect the asset registry " + PathText(registry) + ": " + error.message();
            return false;
        }

        if(!std::filesystem::is_regular_file(status))
        {
            errorMessage = "The asset registry path is not a regular file: " + PathText(registry);
            return false;
        }

        const auto canonicalRegistry = std::filesystem::canonical(registry, error);

        if(error || !IsPathWithin(canonicalRegistry, canonicalRoot))
        {
            errorMessage = "The asset registry resolves outside the project: " + PathText(registry);
            return false;
        }

        return true;
    }
}

namespace ProjectLoader
{
    std::optional<OpenedProject> Open(
        const std::filesystem::path& manifestPath,
        std::string& errorMessage)
    {
        errorMessage.clear();
        std::error_code error;
        const auto absoluteManifest = std::filesystem::absolute(manifestPath, error).lexically_normal();

        if(error || absoluteManifest.empty())
        {
            errorMessage = "Cannot resolve the project manifest path: " + PathText(manifestPath);
            return std::nullopt;
        }

        if(!ProjectAssetRegistry::PathsEqual(absoluteManifest.extension(), ProjectManifest::kFileExtension))
        {
            errorMessage = "A project file must have the .engineproj extension: " + PathText(absoluteManifest);
            return std::nullopt;
        }

        // Lock first so the manifest this process validates is the one it holds.
        ProjectLock::Result lockResult = ProjectLock::Result::Failed;
        std::string lockError;
        ProjectLock lock = ProjectLock::Acquire(absoluteManifest, lockResult, lockError);

        if(lockResult != ProjectLock::Result::Acquired)
        {
            errorMessage = lockError + " (" + PathText(absoluteManifest) + ")";
            return std::nullopt;
        }

        std::string manifestText;

        if(!lock.ReadManifest(manifestText, errorMessage))
        {
            return std::nullopt;
        }

        std::string parseError;
        auto manifest = ProjectManifest::Parse(manifestText, parseError);

        if(!manifest)
        {
            errorMessage = "Invalid project manifest " + PathText(absoluteManifest) + ": " + parseError;
            return std::nullopt;
        }

        // The project root is the manifest's directory, never a path stored in the manifest.
        const auto canonicalRoot = std::filesystem::canonical(absoluteManifest.parent_path(), error);

        if(error)
        {
            errorMessage = "Cannot resolve the project directory " + PathText(absoluteManifest.parent_path()) +
                ": " + error.message();
            return std::nullopt;
        }

        const auto& contentPaths = manifest->GetContentPaths();
        const std::array requiredDirectories = {
            contentPaths.scenes,
            contentPaths.materials,
            contentPaths.meshes,
            contentPaths.textures
        };

        for(const auto& directory : requiredDirectories)
        {
            if(!ValidateContentDirectory(canonicalRoot, directory, errorMessage))
            {
                return std::nullopt;
            }
        }

        ProjectContext context(canonicalRoot, std::move(*manifest));

        if(!ValidateRegistryFile(canonicalRoot, context.GetPaths(), errorMessage))
        {
            return std::nullopt;
        }

        return OpenedProject{std::move(context), std::move(lock)};
    }
}
