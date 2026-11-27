#pragma once

#include "RayTracingPipeline.h"
#include "RayTracingPipelineDescription.h"
#include "Renderer/AsyncWorkerActivity.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ID3D12Device5;

// ER.1 Phase 4: creates DXR pipelines (state object, global root signature and shader tables) on owned worker threads
// (startup work 4.5: several, CreateStateObject being free-threaded) so a cold activation never calls CreateStateObject
// while recording. A request is immutable and self-contained: the device (referenced for the job's lifetime), a shared
// snapshot of the already compiled DXIL and the description. The worker touches no Renderer or PathTraceReferencePass state; every D3D12 call it makes is a
// free-threaded device method, and the shader tables are its own committed upload buffers (no command list, descriptor
// heap or shared allocator). A finished pipeline is owned by the worker until the render thread takes it, validates its
// identity and adopts it (RayTracingPipeline::Swap); nothing it built has ever been used by the GPU.
struct RayTracingPipelineRequest
{
    // The caller's identity for the pipeline: description and library revision. One key has at most one job.
    std::string key;
    ID3D12Device5* device = nullptr;
    std::shared_ptr<const std::vector<char>> library;
    RayTracingPipelineDescription description;
    // Diagnostics only: the job fails without creating anything.
    bool failForTesting = false;
    // CPU-only preparation of a private auxiliary pipeline bundle on this same owned worker.
    // Captures must own their output and must not mutate active renderer state.
    std::function<bool()> prepareAuxiliary;
    // Startup work (4.1/4.5): a speculative warm-up. It starts only while no ordinary job is queued and runs at the lowest
    // thread priority, so it never delays what the configuration on screen or the one requested needs.
    bool background = false;

};

struct RayTracingPipelineOutcome
{
    std::string key;
    ID3D12Device5* device = nullptr;
    bool succeeded = false;
    // Complete (state object, identifiers, tables) when succeeded, otherwise null.
    std::unique_ptr<RayTracingPipeline> pipeline;
};

enum class RayTracingPipelineJobState
{
    // Not submitted, already taken, or dropped at shutdown.
    None,
    Queued,
    Creating,
    Completed
};

class RayTracingPipelineWorker
{
    public:
    RayTracingPipelineWorker() = default;
    RayTracingPipelineWorker(const RayTracingPipelineWorker&) = delete;
    RayTracingPipelineWorker& operator=(const RayTracingPipelineWorker&) = delete;
    ~RayTracingPipelineWorker();

    // Startup work (4.5): threadCount threads (at least one) share the queue; a key still has at most one job.
    bool Start(unsigned int threadCount = 1);

    // The thread count for the shared DXR pipeline worker on this machine: one per eight hardware threads, 2 to 4, or
    // ENGINE_DXR_PIPELINE_WORKERS (1-8).
    static unsigned int DefaultThreadCount();

    // Drops queued jobs, waits for the creations in flight (a D3D12 call cannot be cancelled), joins the threads and
    // destroys every untaken result. Must run before the device is released. Start may be called again afterwards.
    void Shutdown();

    // Queues a creation. False when the worker is not running or the key already has a job.
    bool Submit(RayTracingPipelineRequest request);

    RayTracingPipelineJobState GetState(const std::string& key) const;
    // Layered prep (F1): drops a job that no thread has started yet (its request is destroyed, the key may be submitted
    // again). False when the key has no queued job (none, or it is creating or completed).
    bool CancelQueued(const std::string& key);

    // Moves a completed result out; the key then has no job and may be submitted again.
    std::optional<RayTracingPipelineOutcome> TakeCompleted(const std::string& key);

    // Read-only progress reporting for the Editor's Background Tasks; see AsyncWorkerActivity for the counters.
    void GetActivity(AsyncWorkerActivity& activity) const;

    // AsyncWorkerActivity::taken alone, for telling a newly taken result from a re-reported one.
    std::uint64_t GetTakenCount() const;

    private:
    struct Job
    {
        RayTracingPipelineRequest request;
        RayTracingPipelineJobState state = RayTracingPipelineJobState::Queued;
        RayTracingPipelineOutcome outcome;
        // Layered prep diagnostics (ENGINE_STARTUP_TIMING): when the job was queued, for its queue wait.
        std::chrono::steady_clock::time_point submitted{};
    };

    void Run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::thread> m_threads;
    std::map<std::string, Job> m_jobs;
    std::deque<std::string> m_queue;
    // Background requests, taken only while m_queue is empty.
    std::deque<std::string> m_backgroundQueue;
    bool m_running = false;
    bool m_stopRequested = false;

    // Activity counters, guarded by m_mutex like the jobs they count.
    AsyncWorkerActivity m_activity;
};
