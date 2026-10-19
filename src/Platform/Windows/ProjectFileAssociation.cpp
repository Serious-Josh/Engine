#include "Platform/Windows/ProjectFileAssociation.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shlwapi.h>

#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")

namespace ProjectFileAssociation
{
namespace
{
    // Values this Editor writes under its own ProgID key. They identify the owning executable and
    // what to restore on Unregister; nothing outside the ProgID key records Editor state.
    constexpr wchar_t kExecutableValue[] = L"EngineEditorExecutable";
    constexpr wchar_t kPreviousDefaultValue[] = L"EnginePreviousDefault";
    constexpr wchar_t kCreatedExtensionKeyValue[] = L"EngineCreatedExtensionKey";
    constexpr wchar_t kTypeName[] = L"Engine Project";

    std::string SystemMessage(LSTATUS status)
    {
        return std::system_category().message(static_cast<int>(status));
    }

    std::string UTF8(const std::wstring& text)
    {
        if(text.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
            nullptr, nullptr);
        std::string converted(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), converted.data(), size,
            nullptr, nullptr);
        return converted;
    }

    std::string UTF8(const std::filesystem::path& path)
    {
        return UTF8(path.wstring());
    }

    bool SamePath(const std::filesystem::path& first, const std::filesystem::path& second)
    {
        const auto left = first.lexically_normal().native();
        const auto right = second.lexically_normal().native();
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool SameText(const std::wstring& first, const std::wstring& second)
    {
        return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    std::wstring ExtensionKey(const std::wstring& classes)
    {
        return classes + L"\\" + kExtension;
    }

    std::wstring ProgIDKey(const std::wstring& classes)
    {
        return classes + L"\\" + kProgID;
    }

    // Reads a string value. Missing keys or values are not errors: status is ERROR_FILE_NOT_FOUND
    // and value is empty.
    LSTATUS ReadString(HKEY root, const std::wstring& subkey, const wchar_t* name, std::wstring& value)
    {
        value.clear();
        DWORD bytes = 0;
        LSTATUS status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);

        if(status != ERROR_SUCCESS)
        {
            return status;
        }

        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes);

        if(status == ERROR_SUCCESS)
        {
            value = buffer.data();
        }

        return status;
    }

    bool ReadDword(HKEY root, const std::wstring& subkey, const wchar_t* name, DWORD& value)
    {
        DWORD bytes = sizeof(value);
        return RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
    }

    bool IsMissing(LSTATUS status)
    {
        return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    }

    LSTATUS WriteString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value)
    {
        const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        return RegSetKeyValueW(root, subkey.c_str(), name, REG_SZ, value.c_str(), bytes);
    }

    LSTATUS WriteDword(HKEY root, const std::wstring& subkey, const wchar_t* name, DWORD value)
    {
        return RegSetKeyValueW(root, subkey.c_str(), name, REG_DWORD, &value, sizeof(value));
    }

    LSTATUS DeleteValue(HKEY root, const std::wstring& subkey, const wchar_t* name)
    {
        const LSTATUS status = RegDeleteKeyValueW(root, subkey.c_str(), name);
        return IsMissing(status) ? ERROR_SUCCESS : status;
    }

    bool KeyExists(HKEY root, const std::wstring& subkey)
    {
        HKEY key = nullptr;

        if(RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        RegCloseKey(key);
        return true;
    }

    // Deletes a key only when it has no values and no subkeys, so nothing unrelated is lost.
    void DeleteKeyIfEmpty(HKEY root, const std::wstring& subkey)
    {
        HKEY key = nullptr;

        if(RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return;
        }

        DWORD subkeyCount = 0;
        DWORD valueCount = 0;
        const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeyCount, nullptr, nullptr,
            &valueCount, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(key);

        if(status == ERROR_SUCCESS && subkeyCount == 0 && valueCount == 0)
        {
            RegDeleteKeyW(root, subkey.c_str());
        }
    }

    HKEY MachineRoot(const RegistryLocation& location)
    {
        return location.machineClassesUnderLocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    }

    void NotifyShell(const RegistryLocation& location)
    {
        if(location.notifyShell)
        {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }
    }

    // Reads the protected per-user choice. Windows 11 may keep it under UserChoiceLatest.
    std::wstring ReadUserChoice(const RegistryLocation& location)
    {
        const auto extension = location.userFileExts + L"\\" + kExtension;
        std::wstring progID;

        if(ReadString(HKEY_CURRENT_USER, extension + L"\\UserChoiceLatest", L"ProgId", progID) == ERROR_SUCCESS &&
            !progID.empty())
        {
            return progID;
        }

        ReadString(HKEY_CURRENT_USER, extension + L"\\UserChoice", L"ProgId", progID);
        return progID;
    }
}

