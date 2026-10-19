#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Exclusive, process-lifetime claim on a project, backed by the operating system.
//
// The lock is a byte-range lock far beyond the end of the project's .engineproj file, taken on a
// handle this object owns. Windows releases it when the handle closes, including when the owning
// process exits or crashes, so no stale lock file can strand a project. The manifest's content is
// never locked, so other processes can still read it. Because the lock belongs to the file
// object rather than to a path string, the same project reached through a different path,
// letter case or junction still conflicts, and a relocated project carries no lock state.
//
// Two handles are held (PR.9):
// - the lock handle carries the byte-range lock and shares delete access, so this process can
//   atomically replace the manifest it holds (ReplaceManifest);
// - the pin handle carries no lock and does not share delete access, so no other process can
//   rename, replace or delete the manifest (which would silently detach the lock) while an
//   Editor holds the project. Only ReplaceManifest closes it, briefly, for its own replacement.
//
// Editors built before PR.9 open the manifest without delete sharing and take the same byte-range
// lock, so old and new Editors exclude each other in both acquisition orders.
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

    enum class Publication
    {
        // Replacement published; the new manifest is locked and pinned (full ownership).
        Published,

        // Replacement published and locked, but its pin could not be restored: other Editors are
        // still excluded, external rename/delete protection is lost. Callers must fail closed.
        PublishedUnprotected,

        // Nothing published; the original manifest and its lock are kept. Check IsFullyOwned():
        // the original pin may not have been restored.
        NotPublished
    };

    // Atomically replaces the held manifest with contents and keeps the project locked throughout.
    //
    // The replacement is written and flushed to a same-directory temporary file, which is locked
    // before it is renamed over the manifest with POSIX replacement semantics. At every instant the
    // file at the manifest path is locked by this process, so no other Editor can claim the project.
    // Refuses to write unless full ownership is held or can first be restored. Fails without
    // writing on file systems that cannot replace an open file (e.g. FAT, many network shares).
    Publication ReplaceManifest(std::string_view contents, std::string& errorMessage);

    // Lock and pin are held, and both the pin and the manifest path name the locked file.
    bool IsFullyOwned() const;

    // Reopens a missing pin, only after verifying that the manifest path still names the file the
    // byte-range lock protects. A lock on an obsolete file is never treated as ownership.
    bool RestoreOwnership(std::string& errorMessage);

#if defined(ENGINE_HAS_VERIFICATION) || defined(PROJECT_LOCK_FAULT_INJECTION)
    // Tests only: the next count pin openings fail as if access were denied.
    static void InjectPinFailuresForTesting(int count);
#endif

    void Release() noexcept;

private:
    void* m_lockHandle = nullptr;
    void* m_pinHandle = nullptr;
    std::filesystem::path m_manifestPath;
};
