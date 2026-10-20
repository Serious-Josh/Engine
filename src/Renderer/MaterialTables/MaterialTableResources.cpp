#include "Renderer/MaterialTables/MaterialTableResources.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/ResourceUploader.h"

namespace
{
    // Creates one RG32F texture with its SRV and uploads the texels. On false nothing was submitted that
    // still references the texture: the uploader fails before submission or aborts after it.
    bool CreateTableTexture(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const std::vector<float>& texels,
        std::uint32_t width,
        std::uint32_t height,
        const wchar_t* name,
        Texture& texture)
    {
        if(!texture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST))
        {
            return false;
        }

        texture.GetResource()->SetName(name);

        if(!uploader.UploadTexture(texture.GetResource(), texels.data(), texels.size() * sizeof(float)))
        {
            return false;
        }

        return texture.CreateSRV(device, descriptorHeap);
    }
}

bool MaterialTableResources::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    std::span<const std::byte> fileBytes,
    std::string& failureReason)
{
    Shutdown();

    // Stage 1: integrity and compatibility, entirely on the CPU.
    MaterialTables::MaterialTableSet tables;

    if(!MaterialTables::ValidateBundledMaterialTables(fileBytes, tables, &failureReason))
    {
        return false;
    }

    // Stage 2: candidate GPU resources, never visible to the renderer. Texture has no destructor, so every
    // failure path releases the candidate explicitly (safe: see CreateTableTexture).
    Texture missingEnergy;
    Texture average;
    const MaterialTables::MaterialTableFileHeader& header = tables.header;
    const auto discardCandidate = [&]()
    {
        missingEnergy.Shutdown();
        average.Shutdown();
    };

    if(!CreateTableTexture(device, uploader, descriptorHeap, tables.directional, header.directionalWidth,
        header.directionalHeight, L"Material Table Missing Energy And Bias", missingEnergy))
    {
        discardCandidate();
        failureReason = "The material table texture could not be created, uploaded or described.";
        return false;
    }

    if(!CreateTableTexture(device, uploader, descriptorHeap, tables.average, header.averageWidth, 1,
        L"Material Table Averages", average))
    {
        discardCandidate();
        failureReason = "The material table average texture could not be created, uploaded or described.";
        return false;
    }

    // Stage 3: publication. Both uploads are confirmed complete and both SRVs exist.
    m_missingEnergy = std::move(missingEnergy);
    m_average = std::move(average);
    m_header = header;
    m_available = true;
    return true;
}

void MaterialTableResources::Shutdown()
{
    m_missingEnergy.Shutdown();
    m_average.Shutdown();
    m_header = {};
    m_available = false;
}
