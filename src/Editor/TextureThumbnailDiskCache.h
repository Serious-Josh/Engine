#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class ProjectPaths;
struct AssetRecord;

namespace TextureThumbnailDiskCache
{
    struct Image
    {
        unsigned int width = 0;
        unsigned int height = 0;

        std::vector<unsigned char> pixels;
    };

    struct Source
    {
        std::filesystem::path path;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};

        std::string identity;
        std::filesystem::path directory;
        std::filesystem::path entry;
    };

    // Optional cache only: exceptions and filesystem failures never prevent decoding.
    Source Describe(const ProjectPaths& paths, const AssetRecord& record, const std::filesystem::path& source);
    bool Load(const Source& source, Image& image) noexcept;
    void Store(const Source& source, const Image& image) noexcept;
}