RegistryLocation CurrentUser()
{
    RegistryLocation location;
    location.userClasses = L"Software\\Classes";
    location.userFileExts = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
    location.machineClasses = L"Software\\Classes";
    location.machineClassesUnderLocalMachine = true;
    location.notifyShell = true;
    return location;
}

std::wstring BuildOpenCommand(const std::filesystem::path& executable)
{
    return L"\"" + executable.wstring() + L"\" \"%1\"";
}

Status ReadStatus(const RegistryLocation& location, const std::filesystem::path& executable)
{
    Status status;
    std::wstring registered;
    const LSTATUS executableStatus = ReadString(HKEY_CURRENT_USER, ProgIDKey(location.userClasses),
        kExecutableValue, registered);

    if(executableStatus != ERROR_SUCCESS && !IsMissing(executableStatus))
    {
        status.state = State::Unreadable;
        status.errorMessage = "Cannot read the Engine file association: " + SystemMessage(executableStatus);
        return status;
    }

    status.registeredExecutable = registered;

    const LSTATUS defaultStatus = ReadString(HKEY_CURRENT_USER, ExtensionKey(location.userClasses), nullptr,
        status.userDefaultProgID);

    if(defaultStatus != ERROR_SUCCESS && !IsMissing(defaultStatus))
    {
        status.state = State::Unreadable;
        status.errorMessage = "Cannot read the .engineproj association: " + SystemMessage(defaultStatus);
        return status;
    }

    ReadString(MachineRoot(location), ExtensionKey(location.machineClasses), nullptr, status.machineDefaultProgID);
    status.userChoiceProgID = ReadUserChoice(location);

    const bool userChoiceIsOther = !status.userChoiceProgID.empty() && !SameText(status.userChoiceProgID, kProgID);
    const bool userDefaultIsOther = !status.userDefaultProgID.empty() && !SameText(status.userDefaultProgID, kProgID);

    if(!registered.empty())
    {
        std::error_code error;

        if(!SamePath(status.registeredExecutable, executable))
        {
            const bool exists = std::filesystem::is_regular_file(status.registeredExecutable, error);
            status.state = exists ? State::RegisteredToOtherEditor : State::StaleRegistration;
        }
        else if(userChoiceIsOther)
        {
            status.state = State::OverriddenByUserChoice;
        }
        else if(userDefaultIsOther)
        {
            status.state = State::DefaultChangedByOtherProgram;
        }
        else
        {
            status.state = State::Registered;
        }

        return status;
    }

    const bool machineDefaultIsOther = !status.machineDefaultProgID.empty() &&
        !SameText(status.machineDefaultProgID, kProgID);
    status.state = userChoiceIsOther || userDefaultIsOther || machineDefaultIsOther ?
        State::OtherProgram :
        State::NotRegistered;
    return status;
}

std::filesystem::path QueryEffectiveExecutable()
{
    DWORD length = 0;

    if(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, kExtension, L"open", nullptr, &length) !=
        S_FALSE || length == 0)
    {
        return {};
    }

    std::wstring executable(length, L'\0');

    if(FAILED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, kExtension, L"open",
        executable.data(), &length)))
    {
        return {};
    }

    executable.resize(wcslen(executable.c_str()));
    return executable;
}

std::filesystem::path CurrentExecutable()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));

    if(length == 0 || length >= executable.size())
    {
        return {};
    }

    executable.resize(length);
    return executable;
}

bool IsTemporaryExecutable(const std::filesystem::path& executable)
{
    wchar_t temporary[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, temporary);

    if(length == 0 || length > MAX_PATH)
    {
        return false;
    }

    std::error_code error;
    const auto temporaryRoot = std::filesystem::weakly_canonical(temporary, error);
    const auto candidate = std::filesystem::weakly_canonical(executable, error);
    auto candidateComponent = candidate.begin();

    for(const auto& rootComponent : temporaryRoot)
    {
        if(rootComponent.empty())
        {
            continue;
        }

        if(candidateComponent == candidate.end() || !SamePath(*candidateComponent, rootComponent))
        {
            return false;
        }

        ++candidateComponent;
    }

    return true;
}

