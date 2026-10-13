#pragma once
#include "Renderer/Texture.h"
#include <filesystem>
#include <string>

class ResourceUploader;
class DescriptorHeap;

enum class ProjectTextureInterpretation
{
    SRGBColor,
    LinearData,
    NormalMap
};

// Load an already resolver-validated path. Success proves upload completion;
// caller owns the texture, its transitions, and checked retirement.
bool LoadProjectTexture(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& heap,
    const std::filesystem::path& path,
    ProjectTextureInterpretation interpretation,
    Texture& texture,
    std::string* errorMessage);
