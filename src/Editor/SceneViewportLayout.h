#pragma once

#include "EditorWorkspace.h"
#include <algorithm>

struct SceneViewportRegions
{
    EditorWorkspaceRect toolbar;
    EditorWorkspaceRect imageArea;
};

inline SceneViewportRegions CalculateSceneViewportRegions(const EditorWorkspaceRect& viewport, float toolbarHeight)
{
    const float reservedHeight = (std::clamp)(toolbarHeight, 0.0f, (std::max)(0.0f, viewport.size.y));
    SceneViewportRegions regions;
    regions.toolbar = {viewport.position, ImVec2(viewport.size.x, reservedHeight)};
    regions.imageArea = {ImVec2(viewport.position.x, viewport.position.y + reservedHeight),
        ImVec2(viewport.size.x, (std::max)(0.0f, viewport.size.y - reservedHeight))};
    return regions;
}

inline EditorWorkspaceRect FitSceneViewportImage(const EditorWorkspaceRect& imageArea, ImVec2 logicalImageSize)
{
    if(imageArea.size.x <= 0.0f || imageArea.size.y <= 0.0f || logicalImageSize.x <= 0.0f || logicalImageSize.y <= 0.0f)
    {
        return {};
    }
    const float imageScale = (std::min)(imageArea.size.x / logicalImageSize.x, imageArea.size.y / logicalImageSize.y);
    const ImVec2 imageSize(logicalImageSize.x * imageScale, logicalImageSize.y * imageScale);
    const ImVec2 imagePosition(imageArea.position.x + (imageArea.size.x - imageSize.x) * 0.5f,
        imageArea.position.y + (imageArea.size.y - imageSize.y) * 0.5f);
    return {imagePosition, imageSize};
}