ChangeResult Register(
    const RegistryLocation& location,
    const std::filesystem::path& executable,
    bool replaceExisting,
    std::string& message)
{
    message.clear();
    std::error_code error;

    if(!executable.is_absolute() || !std::filesystem::is_regular_file(executable, error))
    {
        message = "The Editor executable to register does not exist: " + UTF8(executable);
        return ChangeResult::Refused;
    }

    if(IsTemporaryExecutable(executable))
    {
        message = "This Editor runs from the temporary directory and cannot be registered as the permanent "
            ".engineproj handler: " + UTF8(executable);
        return ChangeResult::Refused;
    }

    const Status status = ReadStatus(location, executable);

    if(status.state == State::Unreadable)
    {
        message = status.errorMessage;
        return ChangeResult::Failed;
    }

    const bool userDefaultIsOther = !status.userDefaultProgID.empty() && !SameText(status.userDefaultProgID, kProgID);
    const bool otherEditor = status.state == State::RegisteredToOtherEditor || status.state == State::StaleRegistration;
    const bool otherProgram = userDefaultIsOther || status.state == State::OtherProgram;

    if((otherProgram || otherEditor) && !replaceExisting)
    {
        message = otherEditor ?
            "Project files are registered to another Editor: " + UTF8(status.registeredExecutable) + "." :
            Describe(status);
        message += " Confirm to make this Editor the per-user handler; the previous per-user choice is recorded "
            "so Unregister can restore it.";
        return ChangeResult::NeedsConfirmation;
    }

    const auto progID = ProgIDKey(location.userClasses);
    const auto extension = ExtensionKey(location.userClasses);
    const bool progIDExisted = KeyExists(HKEY_CURRENT_USER, progID);
    const bool extensionExisted = KeyExists(HKEY_CURRENT_USER, extension);
    const auto command = BuildOpenCommand(executable);
    const auto icon = L"\"" + executable.wstring() + L"\",0";

    // The ProgID is complete before .engineproj points at it, so Explorer never sees a half-written
    // handler. Re-registering keeps the originally recorded previous default.
    LSTATUS result = WriteString(HKEY_CURRENT_USER, progID, nullptr, kTypeName);

    if(result == ERROR_SUCCESS && userDefaultIsOther)
    {
        result = WriteString(HKEY_CURRENT_USER, progID, kPreviousDefaultValue, status.userDefaultProgID);
    }

    if(result == ERROR_SUCCESS && !extensionExisted)
    {
        result = WriteDword(HKEY_CURRENT_USER, progID, kCreatedExtensionKeyValue, 1);
    }

    if(result == ERROR_SUCCESS)
    {
        result = WriteString(HKEY_CURRENT_USER, progID + L"\\DefaultIcon", nullptr, icon);
    }

    if(result == ERROR_SUCCESS)
    {
        result = WriteString(HKEY_CURRENT_USER, progID + L"\\shell\\open\\command", nullptr, command);
    }

    if(result == ERROR_SUCCESS)
    {
        result = WriteString(HKEY_CURRENT_USER, progID, kExecutableValue, executable.wstring());
    }

    if(result == ERROR_SUCCESS)
    {
        result = RegSetKeyValueW(HKEY_CURRENT_USER, (extension + L"\\OpenWithProgids").c_str(), kProgID,
            REG_NONE, nullptr, 0);
    }

    if(result == ERROR_SUCCESS)
    {
        result = WriteString(HKEY_CURRENT_USER, extension, nullptr, kProgID);
    }

    if(result != ERROR_SUCCESS)
    {
        // Undo only what this call created; an existing registration is left as it was.
        if(!progIDExisted)
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, progID.c_str());
        }

        message = "Registering .engineproj failed: " + SystemMessage(result);
        return ChangeResult::Failed;
    }

    NotifyShell(location);
    message = "Registered .engineproj for this Windows user with " + UTF8(executable) + ".";

    if(!status.userChoiceProgID.empty() && !SameText(status.userChoiceProgID, kProgID))
    {
        message += " Windows still opens project files with the program chosen in Settings > Apps > Default apps "
            "(or \"Open with > Always\"); choose Engine Project there to use this Editor.";
    }

    return ChangeResult::Changed;
}

