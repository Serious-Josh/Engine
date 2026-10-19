#include "Scripting/ScriptHost.h"
#include "Core/Log.h"

#include <angelscript.h>
#include <scriptstdstring.h>
#include <scriptbuilder.h>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <map>

namespace Scripting
{
namespace
{
constexpr std::size_t TextLimit = 1024;
constexpr std::size_t DiagnosticLimit = 32;
constexpr std::size_t SourceLimit = 1024 * 1024;

std::string Bounded(std::string_view text)
{
    std::string result(text.substr(0, TextLimit));
    std::replace(result.begin(), result.end(), '\0', '?');
    return result;
}

void AddDiagnostic(Result& result, std::string_view text, std::string_view section = {},
    int line = 0, int column = 0, std::string_view severity = "Error")
{
    if(result.diagnostics.size() < DiagnosticLimit)
    {
        result.diagnostics.push_back({Bounded(section), line, column, Bounded(severity), Bounded(text)});
    }
}

struct EngineDeleter
{
    void operator()(asIScriptEngine* engine) const noexcept
    {
        engine->ShutDownAndRelease();
    }
};

struct ModuleDeleter
{
    void operator()(asIScriptModule* module) const noexcept
    {
        module->Discard();
    }
};

struct ContextDeleter
{
    void operator()(asIScriptContext* context) const noexcept
    {
        // Abort suspended calls before unpreparing; a context never survives Execute.
        context->Abort();
        context->Unprepare();
        context->Release();
    }
};

struct Budget
{
    std::size_t remaining;
    std::chrono::steady_clock::time_point deadline;
    bool exceeded = false;
};

struct SourceCollection
{
    const SourceReader& reader;
    SourceLimits limits;
    Result& result;
    SourceSnapshot snapshot;
    std::map<std::string, SourceSection> sources;
    std::vector<std::string> active;
    std::size_t total = 0;

    int Load(const std::string& name, CScriptBuilder& builder)
    {
        CanonicalScriptSection(name);

        if(std::find(active.begin(), active.end(), name) != active.end())
        {
            throw std::runtime_error("Script include cycle: " + name);
        }

        for(const auto& [existing, source] : sources)
        {
            if(existing != name && ScriptNamesEqual(existing, name))
            {
                throw std::runtime_error("Ambiguous script section alias: " + name);
            }
        }

        if(sources.contains(name))
        {
            return 0;
        }

        if(active.size() >= limits.depth || sources.size() >= limits.sections)
        {
            throw std::runtime_error("Script include depth or section limit exceeded.");
        }

        SourceSection source = reader(name);

        if(source.name != name || source.assetReference.empty() || source.assetReference.size() > 256 ||
            source.bytes.size() > limits.sectionBytes ||
            source.bytes.size() > limits.totalBytes - total || source.bytes.find('\0') != std::string::npos)
        {
            throw std::runtime_error("Invalid script source identity, bytes or closure size.");
        }

        for(const auto& [existingName, existing] : sources)
        {
            if(existing.assetReference == source.assetReference)
            {
                throw std::runtime_error("Multiple script section names share one asset identity.");
            }
        }

        if(name == snapshot.rootSection)
        {
            snapshot.rootAsset = source.assetReference;
        }

        total += source.bytes.size();
        auto [entry, inserted] = sources.emplace(name, std::move(source));
        active.push_back(name);
        const int added = builder.AddSectionFromMemory(name.c_str(), entry->second.bytes.data(),
            static_cast<unsigned int>(entry->second.bytes.size()));
        active.pop_back();

        if(added < 0)
        {
            throw std::runtime_error("Script source preprocessing failed: " + name);
        }

        return added;
    }

