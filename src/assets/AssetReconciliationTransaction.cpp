#include "AssetReconciliationTransaction.h"
#include "Project/ProjectPaths.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    // The published limits (AssetReconciliationTransaction.h).
    constexpr size_t maxFileBytes = AssetReconciliationTransaction::MaximumFileBytes;
    constexpr size_t maxTargets = AssetReconciliationTransaction::MaximumTargets;
    constexpr size_t maxTransactionBytes = AssetReconciliationTransaction::MaximumTransactionBytes;
    const fs::path transactionName = ".asset-reconciliation";

#if defined(_DEBUG)
    // W1.3 fault injection: a simulated crash is never caught as an ordinary failure, so nothing is rolled back or cleaned.
    struct SimulatedCrash : std::runtime_error
    {
        SimulatedCrash() : std::runtime_error("Simulated crash (fault injection).") {}
    };

    AssetReconciliationTransaction::FaultStage g_faultStage = AssetReconciliationTransaction::FaultStage::None;
    size_t g_faultIndex = 0;

    void Fault(AssetReconciliationTransaction::FaultStage stage, size_t index)
    {
        if(g_faultStage == stage && g_faultIndex == index)
        {
            g_faultStage = AssetReconciliationTransaction::FaultStage::None;
            throw SimulatedCrash();
        }
    }

