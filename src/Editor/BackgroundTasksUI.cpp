#include "BackgroundTasksUI.h"

#include "EditorWorkspace.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace
{
    constexpr const char* BarWindowName = "##BackgroundTaskBar";
    constexpr const char* PanelWindowName = "Background Tasks##Panel";
    constexpr const char* BarLabel = "Background Tasks";
    const ImVec4 FailureColor(1.0f, 0.42f, 0.38f, 1.0f);
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);
    const ImVec4 SuccessColor(0.45f, 0.85f, 0.45f, 1.0f);

    // The panel's size in font heights, so it follows any later UI scaling.
    constexpr float PanelWidthInFonts = 42.0f;
    constexpr float PanelHeightInFonts = 26.0f;
    constexpr float PanelMaximumWorkHeight = 0.6f;
    constexpr float BarProgressWidthInFonts = 8.0f;
    constexpr float BarMinimumNameWidthInFonts = 6.0f;

    void FormatProgress(const BackgroundTaskInfo& task, char* text, std::size_t size)
    {
        text[0] = '\0';

        switch(task.progressKind)
        {
        case BackgroundTaskProgressKind::WorkUnits:
            std::snprintf(text, size, "%llu / %llu", static_cast<unsigned long long>(task.completedUnits),
                static_cast<unsigned long long>(task.totalUnits));
            break;
        case BackgroundTaskProgressKind::Fraction:
            std::snprintf(text, size, "%.0f%%", static_cast<double>(task.measuredFraction) * 100.0);
            break;
        case BackgroundTaskProgressKind::Indeterminate:
            if(task.completedUnits > 0)
            {
                std::snprintf(text, size, "%llu", static_cast<unsigned long long>(task.completedUnits));
            }

            break;
        }
    }

    // An active task without a measurement animates; a finished one has nothing left to animate.
    void DrawProgress(const BackgroundTaskInfo& task, const ImVec2& size)
    {
        char overlay[64];
        FormatProgress(task, overlay, sizeof(overlay));
        const std::optional<float> fraction = task.GetFraction();

        if(fraction)
        {
            ImGui::ProgressBar(*fraction, size, overlay);
        }
        else if(task.IsActive())
        {
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), size, overlay);
        }
        else
        {
            ImGui::TextDisabled("%s", overlay[0] != '\0' ? overlay : "-");
        }
    }

    const char* DescribeState(const BackgroundTaskInfo& task)
    {
        if(task.finishing)
        {
            return "Waiting for children";
        }

        switch(task.state)
        {
        case BackgroundTaskState::Queued:
            return task.cancelRequested ? "Cancelling" : "Queued";
        case BackgroundTaskState::Running:
            return task.cancelRequested ? "Cancelling" : "Running";
        case BackgroundTaskState::Completed:
            return "Completed";
        case BackgroundTaskState::Failed:
            return "Failed";
        case BackgroundTaskState::Cancelled:
            return "Cancelled";
        }

        return "Unknown";
    }

    void DrawState(const BackgroundTaskInfo& task)
    {
        const char* text = DescribeState(task);

        if(task.state == BackgroundTaskState::Failed)
        {
            ImGui::TextColored(FailureColor, "%s", text);
        }
        else if(task.state == BackgroundTaskState::Cancelled || (task.IsActive() && task.childFailed))
        {
            ImGui::TextColored(WarningColor, "%s", text);
        }
        else if(task.state == BackgroundTaskState::Completed)
        {
            ImGui::TextColored(SuccessColor, "%s", text);
        }
        else
        {
            ImGui::TextUnformatted(text);
        }
    }

    void DrawElapsed(const BackgroundTaskInfo& task, BackgroundTaskInfo::Clock::time_point now)
    {
        if(!task.startedAt)
        {
            ImGui::TextDisabled("-");
            return;
        }

        const double seconds = std::chrono::duration<double>(task.GetElapsed(now)).count();

        if(seconds < 60.0)
        {
            ImGui::Text("%.1fs", seconds);
            return;
        }

        const auto whole = static_cast<unsigned long long>(seconds);
        ImGui::Text("%llu:%02llu:%02llu", whole / 3600, (whole / 60) % 60, whole % 60);
    }
}

void BackgroundTasksUI::TogglePanel() noexcept
{
    m_panelOpen = !m_panelOpen;

    if(m_panelOpen)
    {
        m_unreadFailure = false;
    }
}

BackgroundTasksUI::~BackgroundTasksUI()
{
    if(m_hookContext)
    {
        ImGui::RemoveContextHook(m_hookContext, m_endFrameHook);
        ImGui::RemoveContextHook(m_hookContext, m_shutdownHook);
    }
}

