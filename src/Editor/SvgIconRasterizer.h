#pragma once

#include "Renderer/ImageLoader.h"
#include <filesystem>
#include <string>

// CPU-only, local COM scope. Requires Windows 10 Creators Update's Direct2D SVG API.
// Outputs white RGB + straight alpha coverage, suitable for ImGui tinting.
bool RasterizeEditorSvg(const std::filesystem::path& path, unsigned int maximumSize,
    ImageData& image, std::string& error);
