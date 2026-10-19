#include "Project/PersonalSettingsFile.h"

#include "Platform/Windows/AtomicFileWrite.h"

#include <Windows.h>

#include <fstream>
#include <system_error>

namespace
{
    constexpr std::uintmax_t kMaximumBytes = 1024 * 1024;
}

namespace PersonalSettingsFile
{
    const char* Describe(Status status)
    {
        switch(status)
        {
        case Status::Missing:
            return "missing";
        case Status::Loaded:
            return "loaded";
        case Status::Malformed:
            return "malformed";
        case Status::IdentityMismatch:
            return "belongs to a different project";
        case Status::UnsupportedVersion:
            return "written by a newer Engine version";
        case Status::Unreadable:
            return "unreadable";
        }

        return "unknown";
    }

    bool MayReplace(Status status)
    {
        return status != Status::UnsupportedVersion && status != Status::Unreadable;
    }

    Status Read(const std::filesystem::path& path, std::string& contents, std::string& errorMessage)
    {
        contents.clear();
        errorMessage.clear();
        std::error_code error;
        const auto fileStatus = std::filesystem::status(path, error);

        if(fileStatus.type() == std::filesystem::file_type::not_found)
        {
            return Status::Missing;
        }

        if(error || !std::filesystem::is_regular_file(fileStatus))
        {
            errorMessage = error ? error.message() : "not a regular file";
            return Status::Unreadable;
        }

        const auto size = std::filesystem::file_size(path, error);

        if(error || size > kMaximumBytes)
        {
            errorMessage = error ? error.message() : "larger than 1 MiB";
            return Status::Unreadable;
        }

        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            errorMessage = "cannot open the file";
            return Status::Unreadable;
        }

        contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

        if(file.bad())
        {
            errorMessage = "read failed";
            contents.clear();
            return Status::Unreadable;
        }

        return Status::Loaded;
    }

    bool PreserveInvalid(const std::filesystem::path& path, std::string& errorMessage)
    {
        errorMessage.clear();
        std::error_code error;

        if(!std::filesystem::exists(path, error))
        {
            return true;
        }

        auto preserved = path;
        preserved += ".invalid";

        if(!MoveFileExW(path.c_str(), preserved.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorMessage = "Cannot preserve the invalid settings file: " +
                std::system_category().message(static_cast<int>(GetLastError()));
            return false;
        }

        return true;
    }

    bool Save(
        const std::filesystem::path& path,
        Status loadedStatus,
        const std::string& contents,
        std::string& errorMessage)
    {
        errorMessage.clear();

        if(!MayReplace(loadedStatus))
        {
            errorMessage = std::string("The existing settings file was ") + Describe(loadedStatus) +
                "; it is left unchanged.";
            return false;
        }

        if(loadedStatus == Status::Malformed || loadedStatus == Status::IdentityMismatch)
        {
            if(!PreserveInvalid(path, errorMessage))
            {
                return false;
            }
        }

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);

        if(error)
        {
            errorMessage = "Cannot create the settings directory: " + error.message();
            return false;
        }

        return AtomicFileWrite::Write(path, contents, errorMessage);
    }
}