void BackgroundTasksUI::InstallLayerHooks()
{
    ImGuiContext* context = ImGui::GetCurrentContext();

    if(m_hookContext == context)
    {
        return;
    }

    if(m_hookContext)
    {
        ImGui::RemoveContextHook(m_hookContext, m_endFrameHook);
        ImGui::RemoveContextHook(m_hookContext, m_shutdownHook);
    }

    ImGuiContextHook hook;
    hook.Callback = &BackgroundTasksUI::OnContextHook;
    hook.UserData = this;
    hook.Type = ImGuiContextHookType_EndFramePost;
    m_endFrameHook = ImGui::AddContextHook(context, &hook);
    hook.Type = ImGuiContextHookType_Shutdown;
    m_shutdownHook = ImGui::AddContextHook(context, &hook);
    m_hookContext = context;
    m_panelWindow = nullptr;
}

void BackgroundTasksUI::OnContextHook(ImGuiContext* context, ImGuiContextHook* hook)
{
    auto* ui = static_cast<BackgroundTasksUI*>(hook->UserData);

    if(hook->Type == ImGuiContextHookType_Shutdown)
    {
        ui->m_hookContext = nullptr;
        ui->m_panelWindow = nullptr;
        return;
    }

    ui->KeepPanelLayer(*context);
}

// While open, the panel sits above every ordinary window and directly beneath the lowest open popup, modal or tooltip.
// This runs after EndFrame's click-to-focus and window sort, so a window raised this frame never covers the panel, even
// for one rendered frame, and next frame's hover search sees the same order. The panel moves together with its child
// windows, which follow it contiguously after the sort, so input inside it still reaches the innermost window. Keyboard
// focus (the nav window) is never changed: a clicked floating window keeps its focus while drawn beneath the panel.
void BackgroundTasksUI::KeepPanelLayer(ImGuiContext& context)
{
    ImGuiWindow* panel = m_panelWindow;

    if(!m_panelOpen || !panel || !panel->Active)
    {
        return;
    }

    ImVector<ImGuiWindow*>& windows = context.Windows;
    int panelBegin = 0;

    while(panelBegin < windows.Size && windows[panelBegin] != panel)
    {
        ++panelBegin;
    }

    if(panelBegin == windows.Size)
    {
        return;
    }

    int panelEnd = panelBegin + 1;

    while(panelEnd < windows.Size && windows[panelEnd]->RootWindow == panel)
    {
        ++panelEnd;
    }

    m_reorderedWindows.clear();
    std::size_t insertAt = NoTask;

    for(int index = 0; index < windows.Size; ++index)
    {
        if(index >= panelBegin && index < panelEnd)
        {
            continue;
        }

        ImGuiWindow* window = windows[index];
        const bool overlay = window->Active && !(window->Flags & ImGuiWindowFlags_ChildWindow) &&
            (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_Tooltip));

        if(overlay && insertAt == NoTask)
        {
            insertAt = m_reorderedWindows.size();
        }

        m_reorderedWindows.push_back(window);
    }

    if(insertAt == NoTask)
    {
        insertAt = m_reorderedWindows.size();
    }

    m_reorderedWindows.insert(m_reorderedWindows.begin() + static_cast<std::ptrdiff_t>(insertAt),
        windows.Data + panelBegin, windows.Data + panelEnd);
    std::copy(m_reorderedWindows.begin(), m_reorderedWindows.end(), windows.Data);
}

void BackgroundTasksUI::Draw(BackgroundTaskManager* manager, Clock::time_point now)
{
    InstallLayerHooks();
    RefreshSnapshot(manager);
    DrawBar(now);

    if(m_panelOpen)
    {
        DrawPanel(manager, now);
    }
}

void BackgroundTasksUI::RefreshSnapshot(BackgroundTaskManager* manager)
{
    if(!manager)
    {
        m_snapshot.reset();
        m_links.clear();
        m_rootOrder.clear();
        m_inactiveTaskCount = 0;
        return;
    }

    if(m_snapshot && m_snapshot->revision == manager->GetRevision())
    {
        return;
    }

    m_snapshot = manager->GetSnapshot();
    RebuildTaskOrder();
}

