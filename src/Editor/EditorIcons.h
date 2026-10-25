#pragma once

#include "Renderer/Texture.h"
#include "Scene/LightComponent.h"
#include "imgui.h"
#include <array>
#include <filesystem>

enum class EditorIcon : unsigned int
{
    Box, Camera, File, Folder, Light, Spot, Point, Directional, Count
};

constexpr EditorIcon SceneObjectIcon(bool camera, const LightComponent* light)
{
    if(camera)
    {
        return EditorIcon::Camera;
    }

    if(!light)
    {
        return EditorIcon::Box;
    }

    switch(light->type)
    {
    case LightType::Point:
        return EditorIcon::Point;
    case LightType::Spot:
        return EditorIcon::Spot;
    case LightType::Directional:
        return EditorIcon::Directional;
    default:
        return EditorIcon::Light;
    }
}

class DescriptorHeap;
class EditorIcons
{
public:
    EditorIcons() = default;
    ~EditorIcons()
    {
        Shutdown();
    }
    EditorIcons(const EditorIcons&) = delete;
    EditorIcons& operator=(const EditorIcons&) = delete;
    // Initialization only; one bounded atlas/upload, no per-frame parsing or allocation.
    void Initialize(ID3D12Device* device, DescriptorHeap& heap, const std::filesystem::path& directory);
    void Prepare(ID3D12GraphicsCommandList* commands);
    // Caller must drain the GPU before shutdown, just as for DebugUI and its descriptors.
    void Shutdown();
    // Draw-list decoration only: never submits an ImGui item or changes the last item.
    void Draw(ImDrawList& draw, EditorIcon icon, ImVec2 origin, ImVec2 bounds, ImU32 tint) const;
    ImVec2 GetSize(EditorIcon icon, float maximumDimension) const;

private:
    static constexpr unsigned int Count = static_cast<unsigned int>(EditorIcon::Count);
    static constexpr unsigned int Cell = 100, RasterSize = 96;
    struct Entry
    {
        unsigned int width = 0;
        unsigned int height = 0;
    };
    std::array<Entry, Count> m_entries{};
    Texture m_atlas;
    DescriptorHeap* m_heap = nullptr;
};
