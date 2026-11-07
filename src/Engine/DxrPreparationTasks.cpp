#include "Engine/DxrPreparationTasks.h"

#include <algorithm>

namespace
{
    constexpr const char* ParentName = "Preparing ray tracing shaders";
    constexpr const char* UnusedMessage = "Some results were not used: the request changed before they were needed.";
    constexpr const char* ShutdownMessage = "The renderer shut down before preparation finished.";

    bool CountersWentBack(const AsyncWorkerActivity& activity, std::uint64_t seenSubmissions)
    {
        return activity.submitted < seenSubmissions;
    }
}

void DxrPreparationTasks::Update(BackgroundTaskManager& manager,
    const AsyncWorkerActivity& libraries,
    const AsyncWorkerActivity& pipelines,
    const DxrPreparationFrameReport* report)
{
    // Workers that were recreated start their counters again; nothing from before can be followed.
    const bool restarted = CountersWentBack(libraries, m_seenLibrarySubmissions) ||
        CountersWentBack(pipelines, m_seenPipelineSubmissions);

    if(!m_initialized || restarted)
    {
        // Work submitted before the Editor's first frame (or before a restart) is not presented.
        Stop();
        Rebase(libraries, pipelines);
        m_initialized = true;
        return;
    }

    if(!m_parent.IsValid())
    {
        const bool newWork = libraries.submitted > m_seenLibrarySubmissions ||
            pipelines.submitted > m_seenPipelineSubmissions;

        if(!newWork)
        {
            Rebase(libraries, pipelines);
            return;
        }

        BeginBatch(manager);
    }

    UpdateCategory(manager, m_libraries, libraries, report ? &report->libraryFailure : nullptr);
    UpdateCategory(manager, m_pipelines, pipelines, report ? &report->pipelineFailure : nullptr);
    m_seenLibrarySubmissions = libraries.submitted;
    m_seenPipelineSubmissions = pipelines.submitted;
    m_seenLibraryTaken = libraries.taken;
    m_seenPipelineTaken = pipelines.taken;

    if(!report || libraries.IsBusy() || pipelines.IsBusy())
    {
        return;
    }

    const bool allTaken = libraries.untaken == 0 && pipelines.untaken == 0;

    if(allTaken || !report->waiting)
    {
        FinishBatch(!allTaken);
        Rebase(libraries, pipelines);
    }
}

void DxrPreparationTasks::BeginBatch(BackgroundTaskManager& manager)
{
    m_parent = manager.Begin({.name = ParentName});
    m_parent.Start();
}

void DxrPreparationTasks::UpdateCategory(BackgroundTaskManager& manager,
    Category& category,
    const AsyncWorkerActivity& activity,
    const std::string* reportedFailure)
{
    // A failure belongs to this batch even after its category's round closed: the parent still reports it.
    if(reportedFailure && !reportedFailure->empty())
    {
        if(category.failure.empty())
        {
            category.failure = *reportedFailure;
        }

        if(category.roundFailure.empty())
        {
            category.roundFailure = *reportedFailure;
        }
    }

    // A category with nothing submitted gets no child, so no empty task looks active.
    if(!category.task.IsValid())
    {
        if(activity.submitted <= category.baseSubmitted)
        {
            return;
        }

        category.task = manager.Begin({.name = category.taskName, .parent = m_parent.GetId()});
        category.task.Start();
    }

    // Completed units are results the Renderer took, so the bar never fills before acceptance. A result left untaken
    // from before this round can be taken during it; the clamp keeps that from overcounting.
    const std::uint64_t total = activity.submitted - category.baseSubmitted;
    const std::uint64_t taken = activity.taken - (std::min)(activity.taken, category.baseTaken);
    category.task.SetProgress((std::min)(taken, total), total);

    if(activity.active)
    {
        m_status.assign(category.activeVerb);
        m_status += ' ';
        m_status += activity.activeJob;
    }
    else if(activity.queued > 0)
    {
        m_status.assign("Queued");
    }
    else if(activity.untaken > 0)
    {
        m_status.assign("Waiting for the renderer to accept the results");
    }
    else
    {
        m_status.assign("All results accepted");
    }

    category.task.SetStatus(m_status);

    if(activity.IsBusy() || activity.untaken > 0)
    {
        m_parent.SetStatus(m_status);
    }

    // A round ends when its worker is idle and the Renderer has taken every result. Later submissions in the same batch
    // (a stale result resubmitted, or another gate's work) open a new round.
    if(!activity.IsBusy() && activity.untaken == 0)
    {
        FinishCategory(category, nullptr);
        category.baseSubmitted = activity.submitted;
        category.baseTaken = activity.taken;
    }
}

void DxrPreparationTasks::FinishCategory(Category& category, const char* unusedMessage)
{
    if(!category.task.IsValid())
    {
        return;
    }

    if(!category.roundFailure.empty())
    {
        category.task.Fail(category.roundFailure);
    }
    else
    {
        category.task.Complete(unusedMessage ? unusedMessage : "");
    }

    category.task = {};
    category.roundFailure.clear();
}

void DxrPreparationTasks::FinishBatch(bool unusedResults)
{
    const char* unusedMessage = unusedResults ? UnusedMessage : nullptr;
    FinishCategory(m_libraries, unusedMessage);
    FinishCategory(m_pipelines, unusedMessage);
    const std::string& failure = !m_libraries.failure.empty() ? m_libraries.failure : m_pipelines.failure;

    if(!failure.empty())
    {
        m_parent.Fail(failure);
    }
    else
    {
        m_parent.Complete(unusedMessage ? unusedMessage : "Ray tracing shaders and pipelines are ready.");
    }

    m_parent = {};
    m_libraries.failure.clear();
    m_pipelines.failure.clear();
}

void DxrPreparationTasks::Stop()
{
    // Work already presented must not be counted again by a later batch.
    m_libraries.baseSubmitted = m_seenLibrarySubmissions;
    m_libraries.baseTaken = m_seenLibraryTaken;
    m_pipelines.baseSubmitted = m_seenPipelineSubmissions;
    m_pipelines.baseTaken = m_seenPipelineTaken;

    for(Category* category : {&m_libraries, &m_pipelines})
    {
        if(category->task.IsValid())
        {
            category->task.MarkCancelled(ShutdownMessage);
            category->task = {};
        }

        category->failure.clear();
        category->roundFailure.clear();
    }

    if(m_parent.IsValid())
    {
        m_parent.MarkCancelled(ShutdownMessage);
        m_parent = {};
    }
}

void DxrPreparationTasks::Rebase(const AsyncWorkerActivity& libraries, const AsyncWorkerActivity& pipelines)
{
    m_seenLibrarySubmissions = libraries.submitted;
    m_seenPipelineSubmissions = pipelines.submitted;
    m_seenLibraryTaken = libraries.taken;
    m_seenPipelineTaken = pipelines.taken;
    m_libraries.baseSubmitted = libraries.submitted;
    m_libraries.baseTaken = libraries.taken;
    m_pipelines.baseSubmitted = pipelines.submitted;
    m_pipelines.baseTaken = pipelines.taken;
}