void BackgroundTasksUI::RebuildTaskOrder()
{
    const std::vector<BackgroundTaskInfo>& tasks = m_snapshot->tasks;
    m_links.assign(tasks.size(), {});
    m_rootOrder.clear();
    m_inactiveTaskCount = 0;
    std::size_t activeRoots = 0;
    Clock::time_point latestFailure = m_latestFailureSeen;

    // Walking backward links each parent's children in ascending ID order, and collects finished roots newest first.
    for(std::size_t index = tasks.size(); index-- > 0;)
    {
        const BackgroundTaskInfo& task = tasks[index];

        if(!task.IsActive())
        {
            ++m_inactiveTaskCount;
        }

        if(task.state == BackgroundTaskState::Failed && *task.finishedAt > m_latestFailureSeen)
        {
            latestFailure = (std::max)(latestFailure, *task.finishedAt);
        }

        if(task.parent == InvalidBackgroundTaskId)
        {
            if(task.IsActive())
            {
                ++activeRoots;
            }

            m_rootOrder.push_back(index);
            continue;
        }

        const auto parent = std::lower_bound(tasks.begin(), tasks.begin() + static_cast<std::ptrdiff_t>(index), task.parent,
            [](const BackgroundTaskInfo& candidate, BackgroundTaskId id)
            {
                return candidate.id < id;
            });
        const std::size_t parentIndex = static_cast<std::size_t>(parent - tasks.begin());
        m_links[index].nextSibling = m_links[parentIndex].firstChild;
        m_links[parentIndex].firstChild = index;
    }

    // Sticky, so a failure evicted from history before anyone opened the panel is still announced.
    if(latestFailure != m_latestFailureSeen)
    {
        m_latestFailureSeen = latestFailure;
        m_unreadFailure = m_unreadFailure || !m_panelOpen;
    }

    // Newest first so far. Stable partition keeps finished roots newest first; active roots then read oldest first.
    std::stable_partition(m_rootOrder.begin(), m_rootOrder.end(), [&tasks](std::size_t index)
    {
        return tasks[index].IsActive();
    });
    std::reverse(m_rootOrder.begin(), m_rootOrder.begin() + static_cast<std::ptrdiff_t>(activeRoots));
}

