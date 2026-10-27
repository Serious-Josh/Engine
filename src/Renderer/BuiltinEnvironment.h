#pragma once

#include "Renderer/ImageLoader.h"

// Same equirectangular format and dimensions as the built-in neutral source.
// Normal environment preprocessing supplies every downstream cube/array binding.
inline HDRImageData MakeBlackEnvironmentImage()
{
    HDRImageData image;
    image.width = 16;
    image.height = 8;
    image.channels = 4;
    image.pixels.assign(16 * 8 * 4, 0.0f);
    for(size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 1.0f;
    return image;
}
