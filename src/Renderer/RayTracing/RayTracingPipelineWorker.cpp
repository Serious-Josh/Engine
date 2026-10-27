#include "RayTracingPipelineWorker.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"

#include <d3d12.h>

#include <system_error>
#include <utility>

RayTracingPipelineWorker::~RayTracingPipelineWorker()
{
    Shutdown();
}

bool RayTracingPipelineWorker::Start()
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

void RayTracingPipelineWorker::Shutdown()
{
    bool creationInFlight = false;

    {
        std::lock_guard lock(m_mutex);

        if(!m_thread.joinable())
        {
            return;
        }

        m_stopRequested = true;
        m_running = false;

        // Queued jobs never start; only the creation in flight is waited for.
        for(const std::string& queuedKey : m_queue)
        {
            const auto found = m_jobs.find(queuedKey);

            if(found != m_jobs.end())
            {
                found->second.request.device->Release();
                m_jobs.erase(found);
            }
        }

        m_queue.clear();

        for(const auto& [key, job] : m_jobs)
        {
            if(job.state == RayTracingPipelineJobState::Creating)
            {
                creationInFlight = true;
            }
        }
    }

    m_wake.notify_all();

    if(creationInFlight)
    {
        Log::Info("Waiting for an in-flight DXR pipeline creation to finish before shutdown.");
    }

    m_thread.join();

    // Untaken results were never used by the GPU; their objects and the device references go now.
    std::lock_guard lock(m_mutex);

    for(auto& [key, job] : m_jobs)
    {
        job.outcome.pipeline.reset();
        job.request.device->Release();
    }

    m_jobs.clear();
    m_stopRequested = false;
}

bool RayTracingPipelineWorker::Submit(RayTracingPipelineRequest request)
{
    if(!request.device || !request.library)
    {
        return false;
    }

    {
        std::lock_guard lock(m_mutex);

        if(!m_running || m_jobs.contains(request.key))
        {
            return false;
        }

        // Held until the job's result is taken or discarded.
        request.device->AddRef();
        const std::string key = request.key;
        Job job{};
        job.outcome.key = key;
        job.outcome.device = request.device;
        job.request = std::move(request);
        m_jobs.emplace(key, std::move(job));
        m_queue.push_back(key);
    }

    m_wake.notify_one();
    return true;
}

RayTracingPipelineJobState RayTracingPipelineWorker::GetState(const std::string& key) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_jobs.find(key);

    if(found == m_jobs.end())
    {
        return RayTracingPipelineJobState::None;
    }

    return found->second.state;
}

std::optional<RayTracingPipelineOutcome> RayTracingPipelineWorker::TakeCompleted(const std::string& key)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_jobs.find(key);

    if(found == m_jobs.end() || found->second.state != RayTracingPipelineJobState::Completed)
    {
        return std::nullopt;
    }

    RayTracingPipelineOutcome outcome = std::move(found->second.outcome);
    // The caller compares the device pointer; the device itself outlives this worker (Renderer shuts it down first).
    found->second.request.device->Release();
    m_jobs.erase(found);
    return outcome;
}

void RayTracingPipelineWorker::Run()
{
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

        const std::string key = m_queue.front();
        m_queue.pop_front();
        Job& job = m_jobs.at(key);
        job.state = RayTracingPipelineJobState::Creating;
        // Immutable while creating: Submit never replaces a job's request and Shutdown never erases a creating job.
        const RayTracingPipelineRequest& request = job.request;

        // Create without the lock so the owner can poll.
        lock.unlock();

        RayTracingPipelineOutcome outcome{};
        outcome.key = key;
        outcome.device = request.device;

        if(!request.failForTesting)
        {
            auto pipeline = std::make_unique<RayTracingPipeline>();
            TransitionTiming::Scope er1Scope("Worker RayTracingPipeline " + request.description.shaderPath.generic_string());

            if(pipeline->Initialize(request.device, *request.library, request.description))
            {
                outcome.succeeded = true;
                outcome.pipeline = std::move(pipeline);
            }
        }

        lock.lock();
        job.outcome = std::move(outcome);
        job.state = RayTracingPipelineJobState::Completed;
    }
}
