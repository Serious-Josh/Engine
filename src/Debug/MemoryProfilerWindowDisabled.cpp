#include "Debug/MemoryProfilerWindow.h"

// Separate library member from MemoryRegistryDisabled.cpp; keep its function set identical to MemoryProfilerWindow.cpp.
// Packaged games and tools link only these: no Memory UI, no history, no peak logic.

namespace MemoryProfilerWindow
{
    void DrawMenuItem()
    {
    }

    void Draw()
    {
    }

    bool IsVisible()
    {
        return false;
    }

    void SetVisible(bool)
    {
    }

    MemoryProfilerModel* ModelForTesting()
    {
        return nullptr;
    }
}
