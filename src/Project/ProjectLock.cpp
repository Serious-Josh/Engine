#include "Project/ProjectLock.h"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    // Far beyond any real manifest; locking past end-of-file is permitted and blocks no reads.
    constexpr DWORD kLockOffsetHigh = 0x7FFFFFFF;
    constexpr DWORD kLockOffsetLow = 0;
    constexpr DWORD kLockLength = 1;

    constexpr LONGLONG kMaximumManifestBytes = 1024 * 1024;

    // A virus scanner or indexer can hold the manifest open for a moment without delete sharing.
    constexpr int kReplaceAttempts = 3;
    constexpr DWORD kReplaceRetryMilliseconds = 50;

    constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    // The pin shares read and write like the pre-PR.9 lock handle did, but never delete.
    constexpr DWORD kSharePin = FILE_SHARE_READ | FILE_SHARE_WRITE;

    std::string SystemMessage(DWORD error)
    {
        return std::system_category().message(static_cast<int>(error));
    }

    OVERLAPPED LockRange()
    {
        OVERLAPPED range{};
        range.Offset = kLockOffsetLow;
        range.OffsetHigh = kLockOffsetHigh;
        return range;
    }

    bool LockHandle(HANDLE handle)
    {
        OVERLAPPED range = LockRange();
        return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, kLockLength, 0, &range);
    }

    void CloseIfOpen(void*& handle) noexcept
    {
        if(handle)
        {
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    bool SameFile(HANDLE first, HANDLE second)
    {
        FILE_ID_INFO firstID{};
        FILE_ID_INFO secondID{};

        if(!GetFileInformationByHandleEx(first, FileIdInfo, &firstID, sizeof(firstID)) ||
            !GetFileInformationByHandleEx(second, FileIdInfo, &secondID, sizeof(secondID)))
        {
            return false;
        }

        return firstID.VolumeSerialNumber == secondID.VolumeSerialNumber &&
            std::memcmp(&firstID.FileId, &secondID.FileId, sizeof(firstID.FileId)) == 0;
    }

#if defined(ENGINE_HAS_VERIFICATION) || defined(PROJECT_LOCK_FAULT_INJECTION)
    int g_injectedPinFailures = 0;
#endif

    // Opens a pin on the manifest path and confirms it is the file the lock handle holds.
    HANDLE OpenPin(const std::filesystem::path& manifestPath, HANDLE lockHandle, DWORD& error)
    {
#if defined(ENGINE_HAS_VERIFICATION) || defined(PROJECT_LOCK_FAULT_INJECTION)
        if(g_injectedPinFailures > 0)
        {
            --g_injectedPinFailures;
            error = ERROR_ACCESS_DENIED;
            return nullptr;
        }
#endif

        const HANDLE pin = CreateFileW(manifestPath.c_str(), GENERIC_READ, kSharePin, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);

        if(pin == INVALID_HANDLE_VALUE)
        {
            error = GetLastError();
            return nullptr;
        }

        if(!SameFile(pin, lockHandle))
        {
            CloseHandle(pin);
            error = ERROR_FILE_INVALID;
            return nullptr;
        }

        error = ERROR_SUCCESS;
        return pin;
    }

    bool PathNamesFile(const std::filesystem::path& path, HANDLE handle)
    {
        const HANDLE current = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);

        if(current == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const bool same = SameFile(current, handle);
        CloseHandle(current);
        return same;
    }

    // The replacement manifest until it has been published. Closing without Publish() deletes it.
    class ReplacementFile
    {
    public:
        ~ReplacementFile()
        {
            Discard();
        }

        bool Create(const std::filesystem::path& directory, std::string& errorMessage)
        {
            wchar_t temporaryName[MAX_PATH]{};

            // Same directory keeps the rename on one volume. The name is not *.engineproj, so it is
            // never mistaken for a second manifest of this project.
            if(GetTempFileNameW(directory.c_str(), L"epj", 0, temporaryName) == 0)
            {
                errorMessage = "Cannot create a temporary file beside the project manifest: " +
                    SystemMessage(GetLastError());
                return false;
            }

            m_path = temporaryName;
            m_writeHandle = CreateFileW(m_path.c_str(), GENERIC_WRITE | DELETE, kShareAll, nullptr,
                TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            if(m_writeHandle == INVALID_HANDLE_VALUE)
            {
                m_writeHandle = nullptr;
                errorMessage = "Cannot open the temporary manifest: " + SystemMessage(GetLastError());
                return false;
            }

            return true;
        }

        bool WriteAndFlush(std::string_view contents, std::string& errorMessage)
        {
            std::size_t offset = 0;

            while(offset < contents.size())
            {
                DWORD bytesWritten = 0;
                const auto remaining = static_cast<DWORD>(contents.size() - offset);

                if(!WriteFile(m_writeHandle, contents.data() + offset, remaining, &bytesWritten, nullptr) ||
                    bytesWritten == 0)
                {
                    errorMessage = "Cannot write the temporary manifest: " + SystemMessage(GetLastError());
                    return false;
                }

                offset += bytesWritten;
            }

            if(!FlushFileBuffers(m_writeHandle))
            {
                errorMessage = "Cannot flush the temporary manifest: " + SystemMessage(GetLastError());
                return false;
            }

            return true;
        }

        // Locks the replacement before it becomes visible at the manifest path.
        bool Lock(std::string& errorMessage)
        {
            m_lockHandle = CreateFileW(m_path.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);

            if(m_lockHandle == INVALID_HANDLE_VALUE)
            {
                m_lockHandle = nullptr;
                errorMessage = "Cannot open the temporary manifest for locking: " + SystemMessage(GetLastError());
                return false;
            }

            if(!LockHandle(m_lockHandle))
            {
                errorMessage = "Cannot lock the temporary manifest: " + SystemMessage(GetLastError());
                return false;
            }

            return true;
        }

        // POSIX semantics replace the manifest even though this process's lock handle on it is
        // still open (that handle shares delete). Any other open handle without delete sharing,
        // such as an older Editor probing the lock, makes the rename fail rather than wait.
        DWORD RenameOver(const std::filesystem::path& manifestPath)
        {
            const std::wstring target = manifestPath.wstring();
            const auto nameBytes = static_cast<DWORD>(target.size() * sizeof(wchar_t));
            std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t));

            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
            rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            rename->RootDirectory = nullptr;
            rename->FileNameLength = nameBytes;
            std::memcpy(rename->FileName, target.c_str(), nameBytes);

            if(!SetFileInformationByHandle(m_writeHandle, FileRenameInfoEx, rename, static_cast<DWORD>(buffer.size())))
            {
                return GetLastError();
            }

            return ERROR_SUCCESS;
        }

        // After a successful rename: the write handle is no longer needed and the caller takes
        // the locked handle, which now names the manifest.
        void* Publish() noexcept
        {
            CloseIfOpen(m_writeHandle);
            m_path.clear();
            return std::exchange(m_lockHandle, nullptr);
        }

    private:
        void Discard() noexcept
        {
            CloseIfOpen(m_lockHandle);

            if(m_writeHandle)
            {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                SetFileInformationByHandle(m_writeHandle, FileDispositionInfo, &disposition, sizeof(disposition));
                CloseIfOpen(m_writeHandle);
            }
            else if(!m_path.empty())
            {
                DeleteFileW(m_path.c_str());
            }

            m_path.clear();
        }

        std::filesystem::path m_path;
        void* m_writeHandle = nullptr;
        void* m_lockHandle = nullptr;
    };

    bool IsRetryableReplaceError(DWORD error)
    {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
    }

    bool IsUnsupportedReplaceError(DWORD error)
    {
        return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
            error == ERROR_INVALID_FUNCTION;
    }
}

