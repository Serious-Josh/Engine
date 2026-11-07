#include "BackgroundTasks.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace
{
    constexpr std::string_view AbandonedMessage = "The owner stopped tracking this task before it finished.";
    constexpr std::string_view TruncationMarker = "...";

    bool IsFinishedState(BackgroundTaskState state) noexcept
    {
        return state != BackgroundTaskState::Queued && state != BackgroundTaskState::Running;
    }

    // Cuts on a UTF-8 character boundary so a bounded string stays displayable.
    std::string_view BoundedPrefix(std::string_view text, std::size_t maximumBytes) noexcept
    {
        std::size_t length = maximumBytes;

        while(length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        {
            --length;
        }

        return text.substr(0, length);
    }

    // Returns whether the stored text changed.
    bool AssignBounded(std::string& target, std::string_view text, std::size_t maximumBytes)
    {
        if(text.size() <= maximumBytes)
        {
            if(target == text)
            {
                return false;
            }

            target.assign(text);
            return true;
        }

        std::string bounded(BoundedPrefix(text, maximumBytes - TruncationMarker.size()));
        bounded += TruncationMarker;

        if(target == bounded)
        {
            return false;
        }

        target = std::move(bounded);
        return true;
    }

    struct TaskRecord
    {
        BackgroundTaskInfo info;
        // The owner's deferred finish request; `info.finishing` publishes that one exists.
        BackgroundTaskState requestedOutcome = BackgroundTaskState::Completed;
        std::string requestedMessage;
        std::string firstFailedChild;
        std::string firstCancelledChild;
    };
}

std::optional<float> BackgroundTaskInfo::GetFraction() const noexcept
{
    switch(progressKind)
    {
    case BackgroundTaskProgressKind::WorkUnits:
        return static_cast<float>(static_cast<double>(completedUnits) / static_cast<double>(totalUnits));
    case BackgroundTaskProgressKind::Fraction:
        return measuredFraction;
    case BackgroundTaskProgressKind::Indeterminate:
        break;
    }

    return std::nullopt;
}

BackgroundTaskInfo::Clock::duration BackgroundTaskInfo::GetElapsed(Clock::time_point now) const noexcept
{
    if(!startedAt)
    {
        return Clock::duration::zero();
    }

    return finishedAt.value_or(now) - *startedAt;
}

struct BackgroundTaskManagerState
{
    explicit BackgroundTaskManagerState(const BackgroundTaskRetention& limits) :
        retention{(std::max)(limits.rootTasks, std::size_t{1}), (std::max)(limits.childrenPerParent, std::size_t{1})}
    {
    }

    // Every member below, and every helper, requires the mutex.
    mutable std::mutex mutex;
    const BackgroundTaskRetention retention;
    // Ascending ID order, which creation order maintains.
    std::vector<TaskRecord> tasks;
    BackgroundTaskId nextId = 1;
    std::uint64_t revision = 0;
    mutable std::shared_ptr<const BackgroundTaskSnapshot> publishedSnapshot;

    TaskRecord* Find(BackgroundTaskId id) noexcept
    {
        const auto found = std::lower_bound(tasks.begin(), tasks.end(), id,
            [](const TaskRecord& record, BackgroundTaskId value)
            {
                return record.info.id < value;
            });

        return found != tasks.end() && found->info.id == id ? &*found : nullptr;
    }

    // The task when its owner may still update it: known, unfinished and not finishing.
    TaskRecord* FindUpdatable(BackgroundTaskId id) noexcept
    {
        TaskRecord* record = Find(id);
        return record && record->info.IsActive() && !record->info.finishing ? record : nullptr;
    }

    bool RequestFinish(BackgroundTaskId id, BackgroundTaskState outcome, std::string_view message)
    {
        TaskRecord* record = FindUpdatable(id);

        if(!record)
        {
            return false;
        }

        record->requestedOutcome = outcome;
        AssignBounded(record->requestedMessage, message, BackgroundTaskManager::MaximumStatusBytes);

        if(record->info.activeChildren > 0)
        {
            record->info.finishing = true;
            ++revision;
            return true;
        }

        FinishAndPropagate(id);
        return true;
    }

    // Finishes the task, then any ancestors that were only waiting for it.
    void FinishAndPropagate(BackgroundTaskId id)
    {
        std::vector<BackgroundTaskId> affectedGroups;
        BackgroundTaskId current = id;

        while(current != InvalidBackgroundTaskId)
        {
            TaskRecord& record = *Find(current);
            ApplyOutcome(record);
            const BackgroundTaskId parentId = record.info.parent;
            affectedGroups.push_back(parentId);

            if(parentId == InvalidBackgroundTaskId)
            {
                break;
            }

            // An active child's parent is active, and so cannot have been cleared or evicted.
            TaskRecord& parent = *Find(parentId);
            --parent.info.activeChildren;

            if(record.info.state == BackgroundTaskState::Failed)
            {
                parent.info.childFailed = true;

                if(parent.firstFailedChild.empty())
                {
                    parent.firstFailedChild = record.info.name;
                }
            }
            else if(record.info.state == BackgroundTaskState::Cancelled)
            {
                parent.info.childCancelled = true;

                if(parent.firstCancelledChild.empty())
                {
                    parent.firstCancelledChild = record.info.name;
                }
            }

            current = parent.info.finishing && parent.info.activeChildren == 0 ? parentId : InvalidBackgroundTaskId;
        }

        ++revision;

        // Deepest first: evicting from a higher group may remove a lower one entirely, never the reverse.
        for(const BackgroundTaskId group : affectedGroups)
        {
            EnforceRetention(group);
        }
    }

    void ApplyOutcome(TaskRecord& record)
    {
        BackgroundTaskInfo& info = record.info;
        BackgroundTaskState outcome = record.requestedOutcome;
        std::string message = std::move(record.requestedMessage);

        // Failure outranks cancellation, which outranks success. The child is named when it changed the owner's outcome.
        if(info.childFailed && outcome != BackgroundTaskState::Failed)
        {
            outcome = BackgroundTaskState::Failed;
            message = "Child task failed: " + record.firstFailedChild;
        }
        else if(info.childCancelled && outcome == BackgroundTaskState::Completed)
        {
            outcome = BackgroundTaskState::Cancelled;
            message = "Child task cancelled: " + record.firstCancelledChild;
        }

        info.state = outcome;
        info.finishing = false;
        info.finishedAt = BackgroundTaskInfo::Clock::now();

        if(!message.empty())
        {
            AssignBounded(info.status, message, BackgroundTaskManager::MaximumStatusBytes);
        }
    }

    void EnforceRetention(BackgroundTaskId group)
    {
        const std::size_t limit =
            group == InvalidBackgroundTaskId ? retention.rootTasks : retention.childrenPerParent;

        while(true)
        {
            std::size_t finished = 0;
            const TaskRecord* oldest = nullptr;
            const auto isBetterEviction = [](const TaskRecord& candidate, const TaskRecord& current)
            {
                // Failures outlast everything else, since they are what a reader most needs to find.
                const bool candidateFailed = candidate.info.state == BackgroundTaskState::Failed;
                const bool currentFailed = current.info.state == BackgroundTaskState::Failed;

                if(candidateFailed != currentFailed)
                {
                    return currentFailed;
                }

                return *candidate.info.finishedAt < *current.info.finishedAt;
            };

            for(const TaskRecord& record : tasks)
            {
                if(record.info.parent != group || record.info.IsActive())
                {
                    continue;
                }

                ++finished;

                if(!oldest || isBetterEviction(record, *oldest))
                {
                    oldest = &record;
                }
            }

            if(finished <= limit)
            {
                return;
            }

            RemoveTree(oldest->info.id);
        }
    }

    // A finished task's descendants have all finished. Parents precede children, so one pass finds the whole tree.
    void RemoveTree(BackgroundTaskId root)
    {
        std::vector<BackgroundTaskId> removed{root};

        for(const TaskRecord& record : tasks)
        {
            if(std::binary_search(removed.begin(), removed.end(), record.info.parent))
            {
                removed.push_back(record.info.id);
            }
        }

        std::erase_if(tasks, [&removed](const TaskRecord& record)
        {
            return std::binary_search(removed.begin(), removed.end(), record.info.id);
        });
    }
};

BackgroundTaskHandle::BackgroundTaskHandle(
    std::weak_ptr<BackgroundTaskManagerState> state,
    BackgroundTaskId id) noexcept :
    m_state(std::move(state)),
    m_id(id)
{
}

BackgroundTaskHandle::BackgroundTaskHandle(BackgroundTaskHandle&& other) noexcept :
    m_state(std::move(other.m_state)),
    m_id(std::exchange(other.m_id, InvalidBackgroundTaskId))
{
}

BackgroundTaskHandle& BackgroundTaskHandle::operator=(BackgroundTaskHandle&& other)
{
    if(this != &other)
    {
        AbandonIfUnfinished();
        m_state = std::move(other.m_state);
        m_id = std::exchange(other.m_id, InvalidBackgroundTaskId);
    }

    return *this;
}

BackgroundTaskHandle::~BackgroundTaskHandle()
{
    AbandonIfUnfinished();
}

void BackgroundTaskHandle::AbandonIfUnfinished()
{
    Fail(AbandonedMessage);
}

bool BackgroundTaskHandle::Start()
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    TaskRecord* record = state->FindUpdatable(m_id);

    if(!record || record->info.state != BackgroundTaskState::Queued)
    {
        return false;
    }

    record->info.state = BackgroundTaskState::Running;
    record->info.startedAt = BackgroundTaskInfo::Clock::now();
    ++state->revision;
    return true;
}

