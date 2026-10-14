#include "Scene/SceneSwitchTiming.h"
#include "Renderer/ProjectTextureLoader.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>
#include <cstring>
#include <exception>

namespace
{
    bool Validate(const PreparedProjectTexture& image)
    {
        if(image.width == 0 || image.height == 0 || image.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
           image.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || image.subresources.empty() ||
           image.subresources.size() > Texture::CalculateMipLevels(image.width, image.height))
        {
            return false;
        }

        if(image.interpretation != ProjectTextureInterpretation::SRGBColor &&
           image.interpretation != ProjectTextureInterpretation::LinearData &&
           image.interpretation != ProjectTextureInterpretation::NormalMap)
        {
            return false;
        }

        const bool srgb = image.interpretation == ProjectTextureInterpretation::SRGBColor;
        size_t bytesPerUnit = 0;
        bool compressed = false;

        switch(image.resourceFormat)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            bytesPerUnit = 4;

            if(image.srvFormat != (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM))
            {
                return false;
            }

            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            bytesPerUnit = 16;

            if(image.interpretation != ProjectTextureInterpretation::LinearData ||
               image.srvFormat != image.resourceFormat || image.subresources.size() != 1)
            {
                return false;
            }

            break;
        case DXGI_FORMAT_BC1_UNORM:
            compressed = true;
            bytesPerUnit = 8;

            if(image.srvFormat != (srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM))
            {
                return false;
            }

            break;
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
            compressed = true;
            bytesPerUnit = image.resourceFormat == DXGI_FORMAT_BC4_UNORM ? 8 : 16;

            if(srgb || image.srvFormat != image.resourceFormat)
            {
                return false;
            }

            break;
        default:
            return false;
        }

        size_t width = image.width;
        size_t height = image.height;
        size_t previousEnd = 0;

        for(const auto& mip : image.subresources)
        {
            const size_t columns = compressed ? (width + 3) / 4 : width;
            const size_t rows = compressed ? (height + 3) / 4 : height;
            const size_t rowBytes = columns * bytesPerUnit; // Dimensions bounded above.

            if(mip.rowPitch < rowBytes || mip.rowPitch > (std::numeric_limits<size_t>::max)() / rows ||
               mip.slicePitch < mip.rowPitch * rows || mip.offset < previousEnd ||
               mip.offset > image.pixels.size() || mip.slicePitch > image.pixels.size() - mip.offset)
            {
                return false;
            }

            previousEnd = mip.offset + mip.slicePitch;
            width = (std::max)(size_t(1), width / 2);
            height = (std::max)(size_t(1), height / 2);
        }

        return true;
    }

    void Append(PreparedProjectTexture& image, const void* data, size_t rowPitch, size_t slicePitch)
    {
        if(!data || rowPitch == 0 || slicePitch == 0 || slicePitch > image.pixels.max_size() - image.pixels.size())
        {
            throw std::runtime_error("Invalid or oversized texture subresource.");
        }

        const auto* bytes = static_cast<const unsigned char*>(data);
        image.subresources.push_back({image.pixels.size(), rowPitch, slicePitch});
        image.pixels.insert(image.pixels.end(), bytes, bytes + slicePitch);
    }
}

