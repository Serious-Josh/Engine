#pragma once

class MemoryProfilerModel;

// M0b.1 P3 Editor Memory window. Same build boundary as MemoryRegistry.h: only the Editor links
// MemoryProfilerWindow.cpp; every other EngineCore consumer resolves the empty functions in MemoryRegistryDisabled.cpp,
// so packaged games contain no Memory UI, no history storage and no peak logic.
namespace MemoryProfilerWindow
{
    // Inside the Debug menu: the "Memory" toggle.
    void DrawMenuItem();
    // Once per UI frame, before ImGui::Render: consumes a new monitor sample (if any), then draws the window when visible.
    void Draw();

    bool IsVisible();
    void SetVisible(bool visible);

    // Verification access to the window's model; null in builds with the disabled window.
    MemoryProfilerModel* ModelForTesting();
}
