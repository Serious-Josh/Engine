#pragma once

#include <filesystem>
#include <string>

// Opt-in, per-user Windows registration that opens .engineproj files in this Editor (PR.9).
//
// Only HKEY_CURRENT_USER\Software\Classes is written, so no administrator rights are needed and
// machine-wide associations are never touched. Windows' protected UserChoice selection is read
// but never written: when the user has chosen another program there, Windows keeps using it and
// the user must change the default in Settings. A successful registration therefore does not
// prove Explorer will open projects with this Editor; QueryEffectiveExecutable reports what
// Windows actually resolves.
//
// Registration is a personal Windows preference, not a project setting, and is never performed
// automatically: only explicit Register/Unregister calls change the registry.
namespace ProjectFileAssociation
{
    inline constexpr wchar_t kExtension[] = L".engineproj";
    inline constexpr wchar_t kProgID[] = L"Engine.EditorProject";

    // Registry key locations under HKEY_CURRENT_USER. Production uses CurrentUser(); tests pass
    // isolated keys so they never alter the real associations.
    struct RegistryLocation
    {
        std::wstring userClasses;
        std::wstring userFileExts;

        // Machine-wide classes are only read. Under HKEY_LOCAL_MACHINE in production; under
        // HKEY_CURRENT_USER for isolated tests.
        std::wstring machineClasses;
        bool machineClassesUnderLocalMachine = true;

        // Production notifies the shell after a change; isolated tests do not.
        bool notifyShell = true;
    };

    RegistryLocation CurrentUser();

    enum class State
    {
        NotRegistered,
        Registered,

        // Registered to this Editor, but Windows' protected UserChoice selects another program.
        OverriddenByUserChoice,

        // Registered to this Editor, but the per-user default was later changed to another program.
        DefaultChangedByOtherProgram,

        // This ProgID points to a different Editor executable that still exists.
        RegisteredToOtherEditor,

        // This ProgID points to an Editor executable that no longer exists.
        StaleRegistration,

        // Not registered by any Editor; another program is associated with .engineproj.
        OtherProgram,

        Unreadable
    };

    struct Status
    {
        State state = State::NotRegistered;

        // Executable recorded by this ProgID; empty when no registration exists.
        std::filesystem::path registeredExecutable;

        std::wstring userDefaultProgID;
        std::wstring machineDefaultProgID;
        std::wstring userChoiceProgID;
        std::string errorMessage;
    };

    Status ReadStatus(const RegistryLocation& location, const std::filesystem::path& executable);

    // "<executable>" "%1": both quoted, so spaces and Unicode in either path are passed intact.
    std::wstring BuildOpenCommand(const std::filesystem::path& executable);

    // The program Windows currently resolves for opening .engineproj files (real registry only).
    std::filesystem::path QueryEffectiveExecutable();

    // This process's executable path.
    std::filesystem::path CurrentExecutable();

    // True for executables under the user's temporary directory, which must never become the
    // permanent handler (diagnostic and scratch copies).
    bool IsTemporaryExecutable(const std::filesystem::path& executable);

    enum class ChangeResult
    {
        Changed,
        Unchanged,
        NeedsConfirmation,
        Refused,
        Failed
    };

    // Registers executable as the per-user handler. Replacing another program's per-user default,
    // or another Editor's registration, requires replaceExisting; the replaced per-user default is
    // recorded so Unregister can restore it.
    ChangeResult Register(
        const RegistryLocation& location,
        const std::filesystem::path& executable,
        bool replaceExisting,
        std::string& message);

    // Removes this Editor's registration and restores the recorded previous per-user default when
    // .engineproj still points at this registration. Refuses to remove a registration that belongs
    // to another Editor executable that still exists. Unrelated keys and values are left alone.
    ChangeResult Unregister(
        const RegistryLocation& location,
        const std::filesystem::path& executable,
        std::string& message);

    // One-line user-facing description of a status.
    std::string Describe(const Status& status);
}
