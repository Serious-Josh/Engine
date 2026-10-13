#pragma once
#include "Renderer/Texture.h"
#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <wrl/client.h>

class ResourceUploader;
class DescriptorHeap;

enum class ProjectTextureInterpretation
{
    SRGBColor,
    LinearData,
    NormalMap
};

// Owned CPU image. Offsets refer only to pixels, never to decoder storage.
struct PreparedProjectTexture
{
    struct Subresource
    {
        size_t offset = 0;
        size_t rowPitch = 0;
        size_t slicePitch = 0;
    };
    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;

    ProjectTextureInterpretation interpretation = ProjectTextureInterpretation::LinearData;

    std::vector<unsigned char> pixels;

    std::vector<Subresource> subresources; // One entry per mip; single 2D image.
};

// Path must already be resolved/containment-validated by the project resolver.
// Decode/mip preparation only: no device, descriptors, submission or fence waits.
// Failure preserves the previous output.
bool PrepareProjectTexture(const std::filesystem::path& path, ProjectTextureInterpretation interpretation,
    PreparedProjectTexture& prepared, std::string* errorMessage = nullptr);

// Validates prepared layout before GPU allocation. Uses the existing checked synchronous uploader.
// Destination must be empty; failure never replaces a live texture or its retained descriptors.
bool UploadPreparedProjectTexture(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap,
    const PreparedProjectTexture& prepared, Texture& texture, std::string* errorMessage = nullptr);

// Load an already resolver-validated path. Success proves upload completion;
// caller owns the texture, its transitions, and checked retirement.
bool LoadProjectTexture(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& heap,
    const std::filesystem::path& path,
    ProjectTextureInterpretation interpretation,
    Texture& texture,
    std::string* errorMessage);

// Unpublished, Renderer-owned upload. No independent command queue or submission.
class PreparedTextureUpload
{
public:
    enum class State { Queued, Recorded, Submitted, Ready, Rejected, Cancelled, Uncertain };
    enum class RecordResult { Recorded, Deferred, Rejected };
    explicit PreparedTextureUpload(const PreparedProjectTexture& prepared);
    ~PreparedTextureUpload();
    PreparedTextureUpload(const PreparedTextureUpload&) = delete;
    PreparedTextureUpload& operator=(const PreparedTextureUpload&) = delete;
    RecordResult Record(ID3D12Device* device, DescriptorHeap& heap, ID3D12GraphicsCommandList* commands,
        std::uint64_t generation, unsigned int slot, size_t availableStaging, std::string& error);
    void ConfirmSubmission(std::uint64_t generation, std::uint64_t fence);
    void Discard(std::uint64_t generation);
    void MarkUncertain(std::uint64_t generation) noexcept;
    void Poll(std::uint64_t completedFence);
    void Cancel() noexcept;
    void ClearAfterGPUDrain(); // Requires checked completion AND command-list discard.
    bool Take(Texture& destination); // Ready only: never exposes an incomplete catalog texture.
    State GetState() const noexcept
    {
        return m_state;
    }

    size_t CPUBytes() const noexcept
    {
        return m_prepared.pixels.size();
    }

    size_t StagingBytes() const noexcept
    {
        return m_stagingBytes;
    }

    bool CanRetire() const noexcept;
private:
    PreparedProjectTexture m_prepared;

    Texture m_texture;

    Texture::Dependency m_dependency;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_staging;

    size_t m_stagingBytes = 0;

    State m_state = State::Queued;

    bool m_cancelled = false;

    std::uint64_t m_generation = 0;

    unsigned int m_slot = UINT_MAX;
    std::uint64_t m_fence = 0;
};

bool ValidatePreparedProjectTexture(const PreparedProjectTexture& prepared);
