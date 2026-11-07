#pragma once

#include "Core/BackgroundTasks.h"
#include "Renderer/AsyncWorkerActivity.h"

#include <cstdint>
#include <string>

// BT.3: presents the Renderer's asynchronous DXR preparation as one Background Tasks parent, "Preparing ray tracing
// shaders", with a child per worker that has work. It observes only: it never submits, takes, waits for or cancels a job,
// and it holds no Renderer reference. Main thread, once per Editor frame.
//
// A batch starts with the first submission either worker accepts and stays open until both workers are idle and every
// result has been taken by the Renderer, which is when the Renderer accepts or rejects it. Library acceptance submits
// the pipelines within the same Render call, so the batch never sees a gap between the two phases. A batch whose
// remaining results no gate wants any more (the request changed) ends on the first rendered frame in which no gate
// reported waiting. The workers serve every gate through one queue each, so concurrent requests (a lighting mode and a
// denoiser, say) share one batch: their jobs cannot be told apart.
class DxrPreparationTasks
{
public:
    // `report` is the frame's preparation report when Render returned Rendered, and null otherwise; only a rendered
    // frame can end a batch.
    void Update(BackgroundTaskManager& manager,
        const AsyncWorkerActivity& libraries,
        const AsyncWorkerActivity& pipelines,
        const DxrPreparationFrameReport* report);

    // Before the Renderer joins its workers: an open batch is marked cancelled, since its queued work is dropped.
    void Stop();

private:
    struct Category
    {
        const char* taskName;
        const char* activeVerb;
        BackgroundTaskHandle task;
        // The worker's counters when this round of the category began; a later round starts from where it ended.
        std::uint64_t baseSubmitted = 0;
        std::uint64_t baseTaken = 0;
        // The batch's first failure in this category, for the parent; and the current round's, for its child.
        std::string failure;
        std::string roundFailure;
    };

    void BeginBatch(BackgroundTaskManager& manager);
    void UpdateCategory(BackgroundTaskManager& manager, Category& category, const AsyncWorkerActivity& activity,
        const std::string* reportedFailure);
    void FinishCategory(Category& category, const char* unusedMessage);
    void FinishBatch(bool unusedResults);
    void Rebase(const AsyncWorkerActivity& libraries, const AsyncWorkerActivity& pipelines);

    BackgroundTaskHandle m_parent;
    Category m_libraries{"Compile shader libraries", "Compiling"};
    Category m_pipelines{"Create ray tracing pipelines", "Creating"};
    std::uint64_t m_seenLibrarySubmissions = 0;
    std::uint64_t m_seenPipelineSubmissions = 0;
    std::uint64_t m_seenLibraryTaken = 0;
    std::uint64_t m_seenPipelineTaken = 0;
    bool m_initialized = false;
    // Reused each frame so status text does not allocate once its capacity settles.
    std::string m_status;
};
