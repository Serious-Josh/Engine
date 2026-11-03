#include "Scripting/ScriptApiExport.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace Scripting
{
namespace
{
std::optional<std::string> ReadExisting(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);

    if(!stream)
    {
        return std::nullopt;
    }

    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool WriteAll(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    return !stream.fail();
}
}

DeclarationFileStatus WriteDeclarationFile(const std::filesystem::path& path, std::string_view text,
    std::string& error)
{
    std::error_code status;

    // The language server only discovers files with this exact name; any other name is almost certainly a mistake.
    if(path.filename() != "as.predefined")
    {
        error = "The output file must be named as.predefined: " + path.string();
        return DeclarationFileStatus::Refused;
    }

    const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");

    if(!std::filesystem::is_directory(directory, status))
    {
        error = "The output directory does not exist: " + directory.string();
        return DeclarationFileStatus::Refused;
    }

    if(std::filesystem::exists(path, status))
    {
        if(!std::filesystem::is_regular_file(path, status))
        {
            error = "The output path exists and is not a regular file: " + path.string();
            return DeclarationFileStatus::Refused;
        }

        const std::optional<std::string> existing = ReadExisting(path);

        if(!existing)
        {
            error = "Cannot read the existing file: " + path.string();
            return DeclarationFileStatus::Failed;
        }

        // A hand-written as.predefined belongs to someone; only files the exporter generated are replaced.
        if(!existing->starts_with(GeneratedDeclarationsMarker))
        {
            error = "Refusing to replace an as.predefined that ScriptApiExport did not generate: " + path.string();
            return DeclarationFileStatus::Refused;
        }

        if(*existing == text)
        {
            return DeclarationFileStatus::Unchanged;
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";

    // Whatever already occupies the temporary path is not ours to truncate or delete (a stale file from an
    // interrupted export is left for the developer to remove).
    if(std::filesystem::exists(temporary, status) || status)
    {
        error = "The temporary path is already in use; remove it and retry: " + temporary.string();
        return DeclarationFileStatus::Refused;
    }

    if(!WriteAll(temporary, text))
    {
        std::filesystem::remove(temporary, status);
        error = "Cannot write " + temporary.string();
        return DeclarationFileStatus::Failed;
    }

    std::filesystem::rename(temporary, path, status);

    if(status)
    {
        error = "Cannot replace " + path.string() + ": " + status.message();
        std::filesystem::remove(temporary, status);
        return DeclarationFileStatus::Failed;
    }

    return DeclarationFileStatus::Written;
}
}
