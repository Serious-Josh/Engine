#include "TextureThumbnailDiskCache.h"
#include "Project/ProjectPaths.h"
#include "assets/AssetRecord.h"
#include "assets/AssetID.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace
{
    // Windows little-endian v1: 8 uint64 fields, identity bytes, tightly packed RGBA8.
    // Version includes resizing, HDR conversion, DDS interpretation and 96px extent.
    constexpr std::uint64_t kMagic = 0x31424d4854474e45;
    constexpr std::uint64_t kVersion = 1;
    constexpr size_t kMaximumEntries = 1024;
    constexpr size_t kMaximumIdentity = 16384;
    constexpr size_t kMaximumPixels = 96 * 96 * 4;

    std::uint64_t Hash(const unsigned char* data, size_t size)
    {
        std::uint64_t hash = 14695981039346656037ull;

        for(size_t index = 0; index < size; ++index)
        {
            hash = (hash ^ data[index]) * 1099511628211ull;
        }

        return hash;
    }

    std::string Hash(const std::string& text)
    {
        return std::to_string(Hash(reinterpret_cast<const unsigned char*>(text.data()), text.size()));
    }

    // Reject directory aliases and file reparse points instead of following cache links.
    bool SafePath(const std::filesystem::path& path)
    {
        std::filesystem::path prefix;

        for(const auto& component : path)
        {
            prefix /= component;

            if(!prefix.has_root_directory())
            {
                continue;
            }

            const DWORD attributes = GetFileAttributesW(prefix.c_str());

            if(attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return false;
            }
        }

        return true;
    }

    class DirectoryLock
    {
    public:
        DirectoryLock() = default;
        DirectoryLock(const DirectoryLock&) = delete;
        DirectoryLock& operator=(const DirectoryLock&) = delete;

        ~DirectoryLock()
        {
            for(HANDLE handle : m_handles)
            {
                CloseHandle(handle);
            }
        }

        bool Open(const std::filesystem::path& directory, bool create)
        {
            std::filesystem::path prefix;

            for(const auto& component : directory)
            {
                prefix /= component;

                if(!prefix.has_root_directory())
                {
                    continue;
                }

                if(create && !std::filesystem::exists(prefix))
                {
                    std::filesystem::create_directory(prefix);
                }

                // Pin each ancestor before descending. Denying delete-sharing
                // prevents a junction swap between validation and cache access.
                HANDLE handle = CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

                if(handle == INVALID_HANDLE_VALUE)
                {
                    return false;
                }

                BY_HANDLE_FILE_INFORMATION info{};
                const bool safe = GetFileInformationByHandle(handle, &info) &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                if(!safe)
                {
                    CloseHandle(handle);
                    return false;
                }

                try
                {
                    m_handles.push_back(handle);
                }
                catch(...)
                {
                    CloseHandle(handle);
                    throw;
                }
            }

            return true;
        }

    private:
        std::vector<HANDLE> m_handles;
    };

    bool Unchanged(const TextureThumbnailDiskCache::Source& source)
    {
        return std::filesystem::file_size(source.path) == source.size &&
            std::filesystem::last_write_time(source.path) == source.modified;
    }

    bool Prune(const std::filesystem::path& directory)
    {
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;

        for(const auto& entry : std::filesystem::directory_iterator(directory))
        {
            if(entry.path().extension() == ".tmp" && AssetID::Parse(entry.path().stem().string()) &&
               SafePath(entry.path()) && entry.is_regular_file())
            {
                // Recover abandoned writes after crashes; current writers hold
                // an exclusive handle and fresh temporaries are left alone.
                if(std::filesystem::file_time_type::clock::now() - entry.last_write_time() > std::chrono::hours(24))
                {
                    std::error_code ignored;
                    std::filesystem::remove(entry.path(), ignored);
                }
            }

            if(entry.path().extension() != ".thumb" || !SafePath(entry.path()) || !entry.is_regular_file())
            {
                continue;
            }

            // Bound bookkeeping even if external software fills the cache directory.
            if(entries.size() >= 4096)
            {
                return false;
            }

            entries.emplace_back(entry.last_write_time(), entry.path());
        }

        std::sort(entries.begin(), entries.end());

        // Leave room for one atomic publication. Only this dedicated bucket is touched.
        for(size_t index = 0; entries.size() - index >= kMaximumEntries; ++index)
        {
            if(!SafePath(entries[index].second) || !std::filesystem::remove(entries[index].second))
            {
                return false;
            }
        }

        return true;
    }
}

TextureThumbnailDiskCache::Source TextureThumbnailDiskCache::Describe(
    const ProjectPaths& paths,
    const AssetRecord& record,
    const std::filesystem::path& source)
{
    Source result;

    try
    {
        const auto project = std::filesystem::canonical(paths.GetRoot()).generic_u8string();
        const std::string projectIdentity(reinterpret_cast<const char*>(project.data()), project.size());
        result.path = source;
        result.size = std::filesystem::file_size(source);
        result.modified = std::filesystem::last_write_time(source);
        result.identity = projectIdentity + "\n" + record.id.ToReference() + "\n" +
            record.relativePath.lexically_normal().generic_string() + "\n" +
            std::to_string(result.size) + "\n" + std::to_string(result.modified.time_since_epoch().count()) +
            "\nv1-rgba8-existing-display-96";

        std::array<wchar_t, 32768> local{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));

        if(length == 0 || length >= local.size() || result.identity.size() > kMaximumIdentity)
        {
            return result;
        }

        const std::filesystem::path root(local.data());

        if(!root.is_absolute() || !SafePath(root))
        {
            return result;
        }

        result.directory = root / "Engine" / "TextureThumbnails" / Hash(projectIdentity);
        result.entry = result.directory / (Hash(result.identity) + ".thumb");
    }
    catch(...)
    {
        // Source identity remains useful even when the optional cache bucket is unavailable.
        result.directory.clear();
        result.entry.clear();
    }

    return result;
}