bool PrepareProjectTexture(const std::filesystem::path& path, ProjectTextureInterpretation interpretation,
    PreparedProjectTexture& prepared, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    try
    {
        MipFilterMode filter;

        switch(interpretation)
        {
        case ProjectTextureInterpretation::SRGBColor: filter = MipFilterMode::SRGBColor; break;
        case ProjectTextureInterpretation::LinearData: filter = MipFilterMode::LinearData; break;
        case ProjectTextureInterpretation::NormalMap: filter = MipFilterMode::NormalMap; break;
        default: throw std::runtime_error("Unsupported project texture sampling interpretation.");
        }

        PreparedProjectTexture image;
        image.interpretation = interpretation;
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        const bool srgb = interpretation == ProjectTextureInterpretation::SRGBColor;

        if(extension == ".dds")
        {
            DirectX::ScratchImage decoded;

            if(!DDSLoader::Load(path, decoded))
            {
                throw std::runtime_error("Cannot decode DDS texture.");
            }

            const auto& metadata = decoded.GetMetadata();

            switch(metadata.format)
            {
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                image.resourceFormat = DXGI_FORMAT_BC1_UNORM;
                image.srvFormat = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
                break;
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC5_UNORM:

                if(srgb)
                {
                    throw std::runtime_error("BC4/BC5 textures cannot use sRGB sampling.");
                }

                image.resourceFormat = image.srvFormat = metadata.format;
                break;
            default:
                throw std::runtime_error("Unsupported DDS format; supported formats are BC1, BC4, and BC5 UNORM.");
            }

            if(metadata.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
               metadata.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
               metadata.mipLevels > Texture::CalculateMipLevels(static_cast<unsigned int>(metadata.width),
                   static_cast<unsigned int>(metadata.height)))
            {
                throw std::runtime_error("Invalid DDS dimensions or mip count.");
            }

            image.width = static_cast<unsigned int>(metadata.width);
            image.height = static_cast<unsigned int>(metadata.height);

            for(size_t mip = 0; mip < metadata.mipLevels; ++mip)
            {
                const auto* level = decoded.GetImage(mip, 0, 0);

                if(!level)
                {
                    throw std::runtime_error("Invalid DDS mip data.");
                }

                Append(image, level->pixels, level->rowPitch, level->slicePitch);
            }
        }
        else if(extension == ".hdr")
        {
            if(interpretation != ProjectTextureInterpretation::LinearData)
            {
                throw std::runtime_error("HDR textures require linear interpretation; sRGB and normal-map HDR loading are unsupported.");
            }

            HDRImageData decoded;

            if(!ImageLoader::LoadHDR(path, decoded) || decoded.width == 0 || decoded.height == 0 ||
               decoded.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || decoded.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
               decoded.pixels.size() != static_cast<size_t>(decoded.width) * decoded.height * 4)
            {
                throw std::runtime_error("Cannot decode HDR texture or invalid pixel layout.");
            }

            image.width = decoded.width;
            image.height = decoded.height;
            image.resourceFormat = image.srvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
            Append(image, decoded.pixels.data(), static_cast<size_t>(decoded.width) * 16, decoded.pixels.size() * sizeof(float));
        }
        else
        {
            const bool supported = extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".tga" || extension == ".bmp" || extension == ".psd" || extension == ".gif" ||
                extension == ".pic" || extension == ".pnm" || extension == ".ppm" || extension == ".pgm";

            if(!supported)
            {
                throw std::runtime_error("Unsupported project texture extension: " + extension);
            }

            ImageData decoded;
            ImageMipChain mips;

            if(!ImageLoader::Load(path, decoded) || decoded.width == 0 || decoded.height == 0 ||
               decoded.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || decoded.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
               !ImageLoader::GenerateMipChain(decoded, filter, mips))
            {
                throw std::runtime_error("Cannot decode texture or generate its mip chain.");
            }

            image.width = decoded.width;
            image.height = decoded.height;
            image.resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            image.srvFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

            for(const auto& level : mips.levels)
            {
                Append(image, level.pixels.data(), static_cast<size_t>(level.width) * 4, level.pixels.size());
            }
        }

        if(!Validate(image))
        {
            throw std::runtime_error("Invalid prepared texture dimensions, format, or subresource layout.");
        }

        prepared = std::move(image);
        return true;
    }
    catch(const std::exception& error)
    {
        if(errorMessage)
        {
            *errorMessage = "Texture preparation failed for " + path.string() + ": " + error.what();
        }

        return false;
    }
}

bool UploadPreparedProjectTexture(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap,
    const PreparedProjectTexture& prepared, Texture& texture, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!device || texture.GetResource() || !Validate(prepared))
    {
        if(errorMessage)
        {
            *errorMessage = "Texture upload requires an empty destination and valid prepared data.";
        }

        return false;
    }

    try
    {
        std::vector<TextureSubresourceData> subresources;
        subresources.reserve(prepared.subresources.size());

        for(const auto& mip : prepared.subresources)
        {
            subresources.push_back({prepared.pixels.data() + mip.offset, mip.rowPitch, mip.slicePitch});
        }

        if(!texture.InitializeMipmapped(device, prepared.width, prepared.height,
            static_cast<unsigned int>(subresources.size()), prepared.resourceFormat) ||
           !uploader.UploadTexture(texture.GetResource(), subresources.data(), static_cast<unsigned int>(subresources.size())) ||
           !texture.CreateSRV(device, heap, prepared.srvFormat))
        {
            texture.Shutdown();

            if(errorMessage)
            {
                *errorMessage = "Failed to create/upload prepared texture or allocate its SRV.";
            }

            return false;
        }

        return true;
    }
    catch(const std::exception& error)
    {
        // Post-submission uncertainty terminates inside ResourceUploader, never reaches cleanup.
        texture.Shutdown();

        if(errorMessage)
        {
            *errorMessage = std::string("Prepared texture upload failed: ") + error.what();
        }

        return false;
    }
}

