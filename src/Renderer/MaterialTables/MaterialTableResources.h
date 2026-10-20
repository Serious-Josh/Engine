#pragma once

#include "Renderer/MaterialTables/MaterialTableFile.h"
#include "Renderer/Texture.h"

#include <span>
#include <string>

class DescriptorHeap;
class ResourceUploader;

// RTXPT.1A.2 GPU copy of the bundled GGX single-scatter complement tables, owned by the Renderer.
//
// Publication is all-or-nothing. Initialize builds both RG32F textures as a local candidate, uploads them with
// the renderer's ResourceUploader (which returns true only after confirmed GPU completion, and aborts the
// process rather than returning while a copy may still be in flight), creates both SRVs, and only then moves
// the candidate into the published state. Any failure leaves nothing published; the candidate's resources
// were never referenced by submitted work, or their copy is confirmed complete, so releasing them is safe.
//
// Published textures stay in COPY_DEST until the first consumer transitions them on the renderer's command
// list (the EnvironmentMap convention). They are immutable for the renderer lifetime and released by
// Shutdown, which the Renderer calls only after its final GPU drain.
class MaterialTableResources
{
    public:
    // Validates (SHA-256 pin, format, shader layout) and publishes. Returns false, with a reason, when the
    // tables are unavailable; the Renderer keeps rendering Legacy in that case.
    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        std::span<const std::byte> fileBytes,
        std::string& failureReason);

    void Shutdown();

    bool IsAvailable() const
    {
        return m_available;
    }

    Texture& GetMissingEnergyTexture()
    {
        return m_missingEnergy;
    }

    Texture& GetAverageTexture()
    {
        return m_average;
    }

    MaterialTables::CosineMapping GetCosineMapping() const
    {
        return static_cast<MaterialTables::CosineMapping>(m_header.cosineMapping);
    }

    const MaterialTables::MaterialTableFileHeader& GetHeader() const
    {
        return m_header;
    }

    private:
    Texture m_missingEnergy;
    Texture m_average;
    MaterialTables::MaterialTableFileHeader m_header{};
    bool m_available = false;
};
