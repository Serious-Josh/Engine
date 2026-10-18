#pragma once

#include "Project/ProjectContext.h"
#include "Project/ProjectLock.h"

#include <filesystem>
#include <optional>
#include <string>

// A validated project and the lock that protects it. Engine takes ownership of both; the lock
// must live exactly as long as the Editor works in the project.
struct OpenedProject
{
    ProjectContext context;
    ProjectLock lock;
};

namespace ProjectLoader
{
    // Opens an explicitly requested .engineproj for writable Editor use.
    //
    // Order: lock the manifest file, read it through the locked handle, parse it strictly,
    // then validate the project directory and every content path against the canonical
    // project root. Nothing is created or written. On failure no lock is retained and
    // errorMessage explains the problem; the caller must not fall back to another project.
    std::optional<OpenedProject> Open(
        const std::filesystem::path& manifestPath,
        std::string& errorMessage);
}
