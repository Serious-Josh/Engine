#pragma once

#include "assets/ImportedMesh.h"

#include <filesystem>
#include <vector>

class ModelImporter
{
    public:
        static std::vector<ImportedMesh> Load(const std::filesystem::path& path);
};