    static int Include(const char* include, const char* from, CScriptBuilder* builder, void* parameter) noexcept
    {
        auto& collection = *static_cast<SourceCollection*>(parameter);

        try
        {
            return collection.Load(ResolveScriptInclude(from, include), *builder);
        }
        catch(const std::exception& error)
        {
            try
            {
                AddDiagnostic(collection.result, error.what(), from);
            }
            catch(...)
            {
            }

            return -1;
        }
        catch(...)
        {
            return -1;
        }
    }
};

void CheckBudget(asIScriptContext* context, Budget* budget) noexcept
{
    if(budget->remaining == 0 || std::chrono::steady_clock::now() >= budget->deadline)
    {
        budget->exceeded = true;
        context->Abort();
        return;
    }

    --budget->remaining;
}
}

struct ScriptHost::Impl
{
    const std::thread::id owner = std::this_thread::get_id();
    Result result;
    bool executing = false;
    std::string section;
    // Reverse member destruction releases the module before its engine.
    std::unique_ptr<asIScriptEngine, EngineDeleter> engine;
    std::unique_ptr<asIScriptModule, ModuleDeleter> module;

    bool IsAvailable() const noexcept
    {
        return owner == std::this_thread::get_id() && !executing;
    }

    static void Message(const asSMessageInfo* message, Impl* state) noexcept
    {
        try
        {
            const char* severity = message->type == asMSGTYPE_ERROR ? "Error" :
                message->type == asMSGTYPE_WARNING ? "Warning" : "Info";
            AddDiagnostic(state->result, message->message, message->section,
                message->row, message->col, severity);
        }
        catch(...)
        {
            // Allocation failures must not unwind through the compiler's C callback.
        }
    }

    static void Info(const std::string& message, Impl* state) noexcept
    {
        auto* context = asGetActiveContext();

        try
        {
            if(!context || !state->executing || state->owner != std::this_thread::get_id())
            {
                throw std::runtime_error("Logging requires the owning execution context.");
            }

            // Per-execution log cap prevents a bounded loop from flooding the host sink.
            if(state->logCount++ >= 64)
            {
                context->SetException("Script log message limit exceeded.");
                return;
            }

            const char* activeSection = nullptr;
            const int line = context->GetLineNumber(0, nullptr, &activeSection);
            Log::Info("[Script " + Bounded(activeSection ? activeSection : state->section) + ":" +
                std::to_string(line) + "] " + Bounded(message));
        }
        catch(const std::exception&)
        {
            if(context)
            {
                context->SetException("Recoverable native logging exception.");
            }
        }
        catch(...)
        {
            if(context)
            {
                context->SetException("Unknown native logging exception.");
            }
        }
    }

    std::size_t logCount = 0;
};

ScriptHost::ScriptHost() : m_impl(std::make_unique<Impl>())
{
}

ScriptHost::~ScriptHost() = default;

Result ScriptHost::Initialize()
{
    return InitializeWithLogDeclaration("void Info(const string &in)");
}

Result ScriptHost::InitializeWithLogDeclaration(const char* declaration)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.module.reset();
    state.engine.reset();
    state.result = {Status::Failed};
    std::unique_ptr<asIScriptEngine, EngineDeleter> candidate(asCreateScriptEngine(ANGELSCRIPT_VERSION));

    if(!candidate)
    {
        AddDiagnostic(state.result, "Cannot create AngelScript 2.38.0 engine.");
        return state.result;
    }

    const auto require = [](int code)
    {
        if(code < 0)
        {
            throw std::runtime_error("AngelScript initialization failed with status " + std::to_string(code));
        }
    };

    try
    {
        require(candidate->SetMessageCallback(asFUNCTION(Impl::Message), &state, asCALL_CDECL));
        // No global initializer or module teardown script execution is admitted by AS.2.
        require(candidate->SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS, true));
        require(candidate->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false));
        require(candidate->SetEngineProperty(asEP_MAX_STACK_SIZE, 256 * 1024));
        require(candidate->SetEngineProperty(asEP_MAX_CALL_STACK_SIZE, 128));
        RegisterStdString(candidate.get());

        if(!candidate->GetTypeInfoByDecl("string"))
        {
            throw std::runtime_error("AngelScript string registration failed.");
        }

        require(candidate->SetDefaultNamespace("Log"));
        require(candidate->RegisterGlobalFunction(declaration, asFUNCTION(Impl::Info), asCALL_CDECL_OBJLAST, &state));
        require(candidate->SetDefaultNamespace(""));
        // Build an empty module to validate the complete configuration, including the
        // upstream string add-on whose registration entry point returns void.
        std::unique_ptr<asIScriptModule, ModuleDeleter> probe(candidate->GetModule("configuration", asGM_ALWAYS_CREATE));

        if(!probe)
        {
            throw std::runtime_error("Cannot allocate configuration module.");
        }

        require(probe->Build());
        probe.reset();
        state.engine = std::move(candidate);
        state.result.status = Status::Ready;
    }
    catch(const std::exception& error)
    {
        AddDiagnostic(state.result, error.what());
    }

    return state.result;
}

