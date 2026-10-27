#include "ShaderLibraryCompileWorker.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace
{
    // A compilation that failed before DXC read its source records nothing. Recording the source as it is
    // now lets IsCurrent tell a later fix (the file appears or changes) from an unchanged failure.
    void RecordUnreadSource(ShaderLibraryCompileOutcome& outcome, const std::filesystem::path& sourcePath)
    {
        if(!outcome.library.dependencies.empty())
        {
            return;
        }

        ShaderSourceDependency dependency{};
        dependency.path = sourcePath;
        std::ifstream file(sourcePath, std::ios::binary);
        dependency.exists = file.is_open();

        if(dependency.exists)
        {
            dependency.contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        outcome.library.dependencies.push_back(std::move(dependency));
    }
}

ShaderLibraryCompileWorker::~ShaderLibraryCompileWorker()
{
    Shutdown();
}

bool ShaderLibraryCompileWorker::Start()
{
    std::lock_guard lock(m_mutex);

    if(m_thread.joinable())
    {
        return false;
    }

    m_running = true;
    m_stopRequested = false;

    try
    {
        m_thread = std::thread([this]
        {
            Run();
        });
    }
    catch(const std::system_error&)
    {
        m_running = false;
        return false;
    }

    return true;
}

void ShaderLibraryCompileWorker::Shutdown()
{
    bool compilationInFlight = false;

    {
        std::lock_guard lock(m_mutex);

        if(!m_thread.joinable())
        {
            return;
        }

        m_stopRequested = true;
        m_running = false;

        // Queued jobs never start; only the compilation in flight is waited for.
        for(const std::filesystem::path& queuedLibrary : m_queue)
        {
            m_jobs.erase(queuedLibrary);
        }

        m_queue.clear();

        for(const auto& [libraryPath, job] : m_jobs)
        {
            if(job.state == ShaderLibraryCompileState::Compiling)
            {
                compilationInFlight = true;
            }
        }
    }

    m_wake.notify_all();

    if(compilationInFlight)
    {
        Log::Info("Waiting for an in-flight DXR shader library compilation to finish before shutdown.");
    }

    // Never detached: the worker uses dxcompiler.dll and this object, so it must end before either goes.
    m_thread.join();

    std::lock_guard lock(m_mutex);
    m_jobs.clear();
    m_stopRequested = false;
}

bool ShaderLibraryCompileWorker::Submit(const ShaderLibraryCompileRequest& request)
{
    {
        std::lock_guard lock(m_mutex);

        if(!m_running || m_jobs.contains(request.libraryPath))
        {
            return false;
        }

        Job job{};
        job.request = request;
        job.outcome.libraryPath = request.libraryPath;
        m_jobs.emplace(request.libraryPath, std::move(job));
        m_queue.push_back(request.libraryPath);
    }

    m_wake.notify_one();
    return true;
}

ShaderLibraryCompileState ShaderLibraryCompileWorker::GetState(const std::filesystem::path& libraryPath) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_jobs.find(libraryPath);

    if(found == m_jobs.end())
    {
        return ShaderLibraryCompileState::None;
    }

    return found->second.state;
}

std::optional<ShaderLibraryCompileOutcome> ShaderLibraryCompileWorker::TakeCompleted(
    const std::filesystem::path& libraryPath)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_jobs.find(libraryPath);

    if(found == m_jobs.end() || found->second.state != ShaderLibraryCompileState::Completed)
    {
        return std::nullopt;
    }

    ShaderLibraryCompileOutcome outcome = std::move(found->second.outcome);
    m_jobs.erase(found);
    return outcome;
}

bool ShaderLibraryCompileWorker::IsCurrent(const ShaderLibraryCompileOutcome& outcome)
{
    return outcome.library.profile == ShaderCompiler::GetLibraryProfile(outcome.libraryPath) &&
        ShaderCompiler::DependenciesUnchanged(outcome.library.dependencies);
}

void ShaderLibraryCompileWorker::Run()
{
    // Created, used and released on this thread only; the owner's compilers are never shared with it.
    ShaderCompiler compiler;
    std::string compilerDiagnostics;
    const bool compilerAvailable = compiler.Initialize(compilerDiagnostics);

    std::unique_lock lock(m_mutex);

    while(true)
    {
        m_wake.wait(lock, [this]
        {
            return m_stopRequested || !m_queue.empty();
        });

        if(m_stopRequested)
        {
            break;
        }

        const std::filesystem::path libraryPath = m_queue.front();
        m_queue.pop_front();
        Job& job = m_jobs.at(libraryPath);
        job.state = ShaderLibraryCompileState::Compiling;
        const std::wstring sourcePath = job.request.sourcePath.wstring();
        const std::wstring includeDirectory = job.request.includeDirectory.wstring();

        // Compile without the lock so the owner can poll; only this thread touches a compiling job's outcome.
        lock.unlock();

        ShaderLibraryCompileOutcome outcome{};
        outcome.libraryPath = libraryPath;

        if(compilerAvailable)
        {
            ShaderLibraryCompileDescription description{};
            description.path = sourcePath.c_str();
            description.profile = ShaderCompiler::GetLibraryProfile(libraryPath);
            description.includeDirectory = includeDirectory.empty() ? nullptr : includeDirectory.c_str();
            description.usePersistentCache = true;
            TransitionTiming::Scope er1Scope("Worker CompileLibrary");
            outcome.succeeded = compiler.CompileLibrary(description, outcome.library, outcome.diagnostics);
        }
        else
        {
            outcome.diagnostics = "The worker's DXC compiler could not be created. " + compilerDiagnostics;
        }

        if(!outcome.succeeded)
        {
            RecordUnreadSource(outcome, sourcePath);
        }

        lock.lock();
        // Shutdown never erases a compiling job, so the reference is still valid.
        job.outcome = std::move(outcome);
        job.state = ShaderLibraryCompileState::Completed;
    }

    lock.unlock();
    compiler.Shutdown();
}
