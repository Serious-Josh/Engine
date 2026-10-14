#pragma once

#include "imgui.h"

struct EditorWorkspaceRect
{
    ImVec2 position{};
    ImVec2 size{};
};

// Owns layout only; scene rendering and input still use their existing coordinates.
class EditorWorkspace
{
public:
    void Update();

    // Call inside a fixed panel's Begin/End scope, never from floating UI.
    static void KeepCurrentWindowBehindFloatingUI();

    bool IsSplitterDragging() const
    {
        return m_splitterDragging;
    }

    bool& HierarchyVisible()
    {
        return m_hierarchyVisible;
    }

    bool& InspectorVisible()
    {
        return m_inspectorVisible;
    }
    bool& AssetBrowserVisible()
    {
        return m_assetBrowserVisible;
    }

    const EditorWorkspaceRect& GetWorkspace() const
    {
        return m_workspace;
    }
    const EditorWorkspaceRect& GetSceneViewport() const
    {
        return m_sceneViewport;
    }
    const EditorWorkspaceRect& GetHierarchy() const
    {
        return m_hierarchy;
    }

    const EditorWorkspaceRect& GetInspector() const
    {
        return m_inspector;
    }
    const EditorWorkspaceRect& GetAssetBrowser() const
    {
        return m_assetBrowser;
    }

private:
    void Calculate();

    float m_hierarchyWidth = 240.0f;
    float m_inspectorWidth = 320.0f;
    float m_assetBrowserHeight = 260.0f;

    bool m_hierarchyVisible = true;
    bool m_inspectorVisible = true;
    bool m_assetBrowserVisible = true;
    bool m_splitterDragging = false;

    EditorWorkspaceRect m_workspace;
    EditorWorkspaceRect m_sceneViewport;
    EditorWorkspaceRect m_hierarchy;
    EditorWorkspaceRect m_inspector;
    EditorWorkspaceRect m_assetBrowser;

    EditorWorkspaceRect m_hierarchySplitter;
    EditorWorkspaceRect m_verticalSplitter;
    EditorWorkspaceRect m_horizontalSplitter;
};