void BackgroundTasksUI::DrawBar(Clock::time_point now)
{
    const BackgroundTaskInfo* firstTask = nullptr;
    m_barTaskCount = 0;

    if(m_snapshot)
    {
        for(const std::size_t index : m_rootOrder)
        {
            const BackgroundTaskInfo& task = m_snapshot->tasks[index];

            if(!task.IsActive())
            {
                break;
            }

            if(now - task.createdAt >= TransientDelay)
            {
                firstTask = firstTask ? firstTask : &task;
                ++m_barTaskCount;
            }
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollWithMouse;

    if(ImGui::BeginViewportSideBar(BarWindowName, ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        const ImVec2 origin = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowWidth();
        const float height = ImGui::GetWindowHeight();

        if(width > 0.0f && height > 0.0f)
        {
            if(ImGui::InvisibleButton("##ToggleBackgroundTasks", ImVec2(width, height)))
            {
                TogglePanel();
            }

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const bool hovered = ImGui::IsItemHovered();

            if(hovered)
            {
                draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                    ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            }

            const ImGuiStyle& style = ImGui::GetStyle();
            const float labelWidth = ImGui::CalcTextSize(BarLabel).x;
            const float textY = origin.y + (height - ImGui::GetFontSize()) * 0.5f;
            const float labelX = origin.x + width - style.FramePadding.x - labelWidth;
            const ImU32 labelColor = ImGui::GetColorU32(m_barTaskCount > 0 ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            draw->AddText(ImVec2(labelX, textY), labelColor, BarLabel);
            float activityRight = labelX - style.ItemSpacing.x;

            if(m_unreadFailure)
            {
                const float radius = ImGui::GetFontSize() * 0.3f;
                activityRight -= radius * 2.0f;
                draw->AddCircleFilled(ImVec2(activityRight + radius, origin.y + height * 0.5f), radius,
                    ImGui::GetColorU32(FailureColor));
                activityRight -= style.ItemSpacing.x;

                if(hovered)
                {
                    ImGui::SetTooltip("A background task failed. Open the panel to review it.");
                }
            }

            if(firstTask)
            {
                DrawBarActivity(origin.x + style.FramePadding.x, activityRight, height, firstTask);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void BackgroundTasksUI::DrawBarActivity(float left, float right, float height, const BackgroundTaskInfo* firstTask)
{
    const float fontSize = ImGui::GetFontSize();
    const float origin = ImGui::GetWindowPos().y;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float progressWidth = fontSize * BarProgressWidthInFonts;
    const bool showProgress = right - left >= progressWidth + spacing + fontSize * BarMinimumNameWidthInFonts;
    const float nameRight = showProgress ? right - progressWidth - spacing : right;

    if(nameRight > left)
    {
        char summary[BackgroundTaskManager::MaximumNameBytes + 64];

        if(m_barTaskCount > 1)
        {
            std::snprintf(summary, sizeof(summary), "%zu tasks active: %s", m_barTaskCount, firstTask->name.c_str());
        }
        else
        {
            std::snprintf(summary, sizeof(summary), "%s", firstTask->name.c_str());
        }

        const float textY = origin + (height - fontSize) * 0.5f;
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(left, textY), ImVec2(nameRight, textY + fontSize),
            nameRight, summary, nullptr, nullptr);
    }

    if(!showProgress)
    {
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(right - progressWidth, origin + (height - fontSize) * 0.5f));

    // Unrelated percentages have no meaningful aggregate, so several tasks show activity without a value.
    if(m_barTaskCount > 1)
    {
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(progressWidth, fontSize), "");
    }
    else
    {
        DrawProgress(*firstTask, ImVec2(progressWidth, fontSize));
    }
}

void BackgroundTasksUI::DrawPanel(BackgroundTaskManager* manager, Clock::time_point now)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 size((std::min)(fontSize * PanelWidthInFonts, viewport->WorkSize.x),
        (std::min)(fontSize * PanelHeightInFonts, viewport->WorkSize.y * PanelMaximumWorkHeight));

    if(size.x <= 0.0f || size.y <= 0.0f)
    {
        return;
    }

    // The work area ends where the bar begins, and viewport size changes reach it before this frame's windows.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    // The default minimum would push a small window's panel out of the work area.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

    const bool visible = ImGui::Begin(PanelWindowName, nullptr, flags);
    m_panelWindow = ImGui::GetCurrentWindow();

    if(visible)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Background Tasks");
        const float minimizeWidth = ImGui::GetFrameHeight();
        const float clearWidth = ImGui::CalcTextSize("Clear Inactive").x + style.FramePadding.x * 2.0f;
        ImGui::SameLine((std::max)(0.0f,
            ImGui::GetContentRegionMax().x - minimizeWidth - style.ItemSpacing.x - clearWidth));
        ImGui::BeginDisabled(!manager || m_inactiveTaskCount == 0);

        if(ImGui::Button("Clear Inactive") && manager)
        {
            manager->ClearInactive();
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        if(ImGui::Button("_##MinimizeBackgroundTasks", ImVec2(minimizeWidth, 0.0f)))
        {
            m_panelOpen = false;
        }

        ImGui::Separator();

        if(ImGui::BeginChild("##BackgroundTaskRows"))
        {
            if(m_rootOrder.empty())
            {
                ImGui::TextDisabled("No background tasks.");
            }
            else if(ImGui::BeginTable("##BackgroundTaskTable", 5,
                ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, fontSize * 7.0f);
                ImGui::TableSetupColumn("Elapsed");
                ImGui::TableSetupColumn("##Action");

                for(const std::size_t index : m_rootOrder)
                {
                    DrawTaskRow(manager, index, now);
                }

                ImGui::EndTable();
            }
        }

        ImGui::EndChild();

        // Only with the panel focused and nothing else using the key: other Escape handlers act on their own operations,
        // none of which can be in progress here.
        const bool escapeIsFree = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

        if(escapeIsFree && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            m_panelOpen = false;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void BackgroundTasksUI::DrawTaskRow(BackgroundTaskManager* manager, std::size_t index, Clock::time_point now)
{
    const BackgroundTaskInfo& task = m_snapshot->tasks[index];
    const TaskLinks links = m_links[index];
    const bool hasChildren = links.firstChild != NoTask;
    ImGui::PushID(static_cast<int>(task.id));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

    if(!hasChildren)
    {
        nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
    }

    const bool expanded = ImGui::TreeNodeEx("##Task", nodeFlags, "%s", task.name.c_str());

    if(!task.status.empty())
    {
        ImGui::PushTextWrapPos(0.0f);

        if(task.state == BackgroundTaskState::Failed)
        {
            ImGui::TextColored(FailureColor, "%s", task.status.c_str());
        }
        else
        {
            ImGui::TextDisabled("%s", task.status.c_str());
        }

        ImGui::PopTextWrapPos();
    }

    ImGui::TableNextColumn();
    DrawState(task);
    ImGui::TableNextColumn();
    DrawProgress(task, ImVec2(-1.0f, ImGui::GetFontSize()));
    ImGui::TableNextColumn();
    DrawElapsed(task, now);
    ImGui::TableNextColumn();

    if(task.IsActive() && task.cancellable && !task.finishing)
    {
        ImGui::BeginDisabled(task.cancelRequested);

        if(ImGui::SmallButton("Cancel") && manager)
        {
            manager->RequestCancel(task.id);
        }

        ImGui::EndDisabled();
    }

    if(expanded)
    {
        for(std::size_t child = links.firstChild; child != NoTask; child = m_links[child].nextSibling)
        {
            DrawTaskRow(manager, child, now);
        }

        ImGui::TreePop();
    }

    ImGui::PopID();
}
