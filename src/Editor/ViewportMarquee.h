#pragma once

#include "SceneSelection.h"
#include "SceneViewportLayout.h"
#include "Renderer/Mesh.h"
#include "Math/Matrix4.h"
#include <span>
#include <cstdint>
#include <unordered_map>

namespace ViewportMarquee
{
struct Rectangle
{
    ImVec2 minimum;
    ImVec2 maximum;
};
enum class SelectionMode { Replace, Add, Toggle };
Rectangle Normalize(ImVec2 anchor, ImVec2 cursor);
bool Overlaps(const Rectangle& first, const Rectangle& second);
bool ProjectBounds(const MeshBounds&, const Matrix4& clipFromLocal, ImVec2 imageSize, Rectangle& output);
void CombineSelection(std::span<const SceneSelection::Identity> original,
    std::span<const SceneSelection::Identity> candidates, SelectionMode, std::vector<SceneSelection::Identity>& output);

struct Gesture
{
    bool active = false;
    ImVec2 anchor;
    ImVec2 cursor;
    EditorWorkspaceRect image;
    const void* sceneIdentity = nullptr; // Comparison only; never dereferenced.
    std::uint64_t structureRevision = 0;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t rendererLifetime = 0;
    std::uint64_t bindingEpoch = 0;
    SelectionMode mode = SelectionMode::Replace;
    bool preserveEmptyClick = false;
    // Stable-ID/index map belongs to the unchanged scene/structure captured at mouse-down.
    std::vector<SceneSelection::Identity> objectIdentities;
    std::unordered_map<std::string, std::size_t> persistentIndices;
    std::vector<SceneSelection::Identity> originalSelection;
    std::vector<SceneSelection::Identity> candidates;
    std::vector<SceneSelection::Identity> previewSelection;
    std::vector<std::size_t> previewIndices;
};
}
