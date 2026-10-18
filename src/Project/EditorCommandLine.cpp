#include "Project/EditorCommandLine.h"

#include "Project/ProjectManifest.h"
#include "assets/ProjectAssetRegistry.h"

#include <utility>

namespace
{
    bool IsProjectFileArgument(const std::wstring& argument)
    {
        const std::filesystem::path path(argument);
        return ProjectAssetRegistry::PathsEqual(path.extension(), ProjectManifest::kFileExtension);
    }

    std::string ArgumentText(const std::wstring& argument)
    {
        const auto utf8 = std::filesystem::path(argument).u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    EditorCommandLine Invalid(std::string message)
    {
        EditorCommandLine result;
        result.kind = EditorCommandLine::Kind::Invalid;
        result.errorMessage = std::move(message);
        return result;
    }
}

EditorCommandLine EditorCommandLine::Parse(const std::vector<std::wstring>& arguments)
{
    EditorCommandLine result;

    if(arguments.size() <= 1)
    {
        return result;
    }

    const bool developerOption = arguments[1].starts_with(L"--");

    if(developerOption)
    {
        for(std::size_t index = 2; index < arguments.size(); ++index)
        {
            if(IsProjectFileArgument(arguments[index]))
            {
                return Invalid("A project file cannot be combined with the option " + ArgumentText(arguments[1]) +
                    ". Launch Engine.exe with only the .engineproj path.");
            }
        }

        result.kind = Kind::DeveloperOption;
        return result;
    }

    for(std::size_t index = 1; index < arguments.size(); ++index)
    {
        const auto& argument = arguments[index];

        if(!IsProjectFileArgument(argument))
        {
            return Invalid("Unrecognized argument \"" + ArgumentText(argument) +
                "\". Usage: Engine.exe [path\\to\\Project.engineproj]");
        }

        if(!result.projectManifest.empty())
        {
            return Invalid("More than one project file was given (\"" + ArgumentText(result.projectManifest.native()) +
                "\" and \"" + ArgumentText(argument) + "\"). Open one project at a time.");
        }

        result.projectManifest = std::filesystem::path(argument);
    }

    result.kind = Kind::ExplicitProject;
    return result;
}
