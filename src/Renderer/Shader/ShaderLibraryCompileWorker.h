#pragma once

#include "ShaderCompiler.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Async DXR preparation, Gate 1: compiles DXR shader libraries on one owned worker thread so a first-time
// compilation never blocks frame recording. The worker owns a private ShaderCompiler (its own DXC compiler,
// utils and include handlers), touches no Renderer or ShaderLibrary state and never calls Engine logging.
// Results are owned values taken by the owner thread; this is not a cache, and a taken result must pass
// IsCurrent before it is published.
struct ShaderLibraryCompileRequest
{
    // The caller's key for the library, for example "PathTraceReSTIRDI.hlsl".
    std::filesystem::path libraryPath;
    // The resolved source file DXC reads (the shader root joined with libraryPath).
    std::filesystem::path sourcePath;
    // The shader root (ShaderLibraryCompileDescription::includeDirectory).
    std::filesystem::path includeDirectory;
};

struct ShaderLibraryCompileOutcome
{
    std::filesystem::path libraryPath;
    bool succeeded = false;
    // Bytecode and dependency records exactly as ShaderCompiler::CompileLibrary produces them. A failed
    // compilation keeps the dependencies it recorded, so a failure from a superseded source is detectable.
    ShaderLibraryCompileResult library;
    // DXC errors and warnings, and any failure the synchronous path would have logged.
    std::string diagnostics;
};

enum class ShaderLibraryCompileState
{
    // Not submitted, already taken, or dropped at shutdown.
    None,
    Queued,
    Compiling,
    Completed
};

// Where the owner's preparation of a library stands after polling the worker once.
enum class ShaderLibraryPreparationState
{
    // A current compiled library is held by its owner.
    Ready,
    Queued,
    Compiling,
    // ER.1 Phase 4: every library is ready; the DXR pipelines built from them are being created in the background.
    PreparingPipelines,
    // The current sources fail to compile; retried only once they change.
    Failed
};

struct ShaderLibraryPreparation
{
    ShaderLibraryPreparationState state = ShaderLibraryPreparationState::Queued;
    std::string diagnostic;
    // True on the poll that accepted a newly compiled revision of the library.
    bool acceptedNewRevision = false;
};

class ShaderLibraryCompileWorker
{
    public:
    ShaderLibraryCompileWorker() = default;
    ShaderLibraryCompileWorker(const ShaderLibraryCompileWorker&) = delete;
    ShaderLibraryCompileWorker& operator=(const ShaderLibraryCompileWorker&) = delete;
    ~ShaderLibraryCompileWorker();

    // Starts the worker thread. False if it is already running or could not be started.
    bool Start();

    // Drops queued jobs, waits for the compilation in flight (DXC cannot be cancelled) and joins the
    // thread. Logs the wait from the calling thread when a compilation is still running. Untaken results
    // are discarded. Start may be called again afterwards.
    void Shutdown();

    // Queues a compilation. False when the worker is not running or the library already has a queued,
    // compiling or untaken completed job, so one library never has two jobs.
    bool Submit(const ShaderLibraryCompileRequest& request);

    ShaderLibraryCompileState GetState(const std::filesystem::path& libraryPath) const;

    // Moves a completed result out; the library then has no job and may be submitted again.
    std::optional<ShaderLibraryCompileOutcome> TakeCompleted(const std::filesystem::path& libraryPath);

    // True when the outcome was compiled for the library's mapped profile and every source and include
    // lookup the compilation read is unchanged. A stale outcome, successful or failed, is discarded by the
    // caller and the library resubmitted.
    static bool IsCurrent(const ShaderLibraryCompileOutcome& outcome);

    private:
    struct Job
    {
        ShaderLibraryCompileRequest request;
        ShaderLibraryCompileState state = ShaderLibraryCompileState::Queued;
        ShaderLibraryCompileOutcome outcome;
    };

    void Run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    // Every library with a job, by key; the queue holds the keys of queued jobs in submission order.
    std::map<std::filesystem::path, Job> m_jobs;
    std::deque<std::filesystem::path> m_queue;
    bool m_running = false;
    bool m_stopRequested = false;
};