bool LoadProjectTexture(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap,
    const std::filesystem::path& path, ProjectTextureInterpretation interpretation,
    Texture& texture, std::string* errorMessage)
{
    PreparedProjectTexture prepared;
    return SceneSwitchTiming::MeasureCall(SceneSwitchTiming::Stage::TexturePreparation, [&]()
    {
        return PrepareProjectTexture(path, interpretation, prepared, errorMessage);
    }) && SceneSwitchTiming::MeasureCall(SceneSwitchTiming::Stage::TextureUpload, [&]()
    {
        return UploadPreparedProjectTexture(device, uploader, heap, prepared, texture, errorMessage);
    });
}

bool ValidatePreparedProjectTexture(const PreparedProjectTexture& prepared)
{
    return Validate(prepared);
}

PreparedTextureUpload::PreparedTextureUpload(const PreparedProjectTexture& prepared) : m_prepared(prepared)
{
}

PreparedTextureUpload::~PreparedTextureUpload()
{
    if(m_state == State::Recorded || m_state == State::Submitted || m_state == State::Uncertain)
    {
        std::terminate(); // Potential GPU use cannot be resolved by destruction.
    }

    m_texture.Shutdown();
}

PreparedTextureUpload::RecordResult PreparedTextureUpload::Record(ID3D12Device* device, DescriptorHeap& heap,
    ID3D12GraphicsCommandList* commands, std::uint64_t generation, unsigned int slot,
    size_t availableStaging, std::string& error)
{
    error.clear();

    if(m_state != State::Queued || m_cancelled || !device || !commands || generation == 0 || !Validate(m_prepared))
    {
        error = "Invalid prepared texture upload or recording context.";

        if(m_state == State::Queued)
        {
            m_state = State::Rejected;
        }

        return RecordResult::Rejected;
    }

    try
    {
        const auto count = static_cast<unsigned int>(m_prepared.subresources.size());
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = m_prepared.width;
        description.Height = m_prepared.height;
        description.DepthOrArraySize = 1;

        description.MipLevels = static_cast<UINT16>(count);
        description.Format = m_prepared.resourceFormat;
        description.SampleDesc.Count = 1;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> rowBytes(count);
        UINT64 size = 0;
        device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(), rows.data(), rowBytes.data(), &size);

        if(size == 0 || size > (std::numeric_limits<size_t>::max)())
        {
            throw std::runtime_error("Invalid upload footprint size.");
        }

        if(size > 64ull * 1024 * 1024)
        {
            throw std::runtime_error("Prepared texture exceeds the 64 MiB staging limit.");
        }

        if(size > availableStaging)
        {
            error = "Deferred: prepared texture staging capacity unavailable.";
            return RecordResult::Deferred;
        }

        for(unsigned int index = 0; index < count; ++index)
        {
            const auto& mip = m_prepared.subresources[index];

            if(rows[index] == 0 || rowBytes[index] == 0 || rowBytes[index] > mip.rowPitch ||
               static_cast<UINT64>(rows[index] - 1) * mip.rowPitch + rowBytes[index] > mip.slicePitch ||
               footprints[index].Offset > size ||
               static_cast<UINT64>(rows[index] - 1) * footprints[index].Footprint.RowPitch + rowBytes[index] > size - footprints[index].Offset)
            {
                throw std::runtime_error("Prepared texture does not match D3D12 copy footprints.");
            }
        }

        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;

        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if(FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_staging.ReleaseAndGetAddressOf()))))
        {
            throw std::runtime_error("Cannot allocate texture upload staging.");
        }

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};

        if(FAILED(m_staging->Map(0, &readRange, &mapped)))
        {
            throw std::runtime_error("Cannot map texture upload staging.");
        }

        for(unsigned int index = 0; index < count; ++index)
        {
            const auto& mip = m_prepared.subresources[index];

            for(UINT row = 0; row < rows[index]; ++row)
            {
                std::memcpy(static_cast<unsigned char*>(mapped) + footprints[index].Offset +
                    static_cast<size_t>(row) * footprints[index].Footprint.RowPitch,
                    m_prepared.pixels.data() + mip.offset + static_cast<size_t>(row) * mip.rowPitch,
                    static_cast<size_t>(rowBytes[index]));
            }
        }

        m_staging->Unmap(0, nullptr);

        if(!m_texture.InitializeMipmapped(device, m_prepared.width, m_prepared.height, count, m_prepared.resourceFormat) ||
           !m_texture.CreateSRV(device, heap, m_prepared.srvFormat))
        {
            throw std::runtime_error("Cannot allocate unpublished texture or its SRV.");
        }

        m_dependency = m_texture.Retain(); // K.4A lease keeps resource and descriptors together.

        if(!m_dependency)
        {
            throw std::runtime_error("Cannot retain unpublished texture allocation.");
        }

        m_stagingBytes = static_cast<size_t>(size);
        m_generation = generation;
        m_slot = slot;
        m_state = State::Recorded; // All fallible preparation finished before the first command.

        for(unsigned int index = 0; index < count; ++index)
        {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = m_staging.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[index];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = m_texture.GetResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        m_texture.Transition(commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return RecordResult::Recorded;
    }
    catch(const std::exception& exception)
    {
        if(m_state == State::Recorded)
        {
            m_state = State::Uncertain;
            error = "Recording failed after commands may have been emitted; retaining upload allocations.";
            return RecordResult::Rejected;
        }

        m_texture.Shutdown();
        m_dependency.reset();
        m_staging.Reset();
        m_state = State::Rejected;
        error = exception.what();
        return RecordResult::Rejected;
    }
}

