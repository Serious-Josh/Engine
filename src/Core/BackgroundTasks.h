#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tracks work scheduled elsewhere; it never starts, owns, waits for or cancels that work. It stores plain data only (no
// callbacks, job pointers or scene references), so no task can reach a destroyed object. Every member is thread-safe.
//
// Lifecycle invariants:
// - Queued -> Running -> Completed | Failed | Cancelled. Any active state may finish; a finished state never changes.
// - A finished task's descendants have all finished. A parent asked to finish while children are active defers: it stays
//   active with `finishing` set, accepts no further updates or children, and finishes when its last child does.
// - The final outcome is the worst of the owner's request and the children's outcomes: Failed, then Cancelled, then
//   Completed. Children never finish a parent the owner has not asked to finish, because more children may still follow.
// - A parent must exist and be accepting children when its child begins, and it never changes, so cycles cannot form.
enum class BackgroundTaskState
{
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled
};

enum class BackgroundTaskProgressKind
{
    // No measurable total. `completedUnits` may still hold a running count.
    Indeterminate,
    WorkUnits,
    Fraction
};

using BackgroundTaskId = std::uint64_t;

// Never assigned to a task; as a parent, it means the task is a root.
inline constexpr BackgroundTaskId InvalidBackgroundTaskId = 0;

struct BackgroundTaskOptions
{
    std::string_view name;
    // Whether the owner honours cancellation requests. It does not limit the owner's own MarkCancelled.
    bool cancellable = false;
    BackgroundTaskId parent = InvalidBackgroundTaskId;
};

struct BackgroundTaskInfo
{
    using Clock = std::chrono::steady_clock;

    BackgroundTaskId id = InvalidBackgroundTaskId;
    BackgroundTaskId parent = InvalidBackgroundTaskId;
    std::string name;
    std::string status;
    BackgroundTaskState state = BackgroundTaskState::Queued;
    BackgroundTaskProgressKind progressKind = BackgroundTaskProgressKind::Indeterminate;
    std::uint64_t completedUnits = 0;
    std::uint64_t totalUnits = 0;
    float measuredFraction = 0.0f;
    bool cancellable = false;
    bool cancelRequested = false;
    bool finishing = false;
    // Sticky, so clearing or evicting a child never hides what happened to it.
    bool childFailed = false;
    bool childCancelled = false;
    std::size_t activeChildren = 0;
    Clock::time_point createdAt{};
    std::optional<Clock::time_point> startedAt;
    std::optional<Clock::time_point> finishedAt;

    bool IsActive() const noexcept
    {
        return state == BackgroundTaskState::Queued || state == BackgroundTaskState::Running;
    }

    // This task's own measurement; never derived from its children. Empty when indeterminate.
    std::optional<float> GetFraction() const noexcept;

    // Zero until started; stops at the finish.
    Clock::duration GetElapsed(Clock::time_point now) const noexcept;
};

// Immutable once published. Tasks are in ascending ID order, so a parent always precedes its children.
struct BackgroundTaskSnapshot
{
    // Changes only when visible task data changes.
    std::uint64_t revision = 0;
    std::vector<BackgroundTaskInfo> tasks;
};

struct BackgroundTaskManagerState;

// Move-only owner side of one task. It holds the manager weakly: every call is a harmless no-op once the manager or the
// task is gone. Destroying it before the task was asked to finish fails the task visibly, so no task is abandoned.
// Setters return whether the call was accepted; a rejected call changes nothing.
class BackgroundTaskHandle
{
public:
    BackgroundTaskHandle() = default;
    BackgroundTaskHandle(const BackgroundTaskHandle&) = delete;
    BackgroundTaskHandle& operator=(const BackgroundTaskHandle&) = delete;
    BackgroundTaskHandle(BackgroundTaskHandle&& other) noexcept;
    BackgroundTaskHandle& operator=(BackgroundTaskHandle&& other);
    ~BackgroundTaskHandle();

    // False for an empty handle, including a rejected Begin. Stays true after the task finishes.
    bool IsValid() const noexcept
    {
        return m_id != InvalidBackgroundTaskId;
    }

    BackgroundTaskId GetId() const noexcept
    {
        return m_id;
    }

    // Queued to Running only.
    bool Start();

    // A total of zero means indeterminate. Rejects completed > total, keeping the last valid value.
    bool SetProgress(std::uint64_t completed, std::uint64_t total);
    // Rejects anything but a finite value in [0, 1], keeping the last valid value.
    bool SetFraction(float fraction);
    bool SetIndeterminate();
    bool SetStatus(std::string_view status);

    bool IsCancelRequested() const;

    // The first call decides the request; later calls are ignored. A non-empty message replaces the status.
    bool Complete(std::string_view message = {});
    bool Fail(std::string_view message);
    bool MarkCancelled(std::string_view message = {});

private:
    friend class BackgroundTaskManager;

    BackgroundTaskHandle(std::weak_ptr<BackgroundTaskManagerState> state, BackgroundTaskId id) noexcept;

    void AbandonIfUnfinished();

    std::weak_ptr<BackgroundTaskManagerState> m_state;
    BackgroundTaskId m_id = InvalidBackgroundTaskId;
};

struct BackgroundTaskRetention
{
    // Finished tasks kept at the root, and under any one parent. Values below one are treated as one.
    std::size_t rootTasks = 64;
    std::size_t childrenPerParent = 64;
};

class BackgroundTaskManager
{
public:
    static constexpr std::size_t MaximumNameBytes = 128;
    static constexpr std::size_t MaximumStatusBytes = 256;

    explicit BackgroundTaskManager(BackgroundTaskRetention retention = {});
    BackgroundTaskManager(const BackgroundTaskManager&) = delete;
    BackgroundTaskManager& operator=(const BackgroundTaskManager&) = delete;
    ~BackgroundTaskManager();

    // Starts Queued. Returns an empty handle for an empty name, or for a parent that is unknown, finished or finishing.
    BackgroundTaskHandle Begin(const BackgroundTaskOptions& options);

    // Only sets the flag the owner reads; the task stays active until its owner finishes it. Applies to this task alone:
    // children and siblings are unaffected. False when the task is unknown, not cancellable, finished or finishing.
    bool RequestCancel(BackgroundTaskId id);

    // Removes every finished task. Active tasks, and so any task with an active descendant, always remain.
    void ClearInactive();

    std::uint64_t GetRevision() const;

    // Never null. Rebuilt only when the revision has changed, and safe to keep after the manager is gone.
    std::shared_ptr<const BackgroundTaskSnapshot> GetSnapshot() const;

private:
    std::shared_ptr<BackgroundTaskManagerState> m_state;
};