ProjectLock::~ProjectLock()
{
    Release();
}

ProjectLock::ProjectLock(ProjectLock&& other) noexcept
    : m_lockHandle(std::exchange(other.m_lockHandle, nullptr))
    , m_pinHandle(std::exchange(other.m_pinHandle, nullptr))
    , m_manifestPath(std::move(other.m_manifestPath))
{
}

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept
{
    if(this != &other)
    {
        Release();
        m_lockHandle = std::exchange(other.m_lockHandle, nullptr);
        m_pinHandle = std::exchange(other.m_pinHandle, nullptr);
        m_manifestPath = std::move(other.m_manifestPath);
    }

    return *this;
}

ProjectLock ProjectLock::Acquire(
    const std::filesystem::path& manifestPath,
    Result& result,
    std::string& errorMessage)
{
    errorMessage.clear();
    ProjectLock lock;

    const HANDLE handle = CreateFileW(
        manifestPath.c_str(),
        GENERIC_READ,
        kShareAll,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if(handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();

        if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            result = Result::NotFound;
            errorMessage = "Project manifest does not exist.";
        }
        else if(error == ERROR_ACCESS_DENIED)
        {
            result = Result::AccessDenied;
            errorMessage = "Access to the project manifest was denied.";
        }
        else if(error == ERROR_SHARING_VIOLATION)
        {
            result = Result::Failed;
            errorMessage = "The project manifest is in use by another program: " + SystemMessage(error);
        }
        else
        {
            result = Result::Failed;
            errorMessage = "Cannot open the project manifest: " + SystemMessage(error);
        }

        return lock;
    }

    lock.m_lockHandle = handle;

    if(!LockHandle(handle))
    {
        const DWORD error = GetLastError();
        lock.Release();

        if(error == ERROR_LOCK_VIOLATION)
        {
            result = Result::AlreadyLocked;
            errorMessage = "The project is already open in another Engine Editor.";
        }
        else
        {
            result = Result::Failed;
            errorMessage = "Cannot lock the project: " + SystemMessage(error);
        }

        return lock;
    }

    // Pin after locking: the pin must name the same file object the lock was taken on.
    DWORD pinError = ERROR_SUCCESS;
    lock.m_pinHandle = OpenPin(manifestPath, handle, pinError);

    if(!lock.m_pinHandle)
    {
        lock.Release();
        result = Result::Failed;

        if(pinError == ERROR_SHARING_VIOLATION)
        {
            errorMessage = "The project manifest is in use by another program: " + SystemMessage(pinError);
        }
        else if(pinError == ERROR_FILE_INVALID)
        {
            errorMessage = "The project manifest was replaced while it was being opened.";
        }
        else
        {
            errorMessage = "Cannot open the project manifest: " + SystemMessage(pinError);
        }

        return lock;
    }

    lock.m_manifestPath = manifestPath;
    result = Result::Acquired;
    return lock;
}

