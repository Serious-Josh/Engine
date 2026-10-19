#pragma once

#include "Project/ProjectLoader.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ProjectCreation
{
    // Destination is the parent directory. Never adopts or overwrites an existing child.
    bool Validate(const std::string& name, const std::filesystem::path& destination, std::string& error);
    std::optional<OpenedProject> Create(
        const std::string& name, const std::filesystem::path& destination, std::string& error);
}
