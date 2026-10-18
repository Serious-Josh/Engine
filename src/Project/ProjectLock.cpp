#include "Project/ProjectLock.h"

#include <Windows.h>

#include <system_error>
#include <utility>

namespace
{
    // Far beyond any real manifest; locking past end-of-file is permitted and blocks no reads.
    constexpr DWORD kLockOffsetHigh = 0x7FFFFFFF;
    constexpr DWORD kLockOffsetLow = 0;
    constexpr DWORD kLockLength = 1;

    constexpr LONGLONG kMaximumManifestBytes = 1024 * 1024;

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
}

ProjectLock::~ProjectLock()
{
    Release();
}

ProjectLock::ProjectLock(ProjectLock&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr))
{
}

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept
{
    if(this != &other)
    {
        Release();
        m_handle = std::exchange(other.m_handle, nullptr);
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
        FILE_SHARE_READ | FILE_SHARE_WRITE,
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

    lock.m_handle = handle;
    OVERLAPPED range = LockRange();

    if(!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, kLockLength, 0, &range))
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

    result = Result::Acquired;
    return lock;
}

bool ProjectLock::IsHeld() const noexcept
{
    return m_handle != nullptr;
}

bool ProjectLock::ReadManifest(std::string& contents, std::string& errorMessage) const
{
    contents.clear();
    errorMessage.clear();

    if(!m_handle)
    {
        errorMessage = "The project lock is not held.";
        return false;
    }

    LARGE_INTEGER size{};

    if(!GetFileSizeEx(m_handle, &size))
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

    if(!SetFilePointerEx(m_handle, start, nullptr, FILE_BEGIN))
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

        if(!ReadFile(m_handle, contents.data() + offset, remaining, &bytesRead, nullptr))
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

void ProjectLock::Release() noexcept
{
    if(!m_handle)
    {
        return;
    }

    // Closing the handle releases the byte-range lock; no explicit UnlockFileEx is required.
    CloseHandle(m_handle);
    m_handle = nullptr;
}