bool ProjectLock::IsHeld() const noexcept
{
    return m_lockHandle != nullptr;
}

bool ProjectLock::ReadManifest(std::string& contents, std::string& errorMessage) const
{
    contents.clear();
    errorMessage.clear();

    if(!m_lockHandle)
    {
        errorMessage = "The project lock is not held.";
        return false;
    }

    LARGE_INTEGER size{};

    if(!GetFileSizeEx(m_lockHandle, &size))
    {
        errorMessage = "Cannot read the project manifest size: " + SystemMessage(GetLastError());
        return false;
    }

    if(size.QuadPart > kMaximumManifestBytes)
    {
        errorMessage = "The project manifest is larger than the 1 MiB limit.";
        return false;
    }

    LARGE_INTEGER start{};

    if(!SetFilePointerEx(m_lockHandle, start, nullptr, FILE_BEGIN))
    {
        errorMessage = "Cannot read the project manifest: " + SystemMessage(GetLastError());
        return false;
    }

    contents.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;

    while(offset < contents.size())
    {
        DWORD bytesRead = 0;
        const auto remaining = static_cast<DWORD>(contents.size() - offset);

        if(!ReadFile(m_lockHandle, contents.data() + offset, remaining, &bytesRead, nullptr))
        {
            errorMessage = "Cannot read the project manifest: " + SystemMessage(GetLastError());
            contents.clear();
            return false;
        }

        if(bytesRead == 0)
        {
            break;
        }

        offset += bytesRead;
    }

    contents.resize(offset);
    return true;
}

