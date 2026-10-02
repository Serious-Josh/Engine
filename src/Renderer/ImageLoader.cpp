#include "ImageLoader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool ImageLoader::LoadHDR(const std::filesystem::path& path, ImageData& image){
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