bool BackgroundTaskHandle::SetProgress(std::uint64_t completed, std::uint64_t total)
{
    if(total > 0 && completed > total)
    {
        return false;
    }

    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    TaskRecord* record = state->FindUpdatable(m_id);

    if(!record)
    {
        return false;
    }

    BackgroundTaskInfo& info = record->info;
    const BackgroundTaskProgressKind kind =
        total > 0 ? BackgroundTaskProgressKind::WorkUnits : BackgroundTaskProgressKind::Indeterminate;

    if(info.progressKind != kind || info.completedUnits != completed || info.totalUnits != total)
    {
        info.progressKind = kind;
        info.completedUnits = completed;
        info.totalUnits = total;
        ++state->revision;
    }

    return true;
}

bool BackgroundTaskHandle::SetFraction(float fraction)
{
    if(!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
    {
        return false;
    }

    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    TaskRecord* record = state->FindUpdatable(m_id);

    if(!record)
    {
        return false;
    }

    BackgroundTaskInfo& info = record->info;

    if(info.progressKind != BackgroundTaskProgressKind::Fraction || info.measuredFraction != fraction)
    {
        info.progressKind = BackgroundTaskProgressKind::Fraction;
        info.completedUnits = 0;
        info.totalUnits = 0;
        info.measuredFraction = fraction;
        ++state->revision;
    }

    return true;
}

bool BackgroundTaskHandle::SetIndeterminate()
{
    return SetProgress(0, 0);
}

bool BackgroundTaskHandle::SetStatus(std::string_view status)
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    TaskRecord* record = state->FindUpdatable(m_id);

    if(!record)
    {
        return false;
    }

    if(AssignBounded(record->info.status, status, BackgroundTaskManager::MaximumStatusBytes))
    {
        ++state->revision;
    }

    return true;
}