ProjectLock::Publication ProjectLock::ReplaceManifest(std::string_view contents, std::string& errorMessage)
{
    errorMessage.clear();

    if(!m_lockHandle || !m_manifestPath.is_absolute())
    {
        errorMessage = "The project lock is not held.";
        return Publication::NotPublished;
    }

    if(static_cast<LONGLONG>(contents.size()) > kMaximumManifestBytes)
    {
        errorMessage = "The project manifest would exceed the 1 MiB limit.";
        return Publication::NotPublished;
    }

    // Revalidate the destination: write only with full ownership of the file at the manifest path.
    if(!IsFullyOwned() && !RestoreOwnership(errorMessage))
    {
        errorMessage = "Project ownership cannot be verified; the setting was not saved. " + errorMessage;
        return Publication::NotPublished;
    }

    ReplacementFile replacement;

    if(!replacement.Create(m_manifestPath.parent_path(), errorMessage) ||
        !replacement.WriteAndFlush(contents, errorMessage) ||
        !replacement.Lock(errorMessage))
    {
        return Publication::NotPublished;
    }

    // The pin forbids delete sharing, which would also block this process's own replacement.
    // The byte-range lock stays held on the old manifest while the pin is closed.
    CloseIfOpen(m_pinHandle);

    DWORD renameError = ERROR_SUCCESS;

    for(int attempt = 0; attempt < kReplaceAttempts; ++attempt)
    {
        renameError = replacement.RenameOver(m_manifestPath);

        if(renameError == ERROR_SUCCESS || !IsRetryableReplaceError(renameError))
        {
            break;
        }

        Sleep(kReplaceRetryMilliseconds);
    }

    if(renameError != ERROR_SUCCESS)
    {
        if(IsUnsupportedReplaceError(renameError))
        {
            errorMessage = "The project's file system cannot safely replace a manifest that is open; "
                "the setting was not saved (" + SystemMessage(renameError) + ").";
        }
        else
        {
            errorMessage = "Cannot replace the project manifest; the original is unchanged: " +
                SystemMessage(renameError);
        }

        // OpenPin verifies identity: a file put at the path while unpinned is never adopted.
        DWORD pinError = ERROR_SUCCESS;
        m_pinHandle = OpenPin(m_manifestPath, m_lockHandle, pinError);

        if(!m_pinHandle)
        {
            errorMessage += " Project ownership could not be re-verified: " + SystemMessage(pinError);
        }

        return Publication::NotPublished;
    }

    // The locked replacement now names the manifest. The old lock handle refers to the unlinked
    // previous file; closing it releases nothing another Editor could observe.
    void* replacementLock = replacement.Publish();
    CloseIfOpen(m_lockHandle);
    m_lockHandle = replacementLock;

    DWORD pinError = ERROR_SUCCESS;
    m_pinHandle = OpenPin(m_manifestPath, m_lockHandle, pinError);

    if(!m_pinHandle)
    {
        // Published and still locked, but external rename/delete protection is missing.
        errorMessage = "The project manifest was saved, but its ownership could not be re-verified: " +
            SystemMessage(pinError);
        return Publication::PublishedUnprotected;
    }

    return Publication::Published;
}

bool ProjectLock::IsFullyOwned() const
{
    return m_lockHandle && m_pinHandle && SameFile(m_pinHandle, m_lockHandle) &&
        PathNamesFile(m_manifestPath, m_lockHandle);
}

bool ProjectLock::RestoreOwnership(std::string& errorMessage)
{
    errorMessage.clear();

    if(!m_lockHandle || !m_manifestPath.is_absolute())
    {
        errorMessage = "The project lock is not held.";
        return false;
    }

    if(IsFullyOwned())
    {
        return true;
    }

    // The path must still name the locked file; otherwise the lock protects an obsolete file and
    // ownership of whatever now sits at the manifest path is not claimed.
    if(!PathNamesFile(m_manifestPath, m_lockHandle))
    {
        errorMessage = "The project manifest was moved, replaced or deleted outside the Editor.";
        return false;
    }

    CloseIfOpen(m_pinHandle);
    DWORD pinError = ERROR_SUCCESS;
    m_pinHandle = OpenPin(m_manifestPath, m_lockHandle, pinError);

    if(!m_pinHandle)
    {
        errorMessage = "Cannot protect the project manifest: " + SystemMessage(pinError);
        return false;
    }

    return true;
}

#if defined(ENGINE_HAS_VERIFICATION) || defined(PROJECT_LOCK_FAULT_INJECTION)
void ProjectLock::InjectPinFailuresForTesting(int count)
{
    g_injectedPinFailures = count;
}
#endif

void ProjectLock::Release() noexcept
{
    // Closing the lock handle releases the byte-range lock; no explicit UnlockFileEx is required.
    CloseIfOpen(m_pinHandle);
    CloseIfOpen(m_lockHandle);
    m_manifestPath.clear();
}
