#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "Scripting/ScriptSources.h"

namespace Scripting
{
enum class Status
{
    Ready,
    Compiled,
    Finished,
    NotInitialized,
    InvalidRequest,
    Failed,
    MissingFunction,
    Exception,
    BudgetExceeded,
    Suspended
};

struct Diagnostic
{
    std::string section;
    int line = 0;
    int column = 0;
    std::string severity;
    std::string text;
    std::string assetReference;
    std::string sourceRevision;
};

struct Result
{
    Status status = Status::Failed;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<const SourceSnapshot> snapshot;
};

struct ExecutionLimits
{
    std::size_t statements = 100000;
    std::chrono::milliseconds timeout{100};
};

// Isolated session adapter. Not instantiated by Browser, scene activation or Play in AS.2.
// Construct, use and destroy on one owning thread. No VM/context pointers escape this API.
class ScriptHost final
{
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    Result Initialize();
    Result Shutdown();
    Result Compile(std::string_view section, std::string_view source);
    // Nonexecuting validation only: does not install an executable module.
    Result ValidateSources(std::string_view rootSection, const SourceReader& reader, SourceLimits limits = {});
    Result ValidateSnapshot(const SourceSnapshot& snapshot, SourceLimits limits = {});
    // AS.2 permits only a global void function with no arguments, executed synchronously.
    Result Execute(std::string_view declaration, ExecutionLimits limits = {});

private:
    friend class ScriptHostVerification;
    Result InitializeWithLogDeclaration(const char* declaration);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