Result ScriptHost::Shutdown()
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.module.reset();
    state.engine.reset();
    state.section.clear();
    state.result = {Status::NotInitialized};
    return state.result;
}

Result ScriptHost::Compile(std::string_view section, std::string_view source)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.result = {Status::Failed};

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    // Failed compilation never leaves a stale previous module executable.
    state.module.reset();

    if(section.empty() || section.size() > 256 || section.find('\0') != std::string_view::npos ||
        source.empty() || source.size() > SourceLimit || source.find('\0') != std::string_view::npos)
    {
        AddDiagnostic(state.result, "Invalid section name or source size/encoding.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    state.section = section;
    std::unique_ptr<asIScriptModule, ModuleDeleter> candidate(state.engine->GetModule("isolated", asGM_ALWAYS_CREATE));

    if(!candidate || candidate->AddScriptSection(state.section.c_str(), source.data(), source.size()) < 0 ||
        candidate->Build() < 0)
    {
        AddDiagnostic(state.result, "In-memory compilation failed.", section);
        return state.result;
    }

    if(candidate->GetObjectTypeCount() != 0)
    {
        AddDiagnostic(state.result, "Script classes/interfaces are deferred to the Play-scoped host.", section);
        return state.result;
    }

    state.module = std::move(candidate);
    state.result.status = Status::Compiled;
    return state.result;
}

Result ScriptHost::ValidateSources(std::string_view rootSection, const SourceReader& reader, SourceLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.module.reset();
    state.result = {Status::Failed};
    SourceCollection collection{reader, limits, state.result};
    struct ValidationScope
    {
        bool& busy;
        ~ValidationScope()
        {
            busy = false;
        }
    } validationScope{state.executing};
    state.executing = true;
    collection.snapshot.rootSection = Bounded(rootSection);

    try
    {
        if(!reader || limits.sections == 0 || limits.sections > 64 || limits.depth == 0 || limits.depth > 16 ||
            limits.sectionBytes == 0 || limits.sectionBytes > SourceLimit || limits.totalBytes == 0 ||
            limits.totalBytes > 8 * SourceLimit)
        {
            throw std::runtime_error("Invalid source closure limits.");
        }

        CanonicalScriptSection(rootSection);
        CScriptBuilder builder;
        builder.SetIncludeCallback(SourceCollection::Include, &collection);

        if(builder.StartNewModule(state.engine.get(), "validation-only") < 0)
        {
            throw std::runtime_error("Cannot prepare source validation module.");
        }

        std::unique_ptr<asIScriptModule, ModuleDeleter> module(builder.GetModule());
        collection.Load(std::string(rootSection), builder);
        collection.snapshot.complete = true;
        const int built = builder.BuildModule();

        if(built >= 0 && module->GetObjectTypeCount() == 0)
        {
            state.result.status = Status::Compiled;
        }
        else if(built >= 0)
        {
            AddDiagnostic(state.result, "Script classes/interfaces remain deferred; validation does not instantiate them.", rootSection);
        }
    }
    catch(const std::exception& error)
    {
        AddDiagnostic(state.result, error.what(), rootSection);
    }

    for(auto& [name, source] : collection.sources)
    {
        collection.snapshot.sections.push_back(std::move(source));
    }

    collection.snapshot.revision = SourceClosureRevision(collection.snapshot);

    for(auto& diagnostic : state.result.diagnostics)
    {
        diagnostic.assetReference = collection.snapshot.rootAsset;
        diagnostic.sourceRevision = collection.snapshot.revision;

        for(const auto& source : collection.snapshot.sections)
        {
            if(source.name == diagnostic.section)
            {
                diagnostic.assetReference = source.assetReference;
                break;
            }
        }
    }

    state.result.snapshot = std::make_shared<const SourceSnapshot>(std::move(collection.snapshot));
    return state.result;
}

Result ScriptHost::ValidateSnapshot(const SourceSnapshot& snapshot, SourceLimits limits)
{
    // Reapply include/path/closure policy to memory-only data. No filesystem fallback.
    auto result = ValidateSources(snapshot.rootSection, [&](std::string_view name)
    {
        const SourceSection* match = nullptr;

        for(const auto& source : snapshot.sections)
        {
            if(source.name == name)
            {
                if(match)
                {
                    throw std::runtime_error("Duplicate source section in memory snapshot.");
                }

                match = &source;
            }
        }

        if(!match)
        {
            throw std::runtime_error("Missing source section in memory snapshot.");
        }

        return *match;
    }, limits);

    if(result.snapshot && snapshot.complete && result.snapshot->revision != snapshot.revision)
    {
        result.status = Status::Failed;
        const auto previousCount = result.diagnostics.size();
        AddDiagnostic(result, "Snapshot digest/identity or unused-section mismatch.", snapshot.rootSection);

        if(result.diagnostics.size() != previousCount)
        {
            auto& diagnostic = result.diagnostics.back();
            diagnostic.assetReference = snapshot.rootAsset;
            diagnostic.sourceRevision = result.snapshot->revision;
        }
    }

    return result;
}

Result ScriptHost::Execute(std::string_view declaration, ExecutionLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.result = {Status::Failed};

    if(!state.module || declaration.empty() || declaration.size() > 256 ||
        declaration.find('\0') != std::string_view::npos || limits.statements == 0 ||
        limits.timeout.count() <= 0 || limits.timeout > std::chrono::seconds(10))
    {
        AddDiagnostic(state.result, "Execution requires a compiled module and bounded limits.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    const std::string functionDeclaration(declaration);
    auto* function = state.module->GetFunctionByDecl(functionDeclaration.c_str());

    if(!function)
    {
        AddDiagnostic(state.result, "Missing function: " + functionDeclaration, state.section);
        state.result.status = Status::MissingFunction;
        return state.result;
    }

    if(function->GetParamCount() != 0 || function->GetReturnTypeId() != asTYPEID_VOID)
    {
        AddDiagnostic(state.result, "AS.2 execution requires void with no parameters.", state.section);
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    Budget budget{limits.statements, std::chrono::steady_clock::now() + limits.timeout};
    std::unique_ptr<asIScriptContext, ContextDeleter> context(state.engine->CreateContext());

    if(!context || context->Prepare(function) < 0 ||
        context->SetLineCallback(asFUNCTION(CheckBudget), &budget, asCALL_CDECL) < 0)
    {
        AddDiagnostic(state.result, "Cannot prepare bounded execution context.");
        return state.result;
    }

    struct ExecutionScope
    {
        bool& active;
        ~ExecutionScope()
        {
            active = false;
        }
    } executionScope{state.executing};

    state.executing = true;
    state.logCount = 0;
    const int execution = context->Execute();

    if(execution == asEXECUTION_FINISHED)
    {
        state.result.status = Status::Finished;
    }
    else if(budget.exceeded)
    {
        state.result.status = Status::BudgetExceeded;
        AddDiagnostic(state.result, "Script execution budget exceeded; context aborted.", state.section);
    }
    else if(execution == asEXECUTION_EXCEPTION)
    {
        state.result.status = Status::Exception;
        const char* section = nullptr;
        int column = 0;
        const int line = context->GetExceptionLineNumber(&column, &section);
        AddDiagnostic(state.result, context->GetExceptionString(), section ? section : "", line, column);

        for(asUINT index = 0; index < (std::min)(context->GetCallstackSize(), asUINT(8)); ++index)
        {
            auto* frame = context->GetFunction(index);

            if(frame)
            {
                AddDiagnostic(state.result, frame->GetDeclaration(), state.section, context->GetLineNumber(index), 0, "Stack");
            }
        }
    }
    else
    {
        state.result.status = execution == asEXECUTION_SUSPENDED ? Status::Suspended : Status::Failed;
        AddDiagnostic(state.result, "Unexpected execution status " + std::to_string(execution) +
            "; context discarded without resumption.", state.section);
    }

    return state.result;
}
}
