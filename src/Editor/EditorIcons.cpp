#include "EditorIcons.h"
#include "SvgIconRasterizer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/ResourceUploader.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstring>

void EditorIcons::Initialize(ID3D12Device* device, DescriptorHeap& heap, const std::filesystem::path& directory)
{
    // Reinitialization is permitted only after the owner has drained the previous device.
    Shutdown();
    constexpr std::array names{"box.svg", "camera.svg", "file.svg", "folder.svg", "lightbulb.svg",
        "spotlight.svg", "sun.svg", "sun-moon.svg"};
    std::vector<unsigned char> pixels(Cell * Count * Cell * 4, 0);
    bool any = false;

    for(unsigned int i = 0; i < Count; ++i)
    {
        ImageData image;
        std::string error;

        if(!RasterizeEditorSvg(directory / names[i], RasterSize, image, error))
        {
            Log::Warning("Editor icon " + std::string(names[i]) + ": " + error);
            continue;
        }

        any = true;
        m_entries[i] = {image.width, image.height};

        for(unsigned int row = 0; row < image.height; ++row)
        {
            std::memcpy(pixels.data() + ((row + 2) * Cell * Count + i * Cell + 2) * 4,
                image.pixels.data() + row * image.width * 4, image.width * 4);
        }
    }

    if(!any)
    {
        return;
    }

    ResourceUploader uploader;

    if(!heap.CanAllocate(1) || !uploader.Initialize(device) ||
        !m_atlas.Initialize(device, Cell * Count, Cell, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !m_atlas.CreateSRV(device, heap) ||
        !uploader.UploadTexture(m_atlas.GetResource(), pixels.data(), pixels.size()))
    {
        Log::Warning("Editor icon atlas upload failed; using drawn fallback symbols.");
        Shutdown();
        return;
    }

    m_heap = &heap;
}

void EditorIcons::Prepare(ID3D12GraphicsCommandList* commands)
{
    if(m_atlas.GetResource())
    {
        m_atlas.Transition(commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void EditorIcons::Shutdown()
{
    m_atlas.Shutdown();
    m_entries = {};
    m_heap = nullptr;
}

ImVec2 EditorIcons::GetSize(EditorIcon icon, float maximumDimension) const
{
    const auto index = static_cast<unsigned int>(icon);

    if(index >= Count || !m_entries[index].width || !m_entries[index].height)
    {
        return ImVec2(maximumDimension, maximumDimension);
    }

    const auto& entry = m_entries[index];
    const float scale = maximumDimension / (std::max)(entry.width, entry.height);
    return ImVec2(entry.width * scale, entry.height * scale);
}

void EditorIcons::Draw(ImDrawList& draw, EditorIcon icon, ImVec2 origin, ImVec2 bounds, ImU32 tint) const
{
    const auto index = static_cast<unsigned int>(icon);

    if(index >= Count || bounds.x <= 0 || bounds.y <= 0)
    {
        return;
    }

    const auto& entry = m_entries[index];

    if(!m_heap || !entry.width || !entry.height)
    {
        // Stable and allocation-free, including when the entire icon directory is unavailable.
        const float size = (std::min)(bounds.x, bounds.y) * 0.7f;
        const ImVec2 start(origin.x + (bounds.x - size) * 0.5f, origin.y + (bounds.y - size) * 0.5f);
        draw.AddRect(start, ImVec2(start.x + size, start.y + size), tint, size * 0.1f);
        return;
    }

    const float scale = (std::min)(bounds.x / entry.width, bounds.y / entry.height);
    const ImVec2 size(entry.width * scale, entry.height * scale);
    const ImVec2 start(origin.x + (bounds.x - size.x) * 0.5f, origin.y + (bounds.y - size.y) * 0.5f);
    const ImVec2 uv0(float(index * Cell + 2) / (Cell * Count), 2.0f / Cell);
    const ImVec2 uv1(float(index * Cell + 2 + entry.width) / (Cell * Count), float(2 + entry.height) / Cell);
    draw.AddImage(static_cast<ImTextureID>(m_heap->GetGPUHandle(m_atlas.GetDescriptorIndex()).ptr),
        start, ImVec2(start.x + size.x, start.y + size.y), uv0, uv1, tint);
}