#define TRANSACTION_FAULT(stage, index) Fault(AssetReconciliationTransaction::FaultStage::stage, (index))
#else
#define TRANSACTION_FAULT(stage, index) ((void)0)
#endif

    std::string Digest(const std::string& bytes)
    {
        unsigned char hash[32]{};
        const auto status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), hash, sizeof(hash));

        if(status < 0)
        {
            throw std::runtime_error("Cannot hash transaction data.");
        }

        std::string result;
        constexpr char hex[] = "0123456789abcdef";

        for(const auto value : hash)
        {
            result += hex[value >> 4];
            result += hex[value & 15];
        }

        return result;
    }

    struct Handle
    {
        HANDLE value = INVALID_HANDLE_VALUE;

        explicit Handle(HANDLE handle) : value(handle) {}
        ~Handle()
        {
            if(value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    void Require(bool success, const std::string& operation)
    {
        if(!success)
        {
            throw std::runtime_error(operation + " (Windows error " + std::to_string(GetLastError()) + ").");
        }
    }

    std::unique_ptr<Handle> Open(const fs::path& path, DWORD access, DWORD disposition)
    {
        auto result = std::make_unique<Handle>(CreateFileW(path.c_str(), access, 0, nullptr,
            disposition, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        Require(result->value != INVALID_HANDLE_VALUE, "Cannot exclusively open " + path.string());
        BY_HANDLE_FILE_INFORMATION info{};
        Require(GetFileInformationByHandle(result->value, &info) != FALSE, "Cannot inspect " + path.string());
        Require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            info.nNumberOfLinks == 1, "Not an ordinary unlinked file: " + path.string());
        return result;
    }

    std::string Read(HANDLE file)
    {
        LARGE_INTEGER size{};
        Require(GetFileSizeEx(file, &size) != FALSE, "Cannot read size");
        Require(size.QuadPart >= 0 && size.QuadPart <= maxFileBytes, "Transaction file exceeds size limit");
        LARGE_INTEGER start{};
        Require(SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != FALSE, "Cannot seek");
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        Require(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
            read == bytes.size(), "Cannot read complete file");
        return bytes;
    }

    std::string Read(const fs::path& path)
    {
        const auto file = Open(path, GENERIC_READ, OPEN_EXISTING);
        return Read(file->value);
    }

    void Write(const fs::path& path, const std::string& bytes)
    {
        Require(bytes.size() <= maxFileBytes, "Transaction file exceeds size limit");
        const auto file = Open(path, GENERIC_WRITE, CREATE_NEW);
        DWORD written = 0;
        Require(WriteFile(file->value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
            written == bytes.size(), "Cannot write complete file: " + path.string());
        Require(FlushFileBuffers(file->value) != FALSE, "Cannot flush " + path.string());
    }

    void Rename(HANDLE file, const fs::path& destination)
    {
        const auto name = destination.wstring();
        std::vector<unsigned char> storage(offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        memcpy(info->FileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        Require(SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(storage.size())) != FALSE,
            "Cannot rename to " + destination.string());
    }

    bool Exists(const fs::path& path)
    {
        std::error_code error;
        const auto status = fs::symlink_status(path, error);

        if(error == std::errc::no_such_file_or_directory)
        {
            return false;
        }

        if(error)
        {
            throw std::runtime_error("Cannot inspect " + path.string() + ": " + error.message());
        }

        return fs::exists(status);
    }

    struct Session
    {
        fs::path root;
        fs::path directory;
        std::vector<std::unique_ptr<Handle>> pins;
        std::unique_ptr<Handle> lock;

        // Pin ancestors without share-delete; reject junctions rather than following them.
        void Pin(const fs::path& path)
        {
            fs::path prefix;

            for(const auto& part : path)
            {
                prefix /= part;

                if(!prefix.has_root_directory())
                {
                    continue;
                }

                auto handle = std::make_unique<Handle>(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                Require(handle->value != INVALID_HANDLE_VALUE, "Cannot pin " + prefix.string());
                BY_HANDLE_FILE_INFORMATION info{};
                Require(GetFileInformationByHandle(handle->value, &info) != FALSE &&
                    !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Unsafe directory " + prefix.string());
                pins.push_back(std::move(handle));
            }
        }

        explicit Session(const ProjectPaths& paths)
        {
            Require(paths.GetRoot().is_absolute(), "Project root must be absolute");
            root = paths.GetRoot().lexically_normal();
            Pin(root);
            lock = Open(root / ".asset-reconciliation.lock", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
            directory = root / transactionName;
        }

        fs::path Target(const fs::path& relative)
        {
            Require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name(), "Target must be project-relative");

            for(const auto& part : relative)
            {
                const auto text = part.native();
                Require(part != "." && part != ".." && text.find_first_of(L":*?\"<>|") == std::wstring::npos &&
                    !text.empty() && text.back() != L'.' && text.back() != L' ', "Unsafe target path");
            }

            const auto extension = relative.extension().wstring();
            const bool registry = _wcsicmp(relative.c_str(), L"Assets.registry.json") == 0;
            Require(registry || _wcsicmp(extension.c_str(), L".material") == 0 ||
                _wcsicmp(extension.c_str(), L".scene") == 0, "Unsupported reconciliation target");
            Require(_wcsicmp(relative.begin()->c_str(), transactionName.c_str()) != 0, "Target is inside recovery storage");
            const auto target = root / relative;
            Pin(target.parent_path());
            return target;
        }
    };

    fs::path File(const Session& session, size_t index, const char* suffix)
    {
        return session.directory / (std::to_string(index) + suffix);
    }

    void Remove(const fs::path& path)
    {
        if(Exists(path))
        {
            // Inspect exclusively, and delete that exact file through its handle.
            const auto file = Open(path, DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING);
            FILE_DISPOSITION_INFO disposition{TRUE};
            Require(SetFileInformationByHandle(file->value, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE,
                "Cannot remove recovery file " + path.string());
        }
    }

    void Complete(Session& session, const std::string& state)
    {
        Remove(session.directory / "done.tmp");
        Write(session.directory / "done.tmp", state + "\n");
        Require(MoveFileExW((session.directory / "done.tmp").c_str(), (session.directory / "done").c_str(),
            MOVEFILE_WRITE_THROUGH) != FALSE, "Cannot publish completion marker");
    }

    void Clean(Session& session)
    {
        // Only known private files may be removed. Never recursively delete recovery storage.
        std::vector<fs::path> files;

        for(const auto& entry : fs::directory_iterator(session.directory))
        {
            const auto name = entry.path().filename().string();
            const auto dot = name.find('.');
            const bool numbered = dot != std::string::npos && dot > 0 &&
                name.substr(0, dot).find_first_not_of("0123456789") == std::string::npos;
            const auto suffix = dot == std::string::npos ? std::string{} : name.substr(dot);
            Require(name == "journal.json" || name == "journal.tmp" || name == "done" || name == "done.tmp" ||
                (numbered && (suffix == ".old" || suffix == ".new" || suffix == ".apply" || suffix == ".removed")),
                "Unexpected recovery file " + entry.path().string());

            if(name != "done")
            {
                files.push_back(entry.path());
            }
        }

        for(const auto& file : files)
        {
            Remove(file);
        }

        Remove(session.directory / "done");
        // Release directory pins before deleting the now-empty directory.
        session.pins.clear();
        Require(RemoveDirectoryW(session.directory.c_str()) != FALSE, "Cannot remove recovery directory");
    }

    std::vector<fs::path> Targets(Session& session, const nlohmann::json& journal)
    {
        // Version 2 (W2) adds create targets ("create": true, no original); version 1 journals never have them.
        Require((journal.at("version") == 1 || journal.at("version") == 2) && journal.at("root") == session.root.generic_string(),
            "Journal project/version mismatch");
        const auto& records = journal.at("targets");
        Require(records.is_array() && !records.empty() && records.size() <= maxTargets, "Invalid journal target count");
        std::vector<fs::path> targets;

        for(const auto& record : records)
        {
            const auto target = session.Target(fs::u8path(record.at("path").get<std::string>()));
            Require(record.at("oldHash").is_string() && record.at("newHash").is_string(), "Missing journal digests");
            Require(!record.contains("create") || (record.at("create").is_boolean() && journal.at("version") == 2),
                "Invalid journal create flag");

            for(const auto& existing : targets)
            {
                Require(_wcsicmp(existing.c_str(), target.c_str()) != 0, "Duplicate transaction target");
            }

            targets.push_back(target);
        }

        return targets;
    }

    void Recover(Session& session)
    {
        if(!Exists(session.directory))
        {
            return;
        }

        session.Pin(session.directory);

        if(Exists(session.directory / "done"))
        {
            const auto marker = Read(session.directory / "done");
            Require(marker == "committed\n" || marker == "rolledback\n", "Invalid transaction completion marker");
            Clean(session);
            return;
        }

        if(!Exists(session.directory / "journal.json"))
        {
            // Journal publication precedes every target mutation. Preparation debris is safe to discard.
            for(const auto& entry : fs::directory_iterator(session.directory))
            {
                Require(entry.path().extension() != ".removed", "Missing journal with displaced originals; manual recovery required");
            }

            Clean(session);
            return;
        }

        const auto journal = nlohmann::json::parse(Read(session.directory / "journal.json"));
        const auto targets = Targets(session, journal);
        std::vector<std::string> originals;
        std::vector<bool> created;
        std::vector<std::unique_ptr<Handle>> current;
        size_t total = 0;

        // Validate every recovery input and lock every extant target before touching any target.
        for(size_t index = 0; index < targets.size(); ++index)
        {
            const auto& record = journal.at("targets").at(index);
            created.push_back(record.value("create", false));

            if(created.back())
            {
                // A created target had no original: rollback removes it, and only if it is exactly what was staged.
                const auto replacement = Read(File(session, index, ".new"));
                Require(record.at("oldHash").get<std::string>().empty() && !Exists(File(session, index, ".old")) &&
                    Digest(replacement) == record.at("newHash").get<std::string>(), "Recovery payload checksum mismatch");
                total += replacement.size();
                Require(total <= maxTransactionBytes, "Transaction exceeds aggregate size limit");
                std::unique_ptr<Handle> file;

                if(Exists(targets[index]))
                {
                    file = Open(targets[index], GENERIC_READ | DELETE, OPEN_EXISTING);
                    Require(Read(file->value) == replacement, "Target changed externally: " + targets[index].string());
                }

                if(Exists(File(session, index, ".removed")))
                {
                    Require(Read(File(session, index, ".removed")) == replacement, "Invalid displaced recovery file");
                }

                originals.emplace_back();
                current.push_back(std::move(file));
                continue;
            }

            originals.push_back(Read(File(session, index, ".old")));
            const auto replacement = Read(File(session, index, ".new"));
            Require(Digest(originals.back()) == record.at("oldHash").get<std::string>() &&
                Digest(replacement) == record.at("newHash").get<std::string>(),
                "Recovery payload checksum mismatch");
            total += originals.back().size() + replacement.size();
            Require(total <= maxTransactionBytes, "Transaction exceeds aggregate size limit");
            std::unique_ptr<Handle> file;

            if(Exists(targets[index]))
            {
                file = Open(targets[index], GENERIC_READ | DELETE, OPEN_EXISTING);
                const auto bytes = Read(file->value);
                Require(bytes == originals.back() || bytes == replacement, "Target changed externally: " + targets[index].string());
            }
            else
            {
                Require(Exists(File(session, index, ".removed")), "Missing target without displaced original");
            }

            if(Exists(File(session, index, ".removed")))
            {
                const auto bytes = Read(File(session, index, ".removed"));
                Require(bytes == originals.back() || bytes == replacement, "Invalid displaced recovery file");
            }

            current.push_back(std::move(file));
        }

        std::vector<std::unique_ptr<Handle>> restored;

        for(size_t index = 0; index < targets.size(); ++index)
        {
            if(created[index])
            {
                if(current[index])
                {
                    Remove(File(session, index, ".removed"));
                    Rename(current[index]->value, File(session, index, ".removed"));
                    current[index].reset();
                }

                TRANSACTION_FAULT(DuringRecovery, index);
                continue;
            }

            Remove(File(session, index, ".apply"));
            Write(File(session, index, ".apply"), originals[index]);

            if(current[index])
            {
                Remove(File(session, index, ".removed"));
                Rename(current[index]->value, File(session, index, ".removed"));
            }

            auto apply = Open(File(session, index, ".apply"), DELETE, OPEN_EXISTING);
            Rename(apply->value, targets[index]);
            restored.push_back(std::move(apply));
            current[index].reset();
            TRANSACTION_FAULT(DuringRecovery, index);
        }

        Complete(session, "rolledback");
        restored.clear();
        Clean(session);
    }
}

bool AssetReconciliationTransaction::Recover(const ProjectPaths& paths, std::string& error)
{
    error.clear();

    try
    {
        Session session(paths);
        Recover(session);
        return true;
    }
#if defined(_DEBUG)
    catch(const SimulatedCrash& crash)
    {
        error = crash.what();
        return false;
    }
#endif
    catch(const std::exception& exception)
    {
        error = std::string(exception.what()) + " Recovery retained at: " + (paths.GetRoot() / transactionName).string();
        return false;
    }
}

bool AssetReconciliationTransaction::Commit(
    const ProjectPaths& paths,
    const std::vector<Replacement>& replacements,
    std::string& error)
{
    error.clear();
    bool prepared = false;
    bool committed = false;

    try
    {
        Session session(paths);
        Require(!Exists(session.directory), "Recover the previous transaction before committing");
        Require(!replacements.empty() && replacements.size() <= maxTargets, "Invalid replacement count");
        const bool creates = std::any_of(replacements.begin(), replacements.end(), [](const Replacement& replacement)
        {
            return replacement.create;
        });
        nlohmann::json journal{{"version", creates ? 2 : 1}, {"root", session.root.generic_string()},
            {"targets", nlohmann::json::array()}};

        for(const auto& replacement : replacements)
        {
            Require(replacement.bytes.size() <= maxFileBytes, "Replacement exceeds size limit");
            Require(!replacement.create || !replacement.expectedOriginal, "A created file has no expected original");
            journal["targets"].push_back({{"path", replacement.relativePath.generic_string()}, {"oldHash", ""}, {"newHash", ""}});

            if(replacement.create)
            {
                journal["targets"].back()["create"] = true;
            }
        }

        const auto targets = Targets(session, journal);
        std::vector<std::unique_ptr<Handle>> originals;
        std::vector<std::string> bytes;
        size_t total = 0;

        for(size_t index = 0; index < targets.size(); ++index)
        {
            if(replacements[index].create)
            {
                Require(!Exists(targets[index]), "Destination already exists: " + targets[index].string());
                originals.push_back(nullptr);
                bytes.emplace_back();
                journal["targets"][index]["newHash"] = Digest(replacements[index].bytes);
                total += replacements[index].bytes.size();
                Require(total <= maxTransactionBytes, "Transaction exceeds aggregate size limit");
                continue;
            }

            originals.push_back(Open(targets[index], GENERIC_READ | DELETE, OPEN_EXISTING));
            bytes.push_back(Read(originals.back()->value));
            Require(!replacements[index].expectedOriginal || bytes.back() == *replacements[index].expectedOriginal,
                "File changed since reconciliation preparation: " + targets[index].string());
            journal["targets"][index]["oldHash"] = Digest(bytes.back());
            journal["targets"][index]["newHash"] = Digest(replacements[index].bytes);
            total += bytes.back().size() + replacements[index].bytes.size();
            Require(total <= maxTransactionBytes, "Transaction exceeds aggregate size limit");
        }

        Require(CreateDirectoryW(session.directory.c_str(), nullptr) != FALSE, "Cannot create transaction storage");
        prepared = true;
        session.Pin(session.directory);

        for(size_t index = 0; index < targets.size(); ++index)
        {
            if(!replacements[index].create)
            {
                Write(File(session, index, ".old"), bytes[index]);
            }

            Write(File(session, index, ".new"), replacements[index].bytes);
            Write(File(session, index, ".apply"), replacements[index].bytes);
        }

        TRANSACTION_FAULT(BeforeJournal, 0);
        Write(session.directory / "journal.tmp", journal.dump(2));
        Require(MoveFileExW((session.directory / "journal.tmp").c_str(), (session.directory / "journal.json").c_str(),
            MOVEFILE_WRITE_THROUGH) != FALSE, "Cannot publish transaction journal");
        TRANSACTION_FAULT(AfterJournal, 0);

        std::vector<std::unique_ptr<Handle>> installed;

        for(size_t index = 0; index < targets.size(); ++index)
        {
            if(originals[index])
            {
                Rename(originals[index]->value, File(session, index, ".removed"));
            }

            TRANSACTION_FAULT(DuringRetirement, index);
            auto apply = Open(File(session, index, ".apply"), GENERIC_READ | DELETE, OPEN_EXISTING);
            Require(Read(apply->value) == replacements[index].bytes, "Staged replacement changed");
            Rename(apply->value, targets[index]);
            installed.push_back(std::move(apply));
            originals[index].reset();
            TRANSACTION_FAULT(DuringInstallation, index);
        }

        TRANSACTION_FAULT(BeforeCommitMarker, 0);
        // Commit point: all replacements installed and the completion marker durably written.
        Complete(session, "committed");
        committed = true;
        TRANSACTION_FAULT(AfterCommitMarker, 0);
        installed.clear();
        Clean(session);
        return true;
    }
#if defined(_DEBUG)
    catch(const SimulatedCrash& crash)
    {
        // A crash runs no rollback: the next Recover sees exactly this state.
        error = crash.what();
        return false;
    }
#endif
    catch(const std::exception& exception)
    {
        error = exception.what();
        std::string recoveryError;

        if(prepared && !Recover(paths, recoveryError))
        {
            error += " Recovery failed: " + recoveryError;
        }

        // Once committed, cleanup failure cannot turn a successful replacement into a failed save.
        return committed;
    }
}

#if defined(_DEBUG)
void AssetReconciliationTransaction::InjectFaultForTesting(FaultStage stage, std::size_t index)
{
    g_faultStage = stage;
    g_faultIndex = index;
}
#endif

bool AssetReconciliationTransaction::ReadExisting(
    const ProjectPaths& paths,
    const std::filesystem::path& relativePath,
    std::string& bytes,
    std::string& error)
{
    error.clear();

    try
    {
        Session session(paths);
        Require(!Exists(session.directory), "Transaction recovery required before reading project documents");
        bytes = Read(session.Target(relativePath));
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
