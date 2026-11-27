#include "RayTracingPipelineWorker.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <utility>

RayTracingPipelineWorker::~RayTracingPipelineWorker()
{
    Shutdown();
}

bool RayTracingPipelineWorker::Start(unsigned int threadCount)
{
    std::lock_guard lock(m_mutex);

    if(!m_threads.empty())
    {
        return false;
    }

    m_running = true;
    m_stopRequested = false;

    for(unsigned int index = 0; index < (std::max)(threadCount, 1u); ++index)
    {
        try
        {
            m_threads.emplace_back([this]
            {
                Run();
            });
        }
        catch(const std::system_error&)
        {
            break;
        }
    }

    m_running = !m_threads.empty();
    return m_running;
}

unsigned int RayTracingPipelineWorker::DefaultThreadCount()
{
    if(const char* value = std::getenv("ENGINE_DXR_PIPELINE_WORKERS"))
    {
        const int requested = std::atoi(value);

        if(requested >= 1 && requested <= 8)
        {
            return static_cast<unsigned int>(requested);
        }
    }

    const unsigned int hardwareThreads = (std::max)(std::thread::hardware_concurrency(), 1u);
    return std::clamp(hardwareThreads / 8u, 2u, 4u);
}

void RayTracingPipelineWorker::Shutdown()
{
    bool creationInFlight = false;

    {
        std::lock_guard lock(m_mutex);

        if(m_threads.empty())
        {
            return;
        }

        m_stopRequested = true;
        m_running = false;

        // Queued jobs never start; only the creations in flight are waited for.
        for(std::deque<std::string>* queue : {&m_queue, &m_backgroundQueue})
        {
            for(const std::string& queuedKey : *queue)
            {
                const auto found = m_jobs.find(queuedKey);

                if(found != m_jobs.end())
                {
                    found->second.request.device->Release();
                    m_jobs.erase(found);
                }
            }

            m_activity.dropped += queue->size();
            queue->clear();
        }

        m_activity.queued = 0;

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

    for(std::thread& thread : m_threads)
    {
        thread.join();
    }

    m_threads.clear();

    // Untaken results were never used by the GPU; their objects and the device references go now.
    std::lock_guard lock(m_mutex);
    // Only completed jobs remain after the join: queued ones were erased above and those in flight have finished.
    m_activity.discarded += m_jobs.size();
    m_activity.untaken = 0;

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
    if(!request.device || (!request.library && !request.prepareAuxiliary))
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
        const bool background = request.background;
        Job job{};
        job.outcome.key = key;
        job.outcome.device = request.device;
        job.submitted = std::chrono::steady_clock::now();
        job.request = std::move(request);
        m_jobs.emplace(key, std::move(job));
        (background ? m_backgroundQueue : m_queue).push_back(key);
        ++m_activity.submitted;
        ++m_activity.queued;
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

bool RayTracingPipelineWorker::CancelQueued(const std::string& key)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_jobs.find(key);

    if(found == m_jobs.end() || found->second.state != RayTracingPipelineJobState::Queued)
    {
        return false;
    }

    for(std::deque<std::string>* queue : {&m_queue, &m_backgroundQueue})
    {
        const auto queued = std::find(queue->begin(), queue->end(), key);

        if(queued != queue->end())
        {
            queue->erase(queued);
        }
    }

    found->second.request.device->Release();
    m_jobs.erase(found);
    --m_activity.queued;
    ++m_activity.dropped;
    return true;
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
    ++m_activity.taken;
    --m_activity.untaken;
    return outcome;
}

void RayTracingPipelineWorker::GetActivity(AsyncWorkerActivity& activity) const
{
    std::lock_guard lock(m_mutex);
    activity = m_activity;
}

std::uint64_t RayTracingPipelineWorker::GetTakenCount() const
{
    std::lock_guard lock(m_mutex);
    return m_activity.taken;
}

void RayTracingPipelineWorker::Run()
{
    std::unique_lock lock(m_mutex);

    while(true)
    {
        m_wake.wait(lock, [this]
        {
            return m_stopRequested || !m_queue.empty() || !m_backgroundQueue.empty();
        });

        if(m_stopRequested)
        {
            break;
        }

        // Ordinary jobs first; a background job only when none is queued.
        const bool background = m_queue.empty();
        std::deque<std::string>& queue = background ? m_backgroundQueue : m_queue;
        const std::string key = queue.front();
        queue.pop_front();
        Job& job = m_jobs.at(key);
        job.state = RayTracingPipelineJobState::Creating;
        --m_activity.queued;
        ++m_activity.activeCount;
        m_activity.active = true;
        m_activity.activeJob = key;
        // Immutable while creating: Submit never replaces a job's request and Shutdown never erases a creating job.
        const RayTracingPipelineRequest& request = job.request;

        // Layered prep diagnostics (ENGINE_STARTUP_TIMING): how long the job waited in its queue before a thread took it.
        if(TransitionTiming::StartupEnabled())
        {
            const auto now = std::chrono::steady_clock::now();
            Log::Info("[STARTUP] t" + std::to_string(GetCurrentThreadId()) + " Worker job " + key + " started after " +
                std::to_string(std::chrono::duration<double, std::milli>(now - job.submitted).count()) + " ms in the " +
                (background ? "background " : "") + "queue at " + std::to_string(TransitionTiming::SecondsSinceProcessStart(now)) +
                " s");
        }

        // Create without the lock so the owner can poll.
        lock.unlock();

        if(background)
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        }

        RayTracingPipelineOutcome outcome{};
        outcome.key = key;
        outcome.device = request.device;

        if(request.prepareAuxiliary && !request.failForTesting)
        {
            try
            {
                // Named by the job's key: the PSR auxiliary pipelines, or (M0c.5 Stage 5.3) AIR and volumetric shaders.
                TransitionTiming::Scope scope("Worker auxiliary preparation: " + key, 0.1, TransitionTiming::TransitionsAndStartup);
                outcome.succeeded = request.prepareAuxiliary();
            }
            catch(...)
            {
                // A failed private bundle is never published. The owner reports the failed preparation.
                outcome.succeeded = false;
            }
        }
        else if(!request.failForTesting)
        {
            auto pipeline = std::make_unique<RayTracingPipeline>();
            TransitionTiming::Scope er1Scope("Worker RayTracingPipeline " + request.description.shaderPath.generic_string(), 0.1,
                TransitionTiming::TransitionsAndStartup);

            if(pipeline->Initialize(request.device, *request.library, request.description))
            {
                outcome.succeeded = true;
                outcome.pipeline = std::move(pipeline);
            }
        }

        if(background)
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        }

        lock.lock();
        job.outcome = std::move(outcome);
        job.state = RayTracingPipelineJobState::Completed;
        --m_activity.activeCount;
        m_activity.active = m_activity.activeCount > 0;

        if(!m_activity.active)
        {
            m_activity.activeJob.clear();
        }
        ++m_activity.finished;
        ++m_activity.untaken;
    }
}
