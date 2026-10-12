#include "assets/ProjectTextureImportService.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetResolver.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <vector>

namespace
{
    bool Contained(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto relative = path.lexically_relative(root);

        if(relative.empty() || relative.is_absolute())
        {
            return false;
        }

        for(const auto& part : relative)
        {
            if(part == "..")
            {
                return false;
            }
        }

        return true;
    }

    bool Decode(const std::filesystem::path& path, const std::string& extension)
    {
        if(extension == ".dds")
        {
            DirectX::ScratchImage image;

            if(!DDSLoader::Load(path, image))
            {
                return false;
            }

            const auto format = image.GetMetadata().format;
            if(format != DXGI_FORMAT_BC1_UNORM && format != DXGI_FORMAT_BC1_UNORM_SRGB &&
               format != DXGI_FORMAT_BC4_UNORM && format != DXGI_FORMAT_BC5_UNORM)
            {
                throw std::runtime_error("Unsupported DDS format " + std::to_string(static_cast<unsigned int>(format)) +
                    ". Supported formats: BC1 UNORM/sRGB, BC4 UNORM, BC5 UNORM.");
            }

            return true;
        }

        if(extension == ".hdr")
        {
            HDRImageData image;
            return ImageLoader::LoadHDR(path, image);
        }

        ImageData image;
        return ImageLoader::Load(path, image);
    }

    std::runtime_error WindowsFailure(const char* operation)
    {
        return std::runtime_error(std::string(operation) + " Windows error " + std::to_string(GetLastError()));
    }
}

bool ProjectTextureImportService::Import(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& source,
    const std::filesystem::path& destinationDirectory,
    std::filesystem::path& importedPath,
    std::string& error)
{
    namespace fs = std::filesystem;
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    importedPath.clear();
    error.clear();

    try
    {
        std::string extension = source.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        const std::array supported = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif",
            ".pic", ".pnm", ".ppm", ".pgm", ".hdr", ".dds"};

        if(std::find(supported.begin(), supported.end(), extension) == supported.end())
        {
            throw std::runtime_error("Unsupported texture file extension.");
        }

        const auto filename = source.filename().wstring();

        if(filename.empty() || filename.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
           filename.back() == L'.' || filename.back() == L' ')
        {
            throw std::runtime_error("Unsafe texture filename.");
        }

        const auto root = fs::canonical(paths.GetRoot());
        const auto destination = fs::canonical(destinationDirectory);

        if(!Contained(root, destination))
        {
            throw std::runtime_error("Import destination must be within the active project's asset root.");
        }

        const auto sourcePath = fs::canonical(source);

        if(!fs::is_regular_file(sourcePath))
        {
            throw std::runtime_error("Source is not a regular file.");
        }

        if(sourcePath.parent_path() == destination)
        {
            throw std::runtime_error("Source is already in the destination folder.");
        }

        const auto id = AssetID::Generate();

        if(!id || registry.Find(*id))
        {
            throw std::runtime_error("Failed to generate a unique texture AssetID.");
        }

        const auto temporary = destination / (L".import-" + fs::path(id->ToString()).wstring() + L".tmp");
        input = CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(input == INVALID_HANDLE_VALUE)
        {
            throw WindowsFailure("Cannot open texture source.");
        }

        output = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

        if(output == INVALID_HANDLE_VALUE)
        {
            throw WindowsFailure("Cannot create import staging file.");
        }

        std::array<char, 65536> buffer{};
        DWORD read = 0;

        for(;;)
        {
            if(!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                throw WindowsFailure("Cannot read texture source.");
            }

            if(read == 0)
            {
                break;
            }

            DWORD offset = 0;

            while(offset < read)
            {
                DWORD written = 0;

                if(!WriteFile(output, buffer.data() + offset, read - offset, &written, nullptr) || written == 0)
                {
                    throw WindowsFailure("Cannot copy texture data.");
                }

                offset += written;
            }
        }

        // The source is still locked against writes/deletion. Decode these same
        // bytes through the existing file-based loaders, whose read-only sharing
        // is incompatible with a writable staging handle.
        if(!Decode(sourcePath, extension))
        {
            throw std::runtime_error("Invalid image data or unsupported DDS layout/format.");
        }

        CloseHandle(input);
        input = INVALID_HANDLE_VALUE;

        if(!FlushFileBuffers(output))
        {
            throw WindowsFailure("Cannot flush texture copy.");
        }

        fs::path finalPath;
        ProjectAssetRegistry staged;

        for(unsigned int suffix = 0;; ++suffix)
        {
            if(suffix == (std::numeric_limits<unsigned int>::max)())
            {
                throw std::runtime_error("No available texture filename suffix.");
            }

            const auto name = suffix == 0 ? source.filename() :
                fs::path(source.stem().wstring() + L" (" + std::to_wstring(suffix) + L")" + source.extension().wstring());
            finalPath = destination / name;
            const AssetRecord record{*id, AssetType::Texture, finalPath.lexically_relative(root)};
            staged = registry;

            if(!ValidateAssetRecord(record))
            {
                throw std::runtime_error("Invalid destination registry path.");
            }

            if(!staged.Register(record))
            {
                continue;
            }

            AssetRegistrySerializer::Serialize(staged);
            const auto wide = finalPath.wstring();
            // Win32 expects FileName to be null-terminated; FileNameLength still
            // excludes the terminator. Without it, publication can append heap bytes.
            const size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (wide.size() + 1) * sizeof(wchar_t);
            std::vector<unsigned char> storage(bytes);
            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            rename->ReplaceIfExists = FALSE;
            rename->RootDirectory = nullptr;
            rename->FileNameLength = static_cast<DWORD>(wide.size() * sizeof(wchar_t));
            std::memcpy(rename->FileName, wide.data(), rename->FileNameLength);
            rename->FileName[wide.size()] = L'\0';

            if(SetFileInformationByHandle(output, FileRenameInfo, rename, static_cast<DWORD>(bytes)))
            {
                break;
            }

            const DWORD failure = GetLastError();

            if(failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS)
            {
                throw WindowsFailure("Cannot publish texture copy.");
            }
        }

        // Keep the exact published file locked against replacement until registration commits.
        if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &error))
        {
            throw std::runtime_error("Cannot persist texture registry: " + error);
        }

        registry = std::move(staged);
        importedPath = std::move(finalPath);
        CloseHandle(output);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();

        if(input != INVALID_HANDLE_VALUE)
        {
            CloseHandle(input);
        }

        if(output != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;

            if(!SetFileInformationByHandle(output, FileDispositionInfo, &disposition, sizeof(disposition)))
            {
                error += " Rollback failed; an unregistered import file may remain.";
            }

            CloseHandle(output);
        }

        return false;
    }
}
