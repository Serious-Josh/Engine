#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Classifies the Editor's command line before any Engine work starts.
//
// Rules, applied to the UTF-16 arguments after the executable:
// - No arguments: open the default project (unchanged legacy startup).
// - First argument starts with "--": a developer/diagnostic option. The existing option
//   dispatch in main.cpp handles it unchanged. A .engineproj argument anywhere in such a
//   command line is rejected, because no option accepts a project file.
// - Otherwise every argument must be a path ending in .engineproj (case-insensitive). Exactly
//   one opens that project explicitly. More than one, or any other positional argument, is
//   rejected rather than ignored.
struct EditorCommandLine
{
    enum class Kind
    {
        DefaultProject,
        ExplicitProject,
        DeveloperOption,
        Invalid
    };

    Kind kind = Kind::DefaultProject;

    // Set for ExplicitProject; built directly from UTF-16, with no code page conversion.
    std::filesystem::path projectManifest;

    // Set for Invalid.
    std::string errorMessage;

    static EditorCommandLine Parse(const std::vector<std::wstring>& arguments);
};