void PreparedTextureUpload::ConfirmSubmission(std::uint64_t generation, std::uint64_t fence)
{
    if(m_state == State::Recorded && m_generation == generation)
    {
        if(fence == 0 || fence == UINT64_MAX)
        {
            m_state = State::Uncertain;
            return;
        }

        m_fence = fence;
        m_state = State::Submitted;
        m_prepared = {}; // GPU now reads staging, not these original CPU bytes.
    }
}

void PreparedTextureUpload::Discard(std::uint64_t generation)
{
    if(m_state == State::Recorded && m_generation == generation)
    {
        m_texture.Shutdown();
        m_dependency.reset();
        m_staging.Reset();
        m_stagingBytes = 0;
        m_state = m_cancelled ? State::Cancelled : State::Queued;
    }
}

void PreparedTextureUpload::MarkUncertain(std::uint64_t generation) noexcept
{
    if(m_state == State::Recorded && m_generation == generation)
    {
        m_state = State::Uncertain;
    }
}

void PreparedTextureUpload::Poll(std::uint64_t completedFence)
{
    if(m_state == State::Submitted && completedFence != UINT64_MAX && completedFence >= m_fence)
    {
        m_staging.Reset();
        m_stagingBytes = 0;
        m_state = State::Ready;

        if(m_cancelled)
        {
            ClearAfterGPUDrain(); // This upload's exact submission completed; it was never exposed.
        }
    }
}

void PreparedTextureUpload::Cancel() noexcept
{
    m_cancelled = true;

    if(m_state == State::Queued || m_state == State::Ready || m_state == State::Rejected)
    {
        m_state = State::Cancelled;
    }
}

bool PreparedTextureUpload::CanRetire() const noexcept
{
    return m_cancelled && (m_state == State::Cancelled || m_state == State::Rejected);
}

void PreparedTextureUpload::ClearAfterGPUDrain()
{
    m_cancelled = true;
    m_texture.Shutdown();
    m_dependency.reset();
    m_staging.Reset();
    m_stagingBytes = 0;
    m_prepared = {};
    m_state = State::Cancelled;
}

bool PreparedTextureUpload::Take(Texture& destination)
{
    if(m_state != State::Ready || destination.GetResource())
    {
        return false;
    }

    destination = std::move(m_texture);
    m_dependency.reset();
    m_cancelled = true;
    m_state = State::Cancelled;
    return true;
}
