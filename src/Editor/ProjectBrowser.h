#pragma once

#include "Editor/EditorPreferences.h"

#include <array>
#include <optional>

// UI drafts and requests only. Engine remains the owner of project and personal state.
class ProjectBrowser
{
public:
    enum class Action { Open, Create, Locate, Remove, Cancel };
    struct Request
    {
        Action action;
        std::filesystem::path path;
        std::filesystem::path previousPath;
        std::string name;
        std::string expectedProjectID;
    };

    std::optional<Request> Draw(const EditorPreferences& preferences, void* window, bool creationOnly = false);
    std::string message;

private:
    std::array<char, 256> m_name{};
    std::array<char, 4096> m_destination{};
    bool m_initialized = false;
    // Advisory display cache only. Every Open/Locate still uses the authoritative loader.
    std::vector<RecentProject> m_recentSnapshot;
    std::vector<bool> m_recentAvailable;
    double m_nextRecentProbe = 0;
};
