#pragma once

#include "Core/BackgroundTasks.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct ImGuiContext;
struct ImGuiContextHook;
struct ImGuiWindow;

// The Editor's permanent bottom bar and the Background Tasks panel anchored above it. A presenter only: it reads immutable
// manager snapshots and its one write is RequestCancel or ClearInactive. Closing the panel changes no task.
class BackgroundTasksUI
{
public:
    using Clock = BackgroundTaskInfo::Clock;

    // Root tasks younger than this stay out of the bar, though never out of the history. Presentation only.
    static constexpr std::chrono::milliseconds TransientDelay{250};

    BackgroundTasksUI() = default;
    // Registered ImGui hooks point at this object, so it neither copies nor moves.
    BackgroundTasksUI(const BackgroundTasksUI&) = delete;
    BackgroundTasksUI& operator=(const BackgroundTasksUI&) = delete;
    ~BackgroundTasksUI();

    // Call once per frame before the workspace lays out. The bar reserves its height through the viewport's work area,
    // which every workspace rectangle already derives from. A null manager shows an idle bar.
    void Draw(BackgroundTaskManager* manager, Clock::time_point now);

    bool IsPanelOpen() const noexcept
    {
        return m_panelOpen;
    }

    void TogglePanel() noexcept;

    bool HasUnreadFailure() const noexcept
    {
        return m_unreadFailure;
    }

    // Active root tasks the bar shows as of the last Draw.
    std::size_t GetBarTaskCount() const noexcept
    {
        return m_barTaskCount;
    }

private:
    // Links into the current snapshot's task array, rebuilt only when its revision changes.
    struct TaskLinks
    {
        std::size_t firstChild = NoTask;
        std::size_t nextSibling = NoTask;
    };

    static constexpr std::size_t NoTask = static_cast<std::size_t>(-1);

    void RefreshSnapshot(BackgroundTaskManager* manager);
    void RebuildTaskOrder();
    void DrawBar(Clock::time_point now);
    void DrawBarActivity(float left, float right, float height, const BackgroundTaskInfo* firstTask);
    void DrawPanel(BackgroundTaskManager* manager, Clock::time_point now);
    void DrawTaskRow(BackgroundTaskManager* manager, std::size_t index, Clock::time_point now);

    std::shared_ptr<const BackgroundTaskSnapshot> m_snapshot;
    std::vector<TaskLinks> m_links;
    // Active tasks oldest first, then finished tasks newest first.
    std::vector<std::size_t> m_rootOrder;
    std::size_t m_inactiveTaskCount = 0;
    std::size_t m_barTaskCount = 0;
    // By finish time, not ID: an older task can fail after a newer one.
    Clock::time_point m_latestFailureSeen{};
    bool m_panelOpen = false;
    bool m_unreadFailure = false;

    // Display order, enforced after ImGui's own end-of-frame focus changes and window sort; see KeepPanelLayer.
    static void OnContextHook(ImGuiContext* context, ImGuiContextHook* hook);
    void InstallLayerHooks();
    void KeepPanelLayer(ImGuiContext& context);

    // Null once ImGui's shutdown hook reports the context gone, so the destructor never touches a destroyed context.
    ImGuiContext* m_hookContext = nullptr;
    unsigned int m_endFrameHook = 0;
    unsigned int m_shutdownHook = 0;
    // The panel's window this frame; ImGui keeps window objects for the context's lifetime.
    ImGuiWindow* m_panelWindow = nullptr;
    // Reused each frame so reordering does not allocate.
    std::vector<ImGuiWindow*> m_reorderedWindows;
};