ChangeResult Unregister(
    const RegistryLocation& location,
    const std::filesystem::path& executable,
    std::string& message)
{
    message.clear();
    const Status status = ReadStatus(location, executable);

    if(status.state == State::Unreadable)
    {
        message = status.errorMessage;
        return ChangeResult::Failed;
    }

    if(status.registeredExecutable.empty())
    {
        message = "No Engine Editor registration exists for this user; nothing was changed.";
        return ChangeResult::Unchanged;
    }

    if(status.state == State::RegisteredToOtherEditor)
    {
        message = "Project files are registered to another Editor that still exists: " +
            UTF8(status.registeredExecutable) + ". Unregister from that Editor; nothing was changed.";
        return ChangeResult::Refused;
    }

    const auto progID = ProgIDKey(location.userClasses);
    const auto extension = ExtensionKey(location.userClasses);

    std::wstring previousDefault;
    ReadString(HKEY_CURRENT_USER, progID, kPreviousDefaultValue, previousDefault);
    DWORD createdExtensionKey = 0;
    ReadDword(HKEY_CURRENT_USER, progID, kCreatedExtensionKeyValue, createdExtensionKey);

    LSTATUS result = ERROR_SUCCESS;

    // Restore the per-user default only while it still points at this registration; a program
    // that took over since then keeps its choice.
    if(SameText(status.userDefaultProgID, kProgID))
    {
        result = previousDefault.empty() ?
            DeleteValue(HKEY_CURRENT_USER, extension, nullptr) :
            WriteString(HKEY_CURRENT_USER, extension, nullptr, previousDefault);
    }

    if(result == ERROR_SUCCESS)
    {
        result = DeleteValue(HKEY_CURRENT_USER, extension + L"\\OpenWithProgids", kProgID);
    }

    if(result == ERROR_SUCCESS)
    {
        const LSTATUS deleted = RegDeleteTreeW(HKEY_CURRENT_USER, progID.c_str());
        result = IsMissing(deleted) ? ERROR_SUCCESS : deleted;
    }

    if(result != ERROR_SUCCESS)
    {
        message = "Unregistering .engineproj failed: " + SystemMessage(result);
        return ChangeResult::Failed;
    }

    DeleteKeyIfEmpty(HKEY_CURRENT_USER, extension + L"\\OpenWithProgids");

    if(createdExtensionKey == 1)
    {
        DeleteKeyIfEmpty(HKEY_CURRENT_USER, extension);
    }

    NotifyShell(location);
    message = status.state == State::StaleRegistration ?
        "Removed the stale registration for the missing Editor " + UTF8(status.registeredExecutable) + "." :
        "Removed this Editor's .engineproj registration.";

    if(!previousDefault.empty() && SameText(status.userDefaultProgID, kProgID))
    {
        message += " Restored the previous association \"" + UTF8(previousDefault) + "\".";
    }

    return ChangeResult::Changed;
}

std::string Describe(const Status& status)
{
    switch(status.state)
    {
    case State::NotRegistered:
        return "Not registered. Double-clicking .engineproj files does not open this Editor.";
    case State::Registered:
        return "Registered to this Editor for the current Windows user.";
    case State::OverriddenByUserChoice:
        return "Registered to this Editor, but Windows uses \"" + UTF8(status.userChoiceProgID) +
            "\" chosen in Settings. Choose Engine Project in Settings > Apps > Default apps to use this Editor.";
    case State::DefaultChangedByOtherProgram:
        return "Registered to this Editor, but another program (\"" + UTF8(status.userDefaultProgID) +
            "\") has since become the per-user default.";
    case State::RegisteredToOtherEditor:
        return "Registered to another Editor: " + UTF8(status.registeredExecutable) + ".";
    case State::StaleRegistration:
        return "Registered to an Editor that no longer exists: " + UTF8(status.registeredExecutable) +
            ". Register this Editor or remove the stale registration.";
    case State::OtherProgram:
    {
        // Report the selection Windows gives precedence to.
        std::wstring program = status.machineDefaultProgID;

        if(!status.userChoiceProgID.empty())
        {
            program = status.userChoiceProgID;
        }
        else if(!status.userDefaultProgID.empty())
        {
            program = status.userDefaultProgID;
        }

        return "Another program is associated with .engineproj (\"" + UTF8(program) + "\").";
    }
    case State::Unreadable:
        return status.errorMessage;
    }

    return {};
}
}
