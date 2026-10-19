#include "Platform/Windows/AtomicFileWrite.h"

#include <Windows.h>

#include <system_error>
#include <utility>

namespace
{
    std::string SystemMessage(DWORD error)
    {
        return std::system_category().message(static_cast<int>(error));
    }

    std::string FileNameText(const std::filesystem::path& path)
    {
        const auto utf8 = path.filename().u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // Owns the temporary file until it has replaced the destination.
    class TemporaryFile
    {
    public:
        explicit TemporaryFile(std::filesystem::path path)
            : m_path(std::move(path))
        {
        }

        ~TemporaryFile()
        {
            if(!m_path.empty())
            {
                DeleteFileW(m_path.c_str());
            }
        }

        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        const std::filesystem::path& Path() const noexcept
        {
            return m_path;
        }

        void Release() noexcept
        {
            m_path.clear();
        }

    private:
        std::filesystem::path m_path;
    };

    bool WriteAndFlush(const std::filesystem::path& path, std::string_view contents, std::string& errorMessage)
    {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);

        if(handle == INVALID_HANDLE_VALUE)
        {
            errorMessage = "Cannot open the temporary file: " + SystemMessage(GetLastError());
            return false;
        }

        bool written = true;
        std::size_t offset = 0;

        while(written && offset < contents.size())
        {
            DWORD bytesWritten = 0;
            const auto remaining = static_cast<DWORD>(contents.size() - offset);
            written = WriteFile(handle, contents.data() + offset, remaining, &bytesWritten, nullptr) && bytesWritten > 0;
            offset += bytesWritten;
        }

        if(!written)
        {
            errorMessage = "Cannot write the temporary file: " + SystemMessage(GetLastError());
        }
        else if(!FlushFileBuffers(handle))
        {
            errorMessage = "Cannot flush the temporary file: " + SystemMessage(GetLastError());
            written = false;
        }

        CloseHandle(handle);
        return written;
    }
}

namespace AtomicFileWrite
{
    bool Write(
        const std::filesystem::path& destination,
        std::string_view contents,
        std::string& errorMessage)
    {
        errorMessage.clear();
        const auto directory = destination.parent_path();

        if(destination.empty() || directory.empty())
        {
            errorMessage = "An absolute destination path is required.";
            return false;
        }

        // GetTempFileNameW creates the unique temporary file in the destination directory, so
        // the final rename stays on one volume and is atomic.
        wchar_t temporaryName[MAX_PATH]{};

        if(GetTempFileNameW(directory.c_str(), L"eng", 0, temporaryName) == 0)
        {
            errorMessage = "Cannot create a temporary file beside " + FileNameText(destination) + ": " +
                SystemMessage(GetLastError());
            return false;
        }

        TemporaryFile temporary{std::filesystem::path(temporaryName)};

        if(!WriteAndFlush(temporary.Path(), contents, errorMessage))
        {
            return false;
        }

        if(!MoveFileExW(temporary.Path().c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorMessage = "Cannot replace " + FileNameText(destination) + ": " + SystemMessage(GetLastError());
            return false;
        }

        temporary.Release();
        return true;
    }
}
