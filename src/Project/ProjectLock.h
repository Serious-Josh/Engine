#pragma once

#include <filesystem>
#include <string>

// Exclusive, process-lifetime claim on a project, backed by the operating system.
//
// The lock is a byte-range lock far beyond the end of the project's .engineproj file, taken on a
// handle this object owns. Windows releases it when the handle closes, including when the owning
// process exits or crashes, so no stale lock file can strand a project. The manifest's content is
// never locked, so other processes can still read it. Because the lock belongs to the file
// object rather than to a path string, the same project reached through a different path,
// letter case or junction still conflicts, and a relocated project carries no lock state.
//
// The handle does not share delete access, so the manifest cannot be renamed, replaced or
// deleted (which would silently detach the lock) while an Editor holds the project.
class ProjectLock
{
public:
    enum class Result
    {
        Acquired,
        AlreadyLocked,
        NotFound,
        AccessDenied,
        Failed
    };

    ProjectLock() = default;
    ~ProjectLock();

    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;
    ProjectLock(ProjectLock&& other) noexcept;
    ProjectLock& operator=(ProjectLock&& other) noexcept;

    // Opens the manifest and takes the lock without waiting. On any result other than
    // Acquired the returned object holds nothing and errorMessage describes the failure.
    static ProjectLock Acquire(
        const std::filesystem::path& manifestPath,
        Result& result,
        std::string& errorMessage);

    bool IsHeld() const noexcept;

    // Reads the whole manifest through the locked handle, so the bytes validated are the
    // bytes of the file this process holds.
    bool ReadManifest(std::string& contents, std::string& errorMessage) const;

    void Release() noexcept;

private:
    void* m_handle = nullptr;
};
