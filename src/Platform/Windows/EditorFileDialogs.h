#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace EditorFileDialogs
{
    enum class Kind { OpenScene, SaveScene, OpenProject };
    // Empty result with no error is cancellation. Uses UTF-16 filesystem paths throughout.
    std::optional<std::filesystem::path> Select(
        Kind kind, void* owner, const std::filesystem::path& directory, std::string& error);
    bool ConfirmOverwrite(void* owner, const std::filesystem::path& path);

    // A launched Editor process; the handle is owned by whoever holds this value.
    struct ChildEditor
    {
        void* process = nullptr;
        unsigned long processID = 0;
    };

    // Process creation only; does not assert that the child initialized its project. When child is
    // supplied it receives the process handle (release it with CloseChild); otherwise it is closed.
    bool Relaunch(const std::filesystem::path& manifest, std::string& error, ChildEditor* child = nullptr);
    bool Launch(const std::filesystem::path& executable, const std::filesystem::path& manifest, std::string& error,
        ChildEditor* child = nullptr);

    enum class ChildState
    {
        Starting,

        // The child shows a top-level window. An Editor opens its project (and takes the project
        // lock) before it creates its window, so this confirms the child owns the project.
        WindowShown,

        Exited
    };

    ChildState PollChild(const ChildEditor& child, unsigned long& exitCode);
    void CloseChild(ChildEditor& child);
}
