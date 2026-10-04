#pragma once

#include <filesystem>
#include <vector>

struct ImageData
{
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int channels = 0;

    std::vector<unsigned char> pixels;
};

struct HDRImageData
{
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int channels = 0;

    std::vector<float> pixels;
};

class ImageLoader
{
    public:
        static bool Load(const std::filesystem::path& path, ImageData& image);
        static bool LoadHDR(const std::filesystem::path& path, HDRImageData& image);
};