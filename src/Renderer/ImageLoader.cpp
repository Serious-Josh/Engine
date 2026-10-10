#include "ImageLoader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
    float SRGBToLinear(float value)
    {
        if(value <= 0.04045f)
        {
            return value / 12.92f;
        }

        return std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

    float LinearToSRGB(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);

        if(value <= 0.0031308f)
        {
            return value * 12.92f;
        }

        return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    }

    unsigned char FloatToByte(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);

        return static_cast<unsigned char>(std::round(value * 255.0f));
    }
}

bool ImageLoader::Load(const std::filesystem::path& path, ImageData& image)
{
    int width = 0;
    int height = 0;
    int channels = 0;

    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);

    if(!pixels){
        return false;
    }

    image.width = static_cast<unsigned int>(width);
    image.height = static_cast<unsigned int>(height);
    image.channels = 4;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

    image.pixels.assign(pixels, pixels + pixelCount);

    stbi_image_free(pixels);

    return true;
}

bool ImageLoader::LoadHDR(const std::filesystem::path& path, HDRImageData& image){
    int width = 0;
    int height = 0;
    int channels = 0;

    float* pixels = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);

    if(!pixels){
        return false;
    }

    image.width = static_cast<unsigned int>(width);
    image.height = static_cast<unsigned int>(height);
    image.channels = 4;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

    image.pixels.assign(pixels, pixels + pixelCount);

    stbi_image_free(pixels);

    return true;
}

bool ImageLoader::GenerateMipChain(
    const ImageData& source,
    MipFilterMode filterMode,
    ImageMipChain& output)
{
    output.levels.clear();

    if(source.width == 0 || source.height == 0)
    {
        return false;
    }

    const size_t width = source.width;
    const size_t height = source.height;

    if(width > (std::numeric_limits<size_t>::max)() / height / 4)
    {
        return false;
    }

    const size_t expectedSize = width * height * 4;

    if(source.pixels.size() != expectedSize)
    {
        return false;
    }

    ImageMipLevel baseLevel{};
    baseLevel.width = source.width;
    baseLevel.height = source.height;
    baseLevel.pixels = source.pixels;

    output.levels.push_back(std::move(baseLevel));

    while(output.levels.back().width > 1 || output.levels.back().height > 1)
    {
        const ImageMipLevel& previous = output.levels.back();

        ImageMipLevel next{};
        next.width = std::max(1u, previous.width / 2);
        next.height = std::max(1u, previous.height / 2);
        next.pixels.resize(static_cast<size_t>(next.width) * next.height * 4);

        for(unsigned int y = 0; y < next.height; ++y)
        {
            for(unsigned int x = 0; x < next.width; ++x)
            {
                float accumulated[4] = {};
                unsigned int sampleCount = 0;

                for(unsigned int dy = 0; dy < 2; ++dy)
                {
                    for(unsigned int dx = 0; dx < 2; ++dx)
                    {
                        const unsigned int sx = std::min(x * 2 + dx, previous.width - 1);

                        const unsigned int sy = std::min(y * 2 + dy, previous.height - 1);

                        const size_t sourceIndex = (static_cast<size_t>(sy) * previous.width + sx) * 4;

                        const unsigned char* pixel = previous.pixels.data() + sourceIndex;

                        if(filterMode == MipFilterMode::NormalMap)
                        {
                            accumulated[0] += pixel[0] / 255.0f * 2.0f - 1.0f;
                            accumulated[1] += pixel[1] / 255.0f * 2.0f - 1.0f;
                            accumulated[2] += pixel[2] / 255.0f * 2.0f - 1.0f;
                        }
                        else
                        {
                            for(unsigned int channel = 0; channel < 3; ++channel)
                            {
                                float value = pixel[channel] / 255.0f;

                                if(filterMode == MipFilterMode::SRGBColor)
                                {
                                    value = SRGBToLinear(value);
                                }

                                accumulated[channel] += value;
                            }
                        }

                        accumulated[3] += pixel[3] / 255.0f;
                        ++sampleCount;
                    }
                }

                const float inverseCount = 1.0f / sampleCount;

                for(float& value : accumulated)
                {
                    value *= inverseCount;
                }

                if(filterMode == MipFilterMode::NormalMap)
                {
                    const float lengthSquared =
                        accumulated[0] * accumulated[0] +
                        accumulated[1] * accumulated[1] +
                        accumulated[2] * accumulated[2];

                    if(lengthSquared > 1e-12f)
                    {
                        const float inverseLength = 1.0f / std::sqrt(lengthSquared);

                        accumulated[0] *= inverseLength;
                        accumulated[1] *= inverseLength;
                        accumulated[2] *= inverseLength;
                    }
                    else
                    {
                        accumulated[0] = 0.0f;
                        accumulated[1] = 0.0f;
                        accumulated[2] = 1.0f;
                    }

                    for(unsigned int channel = 0; channel < 3; ++channel)
                    {
                        accumulated[channel] = accumulated[channel] * 0.5f + 0.5f;
                    }
                }
                else if(filterMode == MipFilterMode::SRGBColor)
                {
                    for(unsigned int channel = 0; channel < 3; ++channel)
                    {
                        accumulated[channel] = LinearToSRGB(accumulated[channel]);
                    }
                }

                const size_t destinationIndex = (static_cast<size_t>(y) * next.width + x) * 4;

                for(unsigned int channel = 0; channel < 4; ++channel)
                {
                    next.pixels[destinationIndex + channel] = FloatToByte(accumulated[channel]);
                }
            }
        }

        output.levels.push_back(std::move(next));
    }

    return true;
}