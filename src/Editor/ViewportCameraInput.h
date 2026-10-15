#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace ViewportCameraInput
{
    inline bool HasCapture()
    {
        const auto window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        return window && GetCapture() == window;
    }

    inline bool UpdateFocus(bool focused, bool navigating, bool hovered)
    {
        // Hover gates acquisition only. A second button cannot revoke an owned gesture.
        if(!navigating && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        {
            return hovered;
        }
        return focused;
    }

    inline void ReserveLeftButton(bool navigating, bool panLeftHeld)
    {
        const auto& io = ImGui::GetIO();
        if(!io.AppFocusLost && HasCapture() &&
            ((navigating && io.MouseDown[ImGuiMouseButton_Right]) ||
                (panLeftHeld && io.MouseDown[ImGuiMouseButton_Left])))
        {
            // Run before panels/splitters so LMB cannot start another operation during RMB navigation.
            // LockThisFrame is renewed while owned; no competing Win32 capture or cursor counter.
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImHashStr("ViewportCameraMouse"), ImGuiInputFlags_LockThisFrame);
        }
    }
}