bool TextureThumbnailDiskCache::Load(const Source& source, Image& image) noexcept
{
    try
    {
        if(source.entry.empty() || !SafePath(source.entry) || !Unchanged(source))
        {
            return false;
        }

        const auto size = std::filesystem::file_size(source.entry);

        if(size > 64 + kMaximumIdentity + kMaximumPixels)
        {
            return false;
        }

        DirectoryLock directories;

        if(!directories.Open(source.directory, false))
        {
            return false;
        }

        std::vector<unsigned char> data(static_cast<size_t>(size));
        HANDLE file = CreateFileW(source.entry.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

        if(file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        BY_HANDLE_FILE_INFORMATION info{};
        DWORD read = 0;
        const bool complete = GetFileInformationByHandle(file, &info) &&
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
            info.nFileSizeHigh == 0 && info.nFileSizeLow == size &&
            ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == size;
        CloseHandle(file);

        if(!complete || data.size() < 64)
        {
            return false;
        }

        std::array<std::uint64_t, 8> header{};
        memcpy(header.data(), data.data(), sizeof(header));

        // DXGI_FORMAT_R8G8B8A8_UNORM = 28, independent of native struct padding.
        if(header[0] != kMagic || header[1] != kVersion || header[2] != 28)
        {
            return false;
        }

        if(header[3] == 0 || header[3] > 96 || header[4] == 0 || header[4] > 96)
        {
            return false;
        }

        const auto bytes = header[3] * header[4] * 4;

        if(header[5] != bytes || header[6] != source.identity.size() ||
           size != sizeof(header) + header[6] + bytes)
        {
            return false;
        }

        const std::string identity(reinterpret_cast<const char*>(data.data() + sizeof(header)),
            static_cast<size_t>(header[6]));
        Image candidate;
        candidate.width = static_cast<unsigned int>(header[3]);
        candidate.height = static_cast<unsigned int>(header[4]);
        candidate.pixels.assign(data.begin() + sizeof(header) + static_cast<size_t>(header[6]), data.end());

        if(identity != source.identity || Hash(candidate.pixels.data(), candidate.pixels.size()) != header[7] ||
           !Unchanged(source))
        {
            return false;
        }

        image = std::move(candidate);
        return true;
    }
    catch(...)
    {
        return false;
    }
}

void TextureThumbnailDiskCache::Store(const Source& source, const Image& image) noexcept
{
    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE writer = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary;
    DirectoryLock directories;

    try
    {
        if(source.entry.empty() || !Unchanged(source) || image.width == 0 || image.width > 96)
        {
            return;
        }

        if(image.height == 0 || image.height > 96 || image.pixels.size() != image.width * image.height * 4)
        {
            return;
        }

        if(!directories.Open(source.directory, true))
        {
            return;
        }

        // Serialize pruning/publication across Engine instances without waiting.
        writer = CreateFileW((source.directory / "writer.lock").c_str(), GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION writerInfo{};

        if(writer == INVALID_HANDLE_VALUE)
        {
            return;
        }

        if(!GetFileInformationByHandle(writer, &writerInfo) ||
           (writerInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            CloseHandle(writer);
            return;
        }

        if(!Prune(source.directory))
        {
            throw std::runtime_error("Optional thumbnail cache publication unavailable.");
        }

        const auto id = AssetID::Generate();

        if(!id)
        {
            throw std::runtime_error("Optional thumbnail cache publication unavailable.");
        }

        temporary = source.directory / (id->ToString() + ".tmp");
        output = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

        if(output == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Optional thumbnail cache publication unavailable.");
        }

        const std::array<std::uint64_t, 8> header{kMagic, kVersion, 28, image.width, image.height,
            image.pixels.size(), source.identity.size(), Hash(image.pixels.data(), image.pixels.size())};
        std::vector<unsigned char> data(sizeof(header) + source.identity.size() + image.pixels.size());
        memcpy(data.data(), header.data(), sizeof(header));
        memcpy(data.data() + sizeof(header), source.identity.data(), source.identity.size());
        memcpy(data.data() + sizeof(header) + source.identity.size(), image.pixels.data(), image.pixels.size());
        DWORD written = 0;
        const bool complete = WriteFile(output, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size() && FlushFileBuffers(output);
        CloseHandle(output);
        output = INVALID_HANDLE_VALUE;

        if(complete && SafePath(source.directory) && Unchanged(source))
        {
            // Replacement changes the directory entry, never writes through a destination link.
            MoveFileExW(temporary.c_str(), source.entry.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
    catch(...)
    {
        // Best-effort cache: ordinary thumbnail creation remains available.
    }

    if(writer != INVALID_HANDLE_VALUE)
    {
        CloseHandle(writer);
    }

    if(output != INVALID_HANDLE_VALUE)
    {
        CloseHandle(output);
    }

    try
    {
        if(!temporary.empty() && SafePath(temporary))
        {
            DeleteFileW(temporary.c_str());
        }
    }
    catch(...)
    {
        // Failure to clean a cache temporary file is nonfatal.
    }
}