bool BackgroundTaskHandle::IsCancelRequested() const
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    const TaskRecord* record = state->Find(m_id);
    return record && record->info.cancelRequested;
}

bool BackgroundTaskHandle::Complete(std::string_view message)
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    return state->RequestFinish(m_id, BackgroundTaskState::Completed, message);
}

bool BackgroundTaskHandle::Fail(std::string_view message)
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    return state->RequestFinish(m_id, BackgroundTaskState::Failed, message);
}

bool BackgroundTaskHandle::MarkCancelled(std::string_view message)
{
    const auto state = m_state.lock();

    if(!state)
    {
        return false;
    }

    std::lock_guard lock(state->mutex);
    return state->RequestFinish(m_id, BackgroundTaskState::Cancelled, message);
}

BackgroundTaskManager::BackgroundTaskManager(BackgroundTaskRetention retention) :
    m_state(std::make_shared<BackgroundTaskManagerState>(retention))
{
}

BackgroundTaskManager::~BackgroundTaskManager()
{
    // A handle on another thread may briefly keep the state alive past this point. Emptying it makes every later call a
    // rejected no-op at once, instead of depending on when that last reference drops.
    std::lock_guard lock(m_state->mutex);
    m_state->tasks.clear();
    m_state->publishedSnapshot.reset();
}

BackgroundTaskHandle BackgroundTaskManager::Begin(const BackgroundTaskOptions& options)
{
    if(options.name.empty())
    {
        return {};
    }

    std::lock_guard lock(m_state->mutex);

    if(options.parent != InvalidBackgroundTaskId)
    {
        TaskRecord* parent = m_state->FindUpdatable(options.parent);

        if(!parent)
        {
            return {};
        }

        ++parent->info.activeChildren;
    }

    TaskRecord record;
    record.info.id = m_state->nextId++;
    record.info.parent = options.parent;
    record.info.cancellable = options.cancellable;
    record.info.createdAt = BackgroundTaskInfo::Clock::now();
    AssignBounded(record.info.name, options.name, MaximumNameBytes);
    const BackgroundTaskId id = record.info.id;
    m_state->tasks.push_back(std::move(record));
    ++m_state->revision;
    return BackgroundTaskHandle(m_state, id);
}

bool BackgroundTaskManager::RequestCancel(BackgroundTaskId id)
{
    std::lock_guard lock(m_state->mutex);
    TaskRecord* record = m_state->FindUpdatable(id);

    if(!record || !record->info.cancellable)
    {
        return false;
    }

    if(!record->info.cancelRequested)
    {
        record->info.cancelRequested = true;
        ++m_state->revision;
    }

    return true;
}

void BackgroundTaskManager::ClearInactive()
{
    std::lock_guard lock(m_state->mutex);
    const std::size_t removed = std::erase_if(m_state->tasks, [](const TaskRecord& record)
    {
        return IsFinishedState(record.info.state);
    });

    if(removed > 0)
    {
        ++m_state->revision;
    }
}

std::uint64_t BackgroundTaskManager::GetRevision() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->revision;
}

std::shared_ptr<const BackgroundTaskSnapshot> BackgroundTaskManager::GetSnapshot() const
{
    std::lock_guard lock(m_state->mutex);

    if(!m_state->publishedSnapshot || m_state->publishedSnapshot->revision != m_state->revision)
    {
        auto snapshot = std::make_shared<BackgroundTaskSnapshot>();
        snapshot->revision = m_state->revision;
        snapshot->tasks.reserve(m_state->tasks.size());

        for(const TaskRecord& record : m_state->tasks)
        {
            snapshot->tasks.push_back(record.info);
        }

        m_state->publishedSnapshot = std::move(snapshot);
    }

    return m_state->publishedSnapshot;
}
