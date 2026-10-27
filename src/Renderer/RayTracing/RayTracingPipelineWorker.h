#pragma once

#include "RayTracingPipeline.h"
#include "RayTracingPipelineDescription.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ID3D12Device5;

// ER.1 Phase 4: creates DXR pipelines (state object, global root signature and shader tables) on one owned worker
// thread so a cold activation never calls CreateStateObject while recording. A request is immutable and
// self-contained: the device (referenced for the job's lifetime), a shared snapshot of the already compiled DXIL and the
// description. The worker touches no Renderer or PathTraceReferencePass state; every D3D12 call it makes is a
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

    bool Start();

    // Drops queued jobs, waits for the creation in flight (a D3D12 call cannot be cancelled), joins the thread and
    // destroys every untaken result. Must run before the device is released. Start may be called again afterwards.
    void Shutdown();

    // Queues a creation. False when the worker is not running or the key already has a job.
    bool Submit(RayTracingPipelineRequest request);

    RayTracingPipelineJobState GetState(const std::string& key) const;

    // Moves a completed result out; the key then has no job and may be submitted again.
    std::optional<RayTracingPipelineOutcome> TakeCompleted(const std::string& key);

    private:
    struct Job
    {
        RayTracingPipelineRequest request;
        RayTracingPipelineJobState state = RayTracingPipelineJobState::Queued;
        RayTracingPipelineOutcome outcome;
    };

    void Run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::map<std::string, Job> m_jobs;
    std::deque<std::string> m_queue;
    bool m_running = false;
    bool m_stopRequested = false;
};
