#include "Renderer/Preview/MaterialPreviewRenderer.h"

#include "Core/Log.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/TangentGenerator.h"
#include "Renderer/Vertex.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Math/Matrix4.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/MaterialAssetSerializer.h"
#include "Renderer/Material.h"

#include <exception>
#include <algorithm>
#include <utility>
#include <cstring>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>


namespace
{
    std::string ThumbnailFingerprint(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const MaterialPreviewRequest& request,
        MaterialThumbnailValidation& validation)
    {
        const auto id = AssetID::ParseReference(request.materialIdentity);
        const auto material = id ? ProjectAssetResolver::Resolve(paths, registry, *id) : AssetResolution{};

        if(material.status != AssetAvailability::Available)
        {
            throw std::runtime_error("Saved thumbnail material is no longer available.");
        }

        nlohmann::json key = {
            {"project", std::filesystem::canonical(paths.GetRoot()).generic_string()},
            {"material", request.materialIdentity}, {"path", material.resolvedPath.generic_string()},
            {"content", validation.Digest(material.resolvedPath)},
            // Bump whenever preview shaders, BRDFs, tone mapping, geometry or studio setup change.
            {"renderVersion", "v2-full-normal-unit-sphere32x64-camera2.6,1.7,-3.2-fov.65-yaw0-pitch0-studio3-POM-AgX"},
            {"extent", 256}, {"format", "R8G8B8A8_UNORM-display-encoded"}, {"textures", nlohmann::json::array()}
        };

        for(size_t slot = 0; slot < request.textures.size(); ++slot)
        {
            nlohmann::json texture{{"slot", slot}, {"reference", request.textures[slot]},
                {"interpretation", slot == 0 ? "srgb" : slot == 1 ? "normal" : "linear"}};
            const auto textureID = request.textures[slot] ? AssetID::ParseReference(*request.textures[slot]) : std::nullopt;
            const auto resolved = textureID ? ProjectAssetResolver::Resolve(paths, registry, *textureID) : AssetResolution{};
            texture["status"] = static_cast<int>(resolved.status);
            const auto* record = textureID ? registry.Find(*textureID) : nullptr;

            if(record)
            {
                texture["registeredPath"] = record->relativePath.generic_string();
                texture["type"] = static_cast<int>(record->type);
            }

            if(resolved.status == AssetAvailability::Available)
            {
                texture["path"] = resolved.resolvedPath.generic_string();
                texture["size"] = std::filesystem::file_size(resolved.resolvedPath);
                texture["mtime"] = std::filesystem::last_write_time(resolved.resolvedPath).time_since_epoch().count();
                texture["content"] = validation.Digest(resolved.resolvedPath);
            }

            key["textures"].push_back(std::move(texture));
        }

        return key.dump();
    }
}

bool MaterialPreviewRenderer::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaders,
    ResourceUploader& uploader,
    DescriptorHeap& rtvHeap,
    DescriptorHeap& dsvHeap,
    DescriptorHeap& resourceHeap)
{
    if(!device || m_initialized)
    {
        return false;
    }

    // These resources have not been recorded into any renderer command list.
    // Mesh uploads complete synchronously, or the uploader fails closed before cleanup.
    try
    {
        if(!InitializePipelines(device, shaders))
        {
            Shutdown();
            return false;
        }

        SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);
        TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);
        const VertexLayout layout = CreateVertexLayout();

        if(!m_sphere.Initialize(
                device,
                uploader,
                resourceHeap,
                sphere.vertices.data(),
                sphere.vertices.size(),
                layout,
                sphere.indices.data(),
                sphere.indices.size()))
        {
            Shutdown();
            return false;
        }

        // Preview attachments use zero background/alpha, unlike the scene G-buffer.
        // Match the optimized creation values to Record's zero clears without
        // changing the production G-buffer defaults.
        RenderTarget* targets[] = {&m_gBuffer.GetMaterial(), &m_gBuffer.GetSurface(),
                                  &m_gBuffer.GetMotion(), &m_gBuffer.GetGeometricNormal()};
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};

        for(unsigned int index = 0; index < 4; ++index)
        {
            if(!targets[index]->Initialize(
                    device, kExtent, kExtent, formats[index], rtvHeap, resourceHeap,
                    {0.0f, 0.0f, 0.0f, 0.0f}))
            {
                Shutdown();
                return false;
            }
        }

        if(!m_gBuffer.GetDepth().Initialize(device, kExtent, kExtent, dsvHeap, resourceHeap))
        {
            Shutdown();
            return false;
        }

        if(!m_hdrColor.Initialize(
                device, kExtent, kExtent, DXGI_FORMAT_R16G16B16A16_FLOAT, rtvHeap, resourceHeap))
        {
            Shutdown();
            return false;
        }

        if(!m_inspector.displayColor.Initialize(
                device, kExtent, kExtent, DXGI_FORMAT_R8G8B8A8_UNORM, rtvHeap, resourceHeap))
        {
            Shutdown();
            return false;
        }

        // Named capture targets allow verification in PIX without adding preview UI.
        m_hdrColor.GetResource()->SetName(L"Material Preview HDR");
        m_inspector.displayColor.GetResource()->SetName(L"Material Preview Display");

        if(!m_thumbnail.displayColor.Initialize(
                device, kExtent, kExtent, DXGI_FORMAT_R8G8B8A8_UNORM, rtvHeap, resourceHeap))
        {
            Shutdown();
            return false;
        }

        m_thumbnail.displayColor.GetResource()->SetName(L"Saved Material Thumbnail Display");
        const auto description = m_thumbnail.displayColor.GetResource()->GetDesc();
        device->GetCopyableFootprints(&description, 0, 1, 0, &m_thumbnailFootprint, nullptr, nullptr, &m_thumbnailBufferSize);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = m_thumbnailBufferSize;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_thumbnailReadback))))
        {
            Shutdown();
            return false;
        }

        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_thumbnailUpload))))
        {
            Shutdown();
            return false;
        }
    }
    catch(const std::exception&)
    {
        Log::Error("Material preview resource initialization threw an exception.");
        Shutdown();
        return false;
    }

    m_initialized = true;
    m_device = device;
    m_uploader = &uploader;
    m_heap = &resourceHeap;
    m_thumbnailRTVHeap = &rtvHeap;
    return true;
}

void MaterialPreviewRenderer::Shutdown()
{
    // Caller has drained GPU work and conclusively discarded remaining commands.
    m_recording.reset();
    m_quarantined = false;
    m_meshPreparation.Shutdown();

    if(m_meshWriter.valid())
    {
        try
        {
            m_meshWriter.get(); // Shutdown only; worker owns CPU data and no Renderer borrows.
        }
        catch(...)
        {
            // Optional disk-cache failures do not affect teardown.
        }
    }

    for(auto& job : m_meshJobs)
    {
        job.color.Shutdown();
    }

    m_meshJobs.clear();
    m_meshTickets.clear();
    m_meshConsumers.clear();
    m_meshCompletedFence = 0;
    m_meshTurn = false;

    for(auto& [key, entry] : m_textureCache)
    {
        entry.texture.Shutdown();
    }

    m_textureCache.clear();
    m_thumbnailValidation.Clear();

    for(auto& image : m_thumbnailImages)
    {
        image.color.Shutdown();
    }

    m_thumbnailImages.clear();
    m_thumbnailWorkingSet.clear();
    m_thumbnailWorkingSetActive = false;
    m_thumbnailRequests.clear();
    m_thumbnailRTVHeap = nullptr;
    m_thumbnailProject.clear();
    m_lastThumbnailIdentity.clear();
    m_thumbnailUseSerial = 0;
    m_thumbnail.destination = nullptr;
    m_completedTextureFence = 0;
    m_textureUseSerial = 0;
    m_inspector.dependencies.textures.fill(nullptr);
    m_inspector.dependencies.request.reset();
    m_inspector.dependencies.registrySnapshot.clear();
    m_inspector.dependencies.ready = false;
    m_device = nullptr;
    m_uploader = nullptr;
    m_heap = nullptr;

    m_inspector.request.reset();
    m_inspector.outputValid = false;
    m_inspector.recorded = false;

    m_thumbnail.request.reset();
    m_thumbnail.dependencies.textures.fill(nullptr);
    m_thumbnail.dependencies.request.reset();
    m_thumbnail.dependencies.registrySnapshot.clear();
    m_thumbnail.dependencies.ready = false;
    m_thumbnail.outputValid = false;
    m_thumbnail.recorded = false;
    m_thumbnail.generationFence = 0;
    m_thumbnail.revision = 0;
    m_thumbnail.capturePending = false;
    m_thumbnail.diskHit = false;
    m_thumbnail.cacheEligible = false;
    m_thumbnail.fingerprint.clear();
    m_thumbnail.registry.Clear();
    m_thumbnail.diskSource = {};

    if(m_thumbnailReadback)
    {
        m_thumbnailReadback->Release();
        m_thumbnailReadback = nullptr;
    }

    if(m_thumbnailUpload)
    {
        m_thumbnailUpload->Release();
        m_thumbnailUpload = nullptr;
    }

    m_toneMap.Shutdown();
    m_lightingPipeline.Shutdown();
    m_geometryPipeline.Shutdown();

    m_inspector.displayColor.Shutdown();
    m_thumbnail.displayColor.Shutdown();
    m_hdrColor.Shutdown();
    m_gBuffer.Shutdown();
    m_sphere.Shutdown();

    m_initialized = false;
}

bool MaterialPreviewRenderer::InitializePipelines(
    ID3D12Device* device,
    ShaderLibrary& shaders)
{
    Shader* geometryVS = shaders.Load({"MaterialPreview.hlsl", "SphereVS", ShaderStage::Vertex});
    Shader* geometryPS = shaders.Load({"MaterialPreview.hlsl", "SpherePS", ShaderStage::Pixel});
    Shader* lightingVS = shaders.Load({"MaterialPreview.hlsl", "LightingVS", ShaderStage::Vertex});
    Shader* lightingPS = shaders.Load({"MaterialPreview.hlsl", "LightingPS", ShaderStage::Pixel});

    if(!geometryVS || !geometryPS || !lightingVS || !lightingPS)
    {
        return false;
    }

    const VertexLayout layout = CreateVertexLayout();
    PipelineDescription geometry{};
    geometry.vertexShader = geometryVS;
    geometry.pixelShader = geometryPS;
    geometry.vertexLayout = &layout;
    geometry.renderTargets.count = 4;
    geometry.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geometry.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometry.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometry.renderTargets.formats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometry.depthEnabled = true;
    geometry.depthFormat = DXGI_FORMAT_D32_FLOAT;
    // 36 geometry DWORDs + 27 material DWORDs = 63, within the 64-DWORD root budget.
    geometry.rootParameters.push_back({RootParameterType::Constants, 0, 36, D3D12_SHADER_VISIBILITY_ALL});
    geometry.rootParameters.push_back({RootParameterType::Constants, 2, 27, D3D12_SHADER_VISIBILITY_PIXEL});

    if(!m_geometryPipeline.Initialize(device, geometry))
    {
        return false;
    }

    PipelineDescription lighting{};
    lighting.vertexShader = lightingVS;
    lighting.pixelShader = lightingPS;
    lighting.renderTargets.count = 1;
    lighting.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lighting.rootParameters.push_back({RootParameterType::Constants, 1, 9, D3D12_SHADER_VISIBILITY_PIXEL});
    lighting.rootParameters.push_back({RootParameterType::Constants, 2, 27, D3D12_SHADER_VISIBILITY_PIXEL});

    return m_lightingPipeline.Initialize(device, lighting) && m_toneMap.Initialize(device, shaders);
}

bool MaterialPreviewRenderer::Record(const RecordingContext& context)
{
    if(!m_initialized || !context.commandList || m_quarantined || !m_recording ||
        m_recording->generation != context.generation)
    {
        return false;
    }

    if(m_inspector.request && m_inspector.dependencies.ready && !m_inspector.outputValid)
    {
        if(!RecordJob(context, m_inspector.request->material,
                m_inspector.request->yaw, m_inspector.request->pitch,
                m_inspector.dependencies.textures, m_inspector.displayColor))
        {
            return false;
        }

        m_inspector.recorded = true;
        return true;
    }

    const bool materialWork = m_thumbnail.request &&
        (m_thumbnail.diskHit || m_thumbnail.dependencies.ready) && !m_thumbnail.outputValid;

    if(HasMeshWork() && (m_meshTurn || !materialWork))
    {
        m_meshTurn = false;
        return RecordMeshJob(context);
    }

    if(materialWork)
    {
        m_meshTurn = true;
        // Protect the destination even if recording fails before submission confirmation.
        if(!m_thumbnail.destination)
        {
            return false;
        }

        m_thumbnail.destination->recorded = true;
        if(m_thumbnail.diskHit)
        {
            m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = m_thumbnailUpload;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = m_thumbnailFootprint;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = m_thumbnail.displayColor.GetResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            context.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else if(!RecordJob(context, m_thumbnail.request->material, 0.0f, 0.0f,
                m_thumbnail.dependencies.textures, m_thumbnail.displayColor))
        {
            return false;
        }

        if(!m_thumbnail.diskHit && m_thumbnail.cacheEligible)
        {
            m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = m_thumbnail.displayColor.GetResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = m_thumbnailReadback;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = m_thumbnailFootprint;
            context.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_thumbnail.capturePending = true;
        }

        auto& destinationImage = *m_thumbnail.destination;
        m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        destinationImage.color.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        // Both resources are fixed 256x256 RGBA8 with identical sample/mip layouts.
        context.commandList->CopyResource(destinationImage.color.GetResource(), m_thumbnail.displayColor.GetResource());
        destinationImage.color.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_thumbnail.displayColor.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_thumbnail.recorded = true;
    }

    return true;
}

bool MaterialPreviewRenderer::RecordJob(
    const RecordingContext& context,
    GPUMaterial material,
    float yaw,
    float pitch,
    const std::array<Texture*, 7>& textures,
    RenderTarget& displayColor)
{
    // Preserve the material sphere's exact camera and rotation-only model.
    PreviewFraming framing;
    framing.eye = {2.6f, 1.7f, -3.2f};
    framing.viewProjection = Matrix4::Perspective(0.65f, 1.0f, 0.1f, 20.0f) *
        Matrix4::LookAt(framing.eye, Vector3{}, Vector3{0.0f, 1.0f, 0.0f});
    const Matrix4 model = Matrix4::RotationY(yaw) * Matrix4::RotationX(pitch);
    PreviewGeometry geometry;
    std::string error;

    if(!DescribeGeometry(m_sphere, model, geometry, error))
    {
        Log::Error("Material preview geometry rejected: " + error);
        return false;
    }

    return RecordGeometryJob(context, material, textures, displayColor, geometry, framing);
}

bool MaterialPreviewRenderer::RecordGeometryJob(const RecordingContext& context, GPUMaterial material,
    const std::array<Texture*, 7>& textures, RenderTarget& displayColor,
    const PreviewGeometry& geometry, const PreviewFraming& framing)
{
    ID3D12GraphicsCommandList* commandList = context.commandList;
    unsigned int* slots[] = {&material.textureIndex, &material.normalTextureIndex,
        &material.roughnessTextureIndex, &material.metallicTextureIndex, &material.armTextureIndex,
        &material.aoTextureIndex, &material.heightTextureIndex};

    for(size_t index = 0; index < textures.size(); ++index)
    {
        *slots[index] = UINT_MAX;

        if(textures[index])
        {
            // Pin before recording any command referencing this resource. Only a successful
            // queue submission can convert this protection to a retirement fence.
            for(auto& [key, entry] : m_textureCache)
            {
                if(&entry.texture == textures[index])
                {
                    entry.awaitingSubmission = true;
                }
            }

            textures[index]->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            *slots[index] = textures[index]->GetDescriptorIndex();
        }
    }

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(kExtent), float(kExtent), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, kExtent, kExtent};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    RenderTarget* targets[] = {&m_gBuffer.GetMaterial(), &m_gBuffer.GetSurface(),
                              &m_gBuffer.GetMotion(), &m_gBuffer.GetGeometricNormal()};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
    constexpr float clear[4]{};

    for(unsigned int index = 0; index < 4; ++index)
    {
        targets[index]->Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[index] = targets[index]->GetRTV();
        commandList->ClearRenderTargetView(rtvs[index], clear, 0, nullptr);
    }

    m_gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto dsv = m_gBuffer.GetDepth().GetDSV();
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(4, rtvs, FALSE, &dsv);
    m_geometryPipeline.Bind(commandList);
    const Vector3& eye = framing.eye;
    commandList->SetGraphicsRoot32BitConstants(0, 16, &framing.viewProjection, 0);
    commandList->SetGraphicsRoot32BitConstants(0, 16, &geometry.model, 16);
    const float cameraPosition[4] = {eye.x, eye.y, eye.z, 1.0f};
    commandList->SetGraphicsRoot32BitConstants(0, 4, cameraPosition, 32);
    static_assert(sizeof(GPUMaterial) == 27 * sizeof(unsigned int));
    commandList->SetGraphicsRoot32BitConstants(1, 27, &material, 0);
    commandList->IASetVertexBuffers(0, 1, &geometry.vertices);
    commandList->IASetIndexBuffer(&geometry.indices);

    for(const auto& range : geometry.ranges)
    {
        commandList->DrawIndexedInstanced(range.indexCount, 1, range.firstIndex, 0, 0);
    }

    for(RenderTarget* target : targets)
    {
        target->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_hdrColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto hdrRTV = m_hdrColor.GetRTV();
    commandList->OMSetRenderTargets(1, &hdrRTV, FALSE, nullptr);
    m_lightingPipeline.Bind(commandList);
    struct LightingConstants
    {
        unsigned int materialIndex;
        unsigned int normalIndex;
        unsigned int positionIndex;

        unsigned int brdfIndex;
        unsigned int averageIndex;

        float camera[3];
        unsigned int geometricIndex;
    };
    static_assert(sizeof(LightingConstants) == 9 * sizeof(unsigned int));
    const LightingConstants lighting{
        m_gBuffer.GetMaterial().GetSRVIndex(), m_gBuffer.GetSurface().GetSRVIndex(),
        m_gBuffer.GetMotion().GetSRVIndex(), context.brdfLUTIndex, context.averageAlbedoLUTIndex,
        {eye.x, eye.y, eye.z}, m_gBuffer.GetGeometricNormal().GetSRVIndex()};
    commandList->SetGraphicsRoot32BitConstants(0, 9, &lighting, 0);
    commandList->SetGraphicsRoot32BitConstants(1, 27, &material, 0);
    commandList->DrawInstanced(3, 1, 0, 0);

    displayColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_toneMap.Execute(commandList, m_hdrColor, displayColor.GetRTV());
    displayColor.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void MaterialPreviewRenderer::SetRequest(const std::optional<MaterialPreviewRequest>& request)
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    if(!request)
    {
        m_inspector.request.reset();
        m_inspector.outputValid = false;
        m_inspector.recorded = false;
        return;
    }

    if(m_inspector.request && m_inspector.request->projectRoot == request->projectRoot &&
       m_inspector.request->materialIdentity == request->materialIdentity && m_inspector.request->revision == request->revision)
    {
        return;
    }

    m_inspector.request = request;
    m_inspector.outputValid = false;
    m_inspector.recorded = false;
}

bool MaterialPreviewRenderer::ConfirmSubmission(std::uint64_t generation, std::uint64_t submissionFence)
{
    if(!m_initialized && !m_recording)
    {
        return true;
    }

    if(!m_recording || m_recording->generation != generation || m_quarantined ||
        submissionFence == 0 || submissionFence == UINT64_MAX)
    {
        return false;
    }

    for(auto& [key, entry] : m_textureCache)
    {
        if(entry.awaitingSubmission)
        {
            entry.submissionFence = submissionFence;
            entry.awaitingSubmission = false;
        }
    }

    // Submission order on the Renderer queue establishes output validity for later use.
    // This is not a GPU-completion claim and never authorizes resource destruction.
    if(m_inspector.recorded)
    {
        m_inspector.outputValid = true;
        m_inspector.recorded = false;
    }

    if(m_thumbnail.recorded)
    {
        m_thumbnail.generationFence = submissionFence;
        m_thumbnail.outputValid = true;
        m_thumbnail.recorded = false;
    }

    for(auto& image : m_thumbnailImages)
    {
        if(image.recorded)
        {
            image.generationFence = submissionFence;
            image.ready = true;
            image.recorded = false;
        }

        if(image.awaitingDisplaySubmission)
        {
            image.displayFence = submissionFence;
            image.awaitingDisplaySubmission = false;
        }
    }

    for(auto& job : m_meshJobs)
    {
        if(job.recorded)
        {
            job.fence = submissionFence;
            job.copiedBytes += job.recordedBytes;
            job.recordedBytes = 0;

            job.recorded = false;
            job.rendered = job.renderRecorded;
            job.renderRecorded = false;
        }

        if(job.awaitingDisplaySubmission)
        {
            job.displayFence = submissionFence;
            job.awaitingDisplaySubmission = false;
        }
    }

    m_recording.reset();
    return true;
}

bool MaterialPreviewRenderer::PrepareSavedThumbnail(
    const ThumbnailRequest& queued,
    std::uint64_t completedFence,
    std::string& error)
{
    const ProjectPaths paths(queued.projectRoot);
    const auto& registry = queued.registry;
    const auto& id = queued.id;
    error.clear();

    try
    {
        if(!m_initialized || completedFence == UINT64_MAX)
        {
            throw std::runtime_error("Thumbnail renderer unavailable or device removed.");
        }

        const auto root = std::filesystem::canonical(paths.GetRoot());

        const auto* record = registry.Find(id);
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, id);

        if(!record || record->type != AssetType::Material || resolved.status != AssetAvailability::Available ||
           _wcsicmp(resolved.resolvedPath.extension().c_str(), L".material") != 0)
        {
            throw std::runtime_error("Thumbnail requires an available registered .material asset: " + id.ToReference());
        }

        const auto snapshot = m_thumbnailValidation.LoadMaterial(resolved.resolvedPath);
        const auto& saved = snapshot.material;

        Material material;
        material.model = saved.model;
        material.roughness = saved.roughness;
        material.metallic = saved.metallic;
        material.ior = saved.ior;
        material.surfaceDetailMode = saved.surfaceDetailMode;
        material.heightScale = saved.heightScale;
        material.heightBias = saved.heightBias;
        material.preferARM = saved.preferARM;

        for(size_t index = 0; index < 3; ++index)
        {
            material.baseColor[index] = saved.baseColor[index];
            material.conductorN[index] = saved.conductorN[index];
            material.conductorK[index] = saved.conductorK[index];
        }

        MaterialPreviewRequest request{root, id.ToReference(), m_thumbnail.revision + 1, material.GetGPUData(), 0.0f, 0.0f};
        request.textures = {saved.baseColorTexture, saved.normalTexture, saved.roughnessTexture,
            saved.metallicTexture, saved.armTexture, saved.aoTexture, saved.heightTexture};
        const auto fingerprint = ThumbnailFingerprint(paths, registry, request, m_thumbnailValidation);

        if(m_thumbnailValidation.Digest(resolved.resolvedPath) != snapshot.digest)
        {
            throw std::runtime_error("Saved material changed during thumbnail preparation; retry.");
        }

        ThumbnailImage* existing = nullptr;

        for(auto& image : m_thumbnailImages)
        {
            if(image.projectRoot == root && image.materialIdentity == id.ToReference() &&
               (!existing || image.revision > existing->revision))
            {
                existing = &image;
            }
        }

        if(existing && existing->fingerprint == fingerprint && existing->ready && !existing->recorded)
        {
            existing->stale = false;
            existing->lastUse = ++m_thumbnailUseSerial;
            existing->nextValidation = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return true;
        }

        if(existing)
        {
            // Never label an old image with a newly validated fingerprint.
            existing->stale = true;
        }

        auto* destination = AcquireThumbnailImage(completedFence, root, id.ToReference());

        if(!destination)
        {
            error = "Deferred: all material thumbnail display entries are protected.";
            return false;
        }

        auto diskSource = MaterialThumbnailDiskCache::Describe(paths, *record, resolved.resolvedPath, fingerprint);
        MaterialThumbnailDiskCache::Image pixels;
        const bool diskHit = MaterialThumbnailDiskCache::Load(diskSource, pixels) &&
            pixels.width == kExtent && pixels.height == kExtent;
        TextureDependencies dependencies;
        bool cacheEligible = true;

        if(diskHit)
        {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, 0};

            if(FAILED(m_thumbnailUpload->Map(0, &readRange, &mapped)))
            {
                throw std::runtime_error("Cannot map material thumbnail upload buffer.");
            }

            for(size_t row = 0; row < kExtent; ++row)
            {
                memcpy(static_cast<unsigned char*>(mapped) + m_thumbnailFootprint.Offset + row * m_thumbnailFootprint.Footprint.RowPitch,
                    pixels.pixels.data() + row * kExtent * 4, kExtent * 4);
            }

            const D3D12_RANGE writtenRange{static_cast<SIZE_T>(m_thumbnailFootprint.Offset), static_cast<SIZE_T>(m_thumbnailBufferSize)};
            m_thumbnailUpload->Unmap(0, &writtenRange);
        }
        else
        {
            const auto preparation = PrepareJobTextures(request, registry, dependencies, true);

            if(preparation == TexturePreparation::Deferred)
            {
                error = "Deferred: no safely retireable texture dependency entry.";
                return false;
            }

            if(preparation == TexturePreparation::Failed)
            {
                throw std::runtime_error("Thumbnail texture preparation failed; no fallback thumbnail was published.");
            }

            for(size_t slot = 0; slot < request.textures.size(); ++slot)
            {
                if(request.textures[slot] && !dependencies.textures[slot])
                {
                    const auto textureID = AssetID::ParseReference(*request.textures[slot]);
                    const auto status = textureID ? ProjectAssetResolver::Resolve(paths, registry, *textureID).status : AssetAvailability::InvalidPath;
                    cacheEligible &= status == AssetAvailability::Missing || status == AssetAvailability::NotRegistered;
                }
            }
        }

        if(ThumbnailFingerprint(paths, registry, request, m_thumbnailValidation) != fingerprint)
        {
            throw std::runtime_error("Thumbnail dependencies changed during preparation; retry.");
        }

        auto registrySnapshot = registry;
        destination->projectRoot = root;
        destination->materialIdentity = id.ToReference();
        destination->fingerprint = fingerprint;
        destination->revision = m_thumbnail.revision + 1;
        destination->lastUse = ++m_thumbnailUseSerial;
        destination->ready = false;
        destination->stale = false;
        destination->nextValidation = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        m_thumbnail.diskSource = std::move(diskSource);
        m_thumbnail.registry = std::move(registrySnapshot);
        m_thumbnail.fingerprint = fingerprint;
        m_thumbnail.dependencies = std::move(dependencies);
        m_thumbnail.diskHit = diskHit;
        m_thumbnail.cacheEligible = cacheEligible;
        m_thumbnail.capturePending = false;
        ++m_thumbnail.revision;
        m_thumbnail.outputValid = false;
        m_thumbnail.generationFence = 0;
        m_thumbnail.destination = destination;
        m_thumbnail.request = std::move(request);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void MaterialPreviewRenderer::PollThumbnailReadback(std::uint64_t completedFence)
{
    // UINT64_MAX indicates device removal, not proof of completion.
    if(completedFence != UINT64_MAX)
    {
        m_completedTextureFence = completedFence;
    }

    if(!m_thumbnail.capturePending || !m_thumbnail.outputValid || !m_thumbnail.request ||
       m_thumbnail.generationFence == 0 || completedFence == UINT64_MAX ||
       completedFence < m_thumbnail.generationFence)
    {
        return;
    }

    // No waits: generation and its copy share this already-completed submission fence.
    try
    {
        const ProjectPaths paths(m_thumbnail.request->projectRoot);

        if(ThumbnailFingerprint(paths, m_thumbnail.registry, *m_thumbnail.request, m_thumbnailValidation) == m_thumbnail.fingerprint)
        {
            MaterialThumbnailDiskCache::Image pixels;
            pixels.width = kExtent;
            pixels.height = kExtent;
            pixels.pixels.resize(kExtent * kExtent * 4);
            void* mapped = nullptr;
            const D3D12_RANGE range{static_cast<SIZE_T>(m_thumbnailFootprint.Offset), static_cast<SIZE_T>(m_thumbnailBufferSize)};

            if(FAILED(m_thumbnailReadback->Map(0, &range, &mapped)))
            {
                throw std::runtime_error("Cannot map completed material thumbnail readback.");
            }

            for(size_t row = 0; row < kExtent; ++row)
            {
                memcpy(pixels.pixels.data() + row * kExtent * 4,
                    static_cast<unsigned char*>(mapped) + m_thumbnailFootprint.Offset + row * m_thumbnailFootprint.Footprint.RowPitch,
                    kExtent * 4);
            }

            const D3D12_RANGE written{0, 0};
            m_thumbnailReadback->Unmap(0, &written);
            MaterialThumbnailDiskCache::Store(m_thumbnail.diskSource, pixels);
        }
        else if(m_thumbnail.destination)
        {
            m_thumbnail.destination->stale = true;
        }
    }
    catch(const std::exception& exception)
    {
        Log::Warning(std::string("Material thumbnail disk capture skipped: ") + exception.what());
    }

    m_thumbnail.capturePending = false;
}

MaterialPreviewRenderer::ThumbnailImage* MaterialPreviewRenderer::FindThumbnailImage(
    const std::filesystem::path& root,
    const std::string& identity)
{
    ThumbnailImage* result = nullptr;

    for(auto& image : m_thumbnailImages)
    {
        if(image.projectRoot == root && image.materialIdentity == identity && !image.stale)
        {
            if(!result || image.revision > result->revision)
            {
                result = &image;
            }
        }
    }

    return result;
}

bool MaterialPreviewRenderer::IsThumbnailAdmitted(
    const std::filesystem::path& root, const std::string& identity) const
{
    return !m_thumbnailWorkingSetActive ||
        std::find(m_thumbnailWorkingSet.begin(), m_thumbnailWorkingSet.end(), std::pair{root, identity}) != m_thumbnailWorkingSet.end();
}

bool MaterialPreviewRenderer::CanAcquireThumbnailImage(
    std::uint64_t completedFence, const std::filesystem::path& root, const std::string& identity) const
{
    if(m_thumbnailImages.size() < kThumbnailImageCapacity)
    {
        return true;
    }

    for(const auto& image : m_thumbnailImages)
    {
        const bool sameMaterial = image.projectRoot == root && image.materialIdentity == identity;
        const bool pinned = !image.stale && m_thumbnailWorkingSetActive && IsThumbnailAdmitted(image.projectRoot, image.materialIdentity);

        if((!pinned || sameMaterial) && &image != m_thumbnail.destination && !image.recorded &&
           !image.awaitingDisplaySubmission && image.generationFence <= completedFence && image.displayFence <= completedFence)
        {
            return true;
        }
    }

    return false;
}

MaterialPreviewRenderer::ThumbnailImage* MaterialPreviewRenderer::AcquireThumbnailImage(
    std::uint64_t completedFence, const std::filesystem::path& root, const std::string& identity)
{
    ThumbnailImage* victim = nullptr;

    for(auto& image : m_thumbnailImages)
    {
        const bool sameMaterial = image.projectRoot == root && image.materialIdentity == identity;
        const bool pinned = !image.stale && m_thumbnailWorkingSetActive && IsThumbnailAdmitted(image.projectRoot, image.materialIdentity);

        if((pinned && !sameMaterial) || &image == m_thumbnail.destination || image.recorded || image.awaitingDisplaySubmission ||
           image.generationFence > completedFence || image.displayFence > completedFence)
        {
            continue;
        }

        if(!victim || image.lastUse < victim->lastUse)
        {
            victim = &image;
        }
    }

    if(m_thumbnailImages.size() < kThumbnailImageCapacity)
    {
        m_thumbnailImages.emplace_back();
        victim = &m_thumbnailImages.back();
    }

    if(!victim)
    {
        return nullptr;
    }

    // The allocation is always 256x256 RGBA8. Reuse it only after every reference retires.
    auto allocation = std::move(victim->color);
    *victim = ThumbnailImage{};
    victim->color = std::move(allocation);

    if(!victim->color.GetResource() && !victim->color.Initialize(m_device, kExtent, kExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
            *m_thumbnailRTVHeap, *m_heap))
    {
        victim->color.Shutdown();
        throw std::runtime_error("Cannot allocate a material thumbnail display image.");
    }

    victim->color.GetResource()->SetName(L"Cached Material Thumbnail Display");
    return victim;
}

bool MaterialPreviewRenderer::RequestSavedThumbnail(
    const ProjectPaths& paths,
    const ProjectAssetRegistry& registry,
    const AssetID& id,
    std::uint64_t completedFence,
    std::string& error)
{
    if(m_recording || m_quarantined)
    {
        error = "Preview resources are awaiting a recording outcome or checked recovery.";
        return false;
    }

    error.clear();

    try
    {
        if(!m_initialized || completedFence == UINT64_MAX)
        {
            throw std::runtime_error("Thumbnail renderer unavailable or device removed.");
        }

        const auto root = std::filesystem::canonical(paths.GetRoot());
        const auto identity = id.ToReference();
        const auto* record = registry.Find(id);

        if(!record || record->type != AssetType::Material)
        {
            for(auto& image : m_thumbnailImages)
            {
                if(image.projectRoot == root && image.materialIdentity == identity)
                {
                    image.stale = true;
                }
            }

            throw std::runtime_error("Thumbnail requires a registered Material AssetID.");
        }

        if(root != m_thumbnailProject)
        {
            DiscardThumbnailRequests();
            m_thumbnailValidation.Clear();
            m_thumbnailProject = root;
        }

        if(m_thumbnailWorkingSetActive && !IsThumbnailAdmitted(root, identity))
        {
            if(m_thumbnailWorkingSet.size() >= kThumbnailImageCapacity)
            {
                error = "Material thumbnail is outside the visible working set.";
                return false;
            }

            m_thumbnailWorkingSet.emplace_back(root, identity);
        }

        m_lastThumbnailIdentity = identity;
        auto* image = FindThumbnailImage(root, identity);

        if(image)
        {
            image->lastUse = ++m_thumbnailUseSerial;

            // Full content/dependency validation is throttled, not repeated for every UI call.
            if(std::chrono::steady_clock::now() < image->nextValidation)
            {
                return true;
            }
        }

        if(m_thumbnail.request && m_thumbnail.request->projectRoot == root &&
           m_thumbnail.request->materialIdentity == identity)
        {
            return true;
        }

        for(auto& pending : m_thumbnailRequests)
        {
            if(pending.projectRoot == root && pending.id == id)
            {
                // Keep the latest live registry without changing this request's queue position.
                pending.registry = registry;
                return true;
            }
        }

        if(m_thumbnailRequests.size() >= kThumbnailQueueCapacity)
        {
            throw std::runtime_error("Material thumbnail request queue is full; retry later.");
        }

        m_thumbnailRequests.push_back({root, registry, id});
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void MaterialPreviewRenderer::BeginThumbnailWorkingSet()
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    m_thumbnailWorkingSetActive = true;
    m_thumbnailWorkingSet.clear();
}

void MaterialPreviewRenderer::DiscardThumbnailRequests()
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    m_thumbnailRequests.clear();
    m_thumbnailWorkingSet.clear();
}

void MaterialPreviewRenderer::InvalidateThumbnailImages()
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    m_thumbnailValidation.Clear();
    for(auto& image : m_thumbnailImages)
    {
        image.stale = true;
    }
}

void MaterialPreviewRenderer::InvalidateThumbnailSource(const std::filesystem::path& path)
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);

    if(error)
    {
        InvalidateThumbnailImages();
        return;
    }

    m_thumbnailValidation.Invalidate(canonical);

    // Only CPU validity changes. Existing GPU and current UI references stay protected.
    for(auto& image : m_thumbnailImages)
    {
        image.stale = true;
    }
}

void MaterialPreviewRenderer::ProcessThumbnailRequest(std::uint64_t completedFence)
{
    if(!m_initialized || completedFence == UINT64_MAX)
    {
        return;
    }

    std::erase_if(m_thumbnailRequests, [this](const auto& pending)
    {
        return !IsThumbnailAdmitted(pending.projectRoot, pending.id.ToReference());
    });

    if(m_thumbnail.request)
    {
        if(!m_thumbnail.outputValid || m_thumbnail.recorded || m_thumbnail.capturePending ||
           m_thumbnail.generationFence == 0 || completedFence < m_thumbnail.generationFence)
        {
            return;
        }

        // Display outputs have their own lifetimes; the single worker can now move on.
        m_thumbnail.request.reset();
        m_thumbnail.dependencies = {};
        m_thumbnail.destination = nullptr;
    }

    if(m_thumbnailRequests.empty())
    {
        return;
    }

    auto pending = std::move(m_thumbnailRequests.front());
    m_thumbnailRequests.pop_front();
    // Admission precedes material parsing, hashing, disk I/O, and resource creation.
    const auto* resident = FindThumbnailImage(pending.projectRoot, pending.id.ToReference());

    if((!resident || !resident->ready) &&
       !CanAcquireThumbnailImage(completedFence, pending.projectRoot, pending.id.ToReference()))
    {
        m_thumbnailRequests.push_back(std::move(pending));
        return;
    }

    std::string error;

    if(!PrepareSavedThumbnail(pending, completedFence, error))
    {
        if(error.starts_with("Deferred:"))
        {
            // Round-robin pressure retries prevent a blocked material starving later jobs.
            m_thumbnailRequests.push_back(std::move(pending));
        }
        else
        {
            for(auto& image : m_thumbnailImages)
            {
                if(image.projectRoot == pending.projectRoot && image.materialIdentity == pending.id.ToReference())
                {
                    image.stale = true;
                }
            }

            Log::Warning("Material thumbnail request failed: " + error);
        }
    }
}

void MaterialPreviewRenderer::PollThumbnailCapture(std::uint64_t completedFence)
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    m_thumbnailValidation.Poll();
    PollMeshGPUFences(completedFence);
    PollThumbnailReadback(completedFence);
    ProcessThumbnailRequest(completedFence);
}

std::optional<MaterialPreviewImage> MaterialPreviewRenderer::GetThumbnailImage(const DescriptorHeap& heap)
{
    if(m_recording || m_quarantined)
    {
        return std::nullopt;
    }

    const auto id = AssetID::ParseReference(m_lastThumbnailIdentity);

    if(!id || m_thumbnailProject.empty())
    {
        return std::nullopt;
    }

    return GetThumbnailImage(heap, ProjectPaths(m_thumbnailProject), *id);
}

std::optional<MaterialPreviewImage> MaterialPreviewRenderer::GetThumbnailImage(
    const DescriptorHeap& heap,
    const ProjectPaths& paths,
    const AssetID& id)
{
    if(m_recording || m_quarantined)
    {
        return std::nullopt;
    }

    if(!m_initialized)
    {
        return std::nullopt;
    }

    std::error_code error;
    const auto root = std::filesystem::canonical(paths.GetRoot(), error);
    auto* image = error || !IsThumbnailAdmitted(root, id.ToReference()) ? nullptr : FindThumbnailImage(root, id.ToReference());

    if(!image || !image->ready || image->stale || image->recorded)
    {
        return std::nullopt;
    }

    image->lastUse = ++m_thumbnailUseSerial;
    image->awaitingDisplaySubmission = true;
    return MaterialPreviewImage{heap.GetGPUHandle(image->color.GetSRVIndex()),
        image->projectRoot, image->materialIdentity, image->revision};
}

std::optional<MaterialPreviewImage> MaterialPreviewRenderer::GetImage(const DescriptorHeap& heap) const
{
    if(m_recording || m_quarantined)
    {
        return std::nullopt;
    }

    if(!m_initialized || !m_inspector.outputValid || !m_inspector.request)
    {
        return std::nullopt;
    }

    return MaterialPreviewImage{heap.GetGPUHandle(m_inspector.displayColor.GetSRVIndex()),
        m_inspector.request->projectRoot, m_inspector.request->materialIdentity, m_inspector.request->revision};
}

void MaterialPreviewRenderer::PrepareTextures(
    const std::optional<MaterialPreviewRequest>& request,
    const ProjectAssetRegistry& registry)
{
    if(m_recording || m_quarantined)
    {
        return;
    }

    SetRequest(request);

    const auto preparation = PrepareJobTextures(request, registry, m_inspector.dependencies);

    if(preparation == TexturePreparation::Ready)
    {
        m_inspector.outputValid = false;
    }

    m_inspector.dependencies.ready = preparation == TexturePreparation::Ready ||
        preparation == TexturePreparation::Unchanged;
}

bool MaterialPreviewRenderer::ReclaimTexture(const std::array<Texture*, 7>& preparing)
{
    auto victim = m_textureCache.end();

    for(auto entry = m_textureCache.begin(); entry != m_textureCache.end(); ++entry)
    {
        const auto* texture = &entry->second.texture;
        const auto contains = [texture](const auto& textures)
        {
            return std::find(textures.begin(), textures.end(), texture) != textures.end();
        };

        if(entry->second.awaitingSubmission || entry->second.submissionFence > m_completedTextureFence ||
           contains(m_inspector.dependencies.textures) || contains(m_thumbnail.dependencies.textures) ||
           contains(preparing))
        {
            continue;
        }

        if(victim == m_textureCache.end() || entry->second.lastUse < victim->second.lastUse)
        {
            victim = entry;
        }
    }

    if(victim == m_textureCache.end())
    {
        return false;
    }

    victim->second.texture.Shutdown();
    m_textureCache.erase(victim);
    return true;
}

MaterialPreviewRenderer::TexturePreparation MaterialPreviewRenderer::PrepareJobTextures(
    const std::optional<MaterialPreviewRequest>& request,
    const ProjectAssetRegistry& registry,
    TextureDependencies& dependencies,
    bool contentVersions)
{
    if(!m_initialized || !request)
    {
        dependencies = {};
        return TexturePreparation::Unchanged;
    }

    // Build new bindings separately: capacity pressure must never publish partial bindings.
    TextureDependencies prepared;
    prepared.request = request;
    prepared.registrySnapshot = registry.GetAssets();
    const ProjectPaths paths(request->projectRoot);

    for(size_t index = 0; index < request->textures.size(); ++index)
    {
        if(!request->textures[index])
        {
            continue;
        }

        Texture unpublished;

        try
        {
            const auto id = AssetID::ParseReference(*request->textures[index]);
            const auto* record = id ? registry.Find(*id) : nullptr;

            if(!record || record->type != AssetType::Texture)
            {
                // Existing missing registrations use the established scalar fallback.
                if(id && !record)
                {
                    continue;
                }

                throw std::runtime_error("Reference is invalid or registered with a non-Texture type.");
            }

            const auto resolved = ProjectAssetResolver::Resolve(paths, registry, *id);

            if(resolved.status == AssetAvailability::Missing)
            {
                continue;
            }

            if(resolved.status != AssetAvailability::Available)
            {
                throw std::runtime_error("Texture is inaccessible or has an unsafe path: " + id->ToReference());
            }

            const auto interpretation = index == 0 ? ProjectTextureInterpretation::SRGBColor :
                (index == 1 ? ProjectTextureInterpretation::NormalMap : ProjectTextureInterpretation::LinearData);
            const TextureKey key{std::filesystem::canonical(paths.GetRoot()), id->ToString(), interpretation,
                resolved.resolvedPath, std::filesystem::last_write_time(resolved.resolvedPath),
                std::filesystem::file_size(resolved.resolvedPath), contentVersions ? m_thumbnailValidation.Digest(resolved.resolvedPath) :
                    "refresh:" + std::to_string(request->dependencyRevision)};
            const auto cached = m_textureCache.find(key);

            if(cached != m_textureCache.end())
            {
                cached->second.lastUse = ++m_textureUseSerial;
                prepared.textures[index] = &cached->second.texture;
                continue;
            }

            if(m_textureCache.size() >= kTextureCapacity && !ReclaimTexture(prepared.textures))
            {
                return TexturePreparation::Deferred;
            }

            std::string error;

            if(!LoadProjectTexture(m_device, *m_uploader, *m_heap,
                    resolved.resolvedPath, interpretation, unpublished, &error))
            {
                throw std::runtime_error(error);
            }

            auto [entry, inserted] = m_textureCache.try_emplace(key);
            entry->second.texture = std::move(unpublished);
            entry->second.lastUse = ++m_textureUseSerial;
            prepared.textures[index] = &entry->second.texture;
        }
        catch(const std::exception& error)
        {
            unpublished.Shutdown();
            Log::Error(std::string("Material preview texture load failed: ") + error.what());

            if(contentVersions)
            {
                return TexturePreparation::Failed;
            }
        }
    }

    const bool changed = !dependencies.ready || dependencies.textures != prepared.textures ||
        !dependencies.request || dependencies.request->projectRoot != request->projectRoot ||
        dependencies.request->textures != request->textures ||
        dependencies.request->dependencyRevision != request->dependencyRevision;
    prepared.ready = true;
    dependencies = std::move(prepared);
    return changed ? TexturePreparation::Ready : TexturePreparation::Unchanged;
}

bool MaterialPreviewRenderer::DescribeGeometry(const Mesh& mesh, const Matrix4& model,
    PreviewGeometry& geometry, std::string& error)
{
    error.clear();
    const auto vertexCount = mesh.GetVertexCount();
    const auto indexCount = mesh.GetIndexCount();
    auto* vertices = mesh.GetVertexBufferResource();
    auto* indices = mesh.GetIndexBufferResource();

    if(!mesh.IsIndexed() || !vertices || !indices || vertexCount == 0 || indexCount == 0 ||
        mesh.GetVertexStride() != sizeof(Vertex) || mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT ||
        vertexCount > UINT_MAX / sizeof(Vertex) || indexCount > UINT_MAX / sizeof(uint32_t) ||
        mesh.GetVertexBufferAddress() == 0 || mesh.GetIndexBufferAddress() == 0)
    {
        error = "Preview requires populated indexed geometry using the Engine Vertex layout and 32-bit indices.";
        return false;
    }

    const auto expected = CreateVertexLayout();
    const auto& attributes = mesh.GetVertexLayout().GetAttributes();
    const auto& expectedAttributes = expected.GetAttributes();

    if(attributes.size() != expectedAttributes.size())
    {
        error = "Preview vertex attributes do not match the preview pipeline.";
        return false;
    }

    for(size_t i = 0; i < attributes.size(); ++i)
    {
        const auto& actual = attributes[i];
        const auto& required = expectedAttributes[i];

        if(!actual.semantic || std::strcmp(actual.semantic, required.semantic) != 0 ||
            actual.semanticIndex != required.semanticIndex || actual.offset != required.offset || actual.type != required.type)
        {
            error = "Preview vertex attributes do not match the preview pipeline.";
            return false;
        }
    }

    if(vertices->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        indices->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        vertices->GetDesc().Width < vertexCount * sizeof(Vertex) ||
        indices->GetDesc().Width < indexCount * sizeof(uint32_t) ||
        !ValidateMeshMetadata(mesh.GetMetadata(), indexCount, error))
    {
        if(error.empty())
        {
            error = "Preview geometry exceeds its GPU buffer allocation.";
        }

        return false;
    }

    PreviewFraming checkedFraming;
    Matrix4 checkedModel;

    if(!FrameMeshBounds(mesh.GetBounds(), checkedFraming, checkedModel, error))
    {
        return false;
    }

    for(const auto& row : model.m)
    {
        for(float value : row)
        {
            if(!std::isfinite(value))
            {
                error = "Preview model transform must be finite.";
                return false;
            }
        }
    }

    PreviewGeometry candidate;
    candidate.vertices = {mesh.GetVertexBufferAddress(), static_cast<UINT>(vertexCount * sizeof(Vertex)), sizeof(Vertex)};
    candidate.indices = {mesh.GetIndexBufferAddress(), static_cast<UINT>(indexCount * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    candidate.indexCount = static_cast<unsigned int>(indexCount);

    candidate.ranges = mesh.GetMetadata().submeshes;
    candidate.bounds = mesh.GetBounds();
    candidate.model = model;
    geometry = candidate;
    return true;
}

bool MaterialPreviewRenderer::FrameMeshBounds(const MeshBounds& bounds, PreviewFraming& framing,
    Matrix4& model, std::string& error)
{
    error.clear();
    const double minimum[] = {bounds.min.x, bounds.min.y, bounds.min.z};
    const double maximum[] = {bounds.max.x, bounds.max.y, bounds.max.z};
    double center[3]{};
    double halfExtent[3]{};

    if(!bounds.valid)
    {
        error = "Preview mesh bounds are unavailable.";
        return false;
    }

    for(size_t axis = 0; axis < 3; ++axis)
    {
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || maximum[axis] < minimum[axis])
        {
            error = "Preview mesh bounds must be finite and ordered.";
            return false;
        }

        center[axis] = minimum[axis] * 0.5 + maximum[axis] * 0.5;
        halfExtent[axis] = (maximum[axis] - minimum[axis]) * 0.5;
    }

    const double radius = std::hypot(halfExtent[0], halfExtent[1], halfExtent[2]);

    if(!std::isfinite(radius) || radius <= 0)
    {
        error = "Preview mesh bounds have zero or invalid enclosing radius.";
        return false;
    }

    const float scale = static_cast<float>(1.0 / radius);

    // The unchanged preview shader transforms normals with the model matrix.
    // Reject scales whose normal lengths would underflow/overflow float math.
    if(!std::isfinite(scale) || scale < std::sqrt((std::numeric_limits<float>::min)()) ||
        scale > std::sqrt((std::numeric_limits<float>::max)() / 4.0f))
    {
        error = "Preview bounds require a normalization scale outside the shader's safe float range.";
        return false;
    }

    Matrix4 normalized = Matrix4::Scale(scale, scale, scale);

    for(size_t axis = 0; axis < 3; ++axis)
    {
        // Combine scaling and centering in double before narrowing to float.
        const double translation = -center[axis] * double(scale);

        if(!std::isfinite(translation) || std::abs(translation) > (std::numeric_limits<float>::max)())
        {
            error = "Preview bounds center cannot be represented by a finite normalized transform.";
            return false;
        }

        normalized.m[axis][3] = static_cast<float>(translation);
    }

    // Positive uniform scaling encloses every bounds corner in a unit sphere,
    // including elongated models. Increase its projected radius by 30% while
    // reserving 10% of the half-FOV as margin; material-sphere framing is separate.
    PreviewFraming candidate;
    // Orbit the mesh thumbnail camera 180 degrees around Y to show model fronts.
    const Vector3 originalEye{-2.6f, 1.7f, 3.2f};
    const double originalDistance = originalEye.Length();
    constexpr double halfFov = 0.65 * 0.5;
    constexpr double zoom = 1.30;
    const double zoomDistance = std::sqrt(1.0 + (originalDistance * originalDistance - 1.0) / (zoom * zoom));
    const double minimumDistance = 1.0 / std::sin(halfFov * 0.90);
    const double distance = (std::max)(zoomDistance, minimumDistance);

    candidate.eye = originalEye * static_cast<float>(distance / originalDistance);
    candidate.viewProjection = Matrix4::Perspective(0.65f, 1.0f, 0.1f, 20.0f) *
        Matrix4::LookAt(candidate.eye, Vector3{}, Vector3{0.0f, 1.0f, 0.0f});
    framing = candidate;
    model = normalized;
    return true;
}

bool MaterialPreviewRenderer::RequestMeshThumbnail(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetID& id, std::uint64_t& ticket, std::string& error,
    MeshThumbnailConsumer consumer)
{
    if(!m_initialized || m_recording || m_quarantined)
    {
        ticket = 0;
        error = "Mesh thumbnail preparation requires an initialized preview renderer.";
        return false;
    }

    if(!m_meshPreparation.Request(paths, registry, id, ticket, error))
    {
        return false;
    }

    if(std::find(m_meshTickets.begin(), m_meshTickets.end(), ticket) == m_meshTickets.end())
    {
        m_meshTickets.push_back(ticket);
    }

    m_meshConsumers[ticket] |= static_cast<unsigned int>(consumer);

    return true;
}

void MaterialPreviewRenderer::PollMeshThumbnailPreparation(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    if(m_recording || m_quarantined || !m_initialized)
    {
        return;
    }

    m_meshPreparation.Poll(paths, registry);

    if(m_meshWriter.valid() && m_meshWriter.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            m_meshWriter.get();
        }
        catch(...)
        {
            // Optional cache writes are isolated from preview generation.
        }
    }

    for(auto& job : m_meshJobs)
    {
        const auto* prepared = m_meshPreparation.Find(job.ticket);

        if(!prepared || prepared->state == MeshThumbnailPreparation::State::Cancelled ||
            prepared->state == MeshThumbnailPreparation::State::Failed)
        {
            job.stale = true;
            job.ready = false;
            continue;
        }

        if(!job.rendered || job.fence > m_meshCompletedFence || job.stale)
        {
            continue;
        }

        if(!job.validating && !job.ready)
        {
            const auto id = AssetID::ParseReference(job.identity.assetReference);
            std::uint64_t ticket = 0;
            std::string error;
            job.validating = true;

            if(!id || !m_meshPreparation.Request(paths, registry, *id, ticket, error) || ticket != job.ticket)
            {
                job.stale = true;
                job.error = error.empty() ? "Mesh identity changed before thumbnail publication." : error;
            }

            continue;
        }

        if(job.validating && (prepared->state == MeshThumbnailPreparation::State::Prepared ||
            prepared->state == MeshThumbnailPreparation::State::CacheHit))
        {
            job.ready = prepared->identity.sourceFingerprint == job.identity.sourceFingerprint &&
                prepared->identity.units == job.identity.units;
            job.stale = !job.ready;
            job.validating = false;
        }

        if(job.ready && !job.stale)
        {
            PersistMeshImage(job);
        }
    }

    for(const auto ticket : m_meshTickets)
    {
        PrepareMeshGPUJob(ticket);
    }

    std::erase_if(m_meshTickets, [this](std::uint64_t ticket)
        { return m_meshPreparation.Find(ticket) == nullptr; });
}

const MeshThumbnailPreparation::Job* MaterialPreviewRenderer::GetMeshThumbnailPreparation(std::uint64_t ticket) const noexcept
{
    return m_meshPreparation.Find(ticket);
}

void MaterialPreviewRenderer::CancelMeshThumbnailPreparation(std::uint64_t ticket)
{
    m_meshPreparation.Cancel(ticket);

    for(auto& job : m_meshJobs)
    {
        if(job.ticket == ticket)
        {
            job.stale = true;
            job.ready = false;
        }
    }
}

void MaterialPreviewRenderer::DiscardMeshThumbnailPreparation(std::uint64_t ticket, MeshThumbnailConsumer consumer)
{
    const auto owners = m_meshConsumers.find(ticket);

    if(owners != m_meshConsumers.end())
    {
        owners->second &= ~static_cast<unsigned int>(consumer);

        if(owners->second != 0)
        {
            return;
        }

        m_meshConsumers.erase(owners);
    }

    CancelMeshThumbnailPreparation(ticket);
    m_meshPreparation.Discard(ticket);
    std::erase(m_meshTickets, ticket);
}

bool MaterialPreviewRenderer::BeginRecording(std::uint64_t generation)
{
    if(!m_initialized)
    {
        return true; // Renderer also records initialization work before previews exist.
    }

    if(generation == 0 || m_recording || m_quarantined)
    {
        return false;
    }

    RecordingSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.inspectorValid = m_inspector.outputValid;
    snapshot.thumbnailValid = m_thumbnail.outputValid;
    snapshot.capturePending = m_thumbnail.capturePending;

    snapshot.depthState = m_gBuffer.GetDepth().GetState();
    RenderTarget* targets[] = {&m_gBuffer.GetMaterial(), &m_gBuffer.GetSurface(), &m_gBuffer.GetMotion(),
        &m_gBuffer.GetGeometricNormal(), &m_hdrColor, &m_inspector.displayColor, &m_thumbnail.displayColor};

    for(auto* target : targets)
    {
        snapshot.targets.push_back({target, target->GetState()});
    }

    for(auto& image : m_thumbnailImages)
    {
        snapshot.targets.push_back({&image.color, image.color.GetState()});
        snapshot.images.push_back({&image, image.ready, image.recorded});
    }

    for(auto& job : m_meshJobs)
    {
        snapshot.targets.push_back({&job.color, job.color.GetState()});
    }

    for(auto& [key, texture] : m_textureCache)
    {
        snapshot.textures.push_back({&texture, texture.texture.GetSubresourceStates(), texture.awaitingSubmission});
    }

    m_recording = std::move(snapshot);
    m_inspector.recorded = false;
    m_thumbnail.recorded = false;
    return true;
}

bool MaterialPreviewRenderer::DiscardRecording(std::uint64_t generation)
{
    if(!m_recording)
    {
        return !m_quarantined;
    }

    if(m_recording->generation != generation || m_quarantined)
    {
        return false;
    }

    for(const auto& target : m_recording->targets)
    {
        target.target->RestoreStateAfterAbort(target.state);
    }

    m_gBuffer.GetDepth().RestoreStateAfterAbort(m_recording->depthState);

    for(auto& texture : m_recording->textures)
    {
        texture.entry->texture.RestoreStatesAfterAbort(texture.states);
        texture.entry->awaitingSubmission = texture.awaitingSubmission;
    }

    for(const auto& image : m_recording->images)
    {
        image.image->ready = image.ready;
        image.image->recorded = image.recorded;
        image.image->awaitingDisplaySubmission = false;
    }

    m_inspector.outputValid = m_recording->inspectorValid;
    m_thumbnail.outputValid = m_recording->thumbnailValid;
    m_thumbnail.capturePending = m_recording->capturePending;

    m_inspector.recorded = false;
    m_thumbnail.recorded = false;

    for(auto& job : m_meshJobs)
    {
        if(job.recorded)
        {
            job.staging.Reset();
            job.recorded = false;
            job.recordedBytes = 0;
            job.renderRecorded = false;
        }

        job.awaitingDisplaySubmission = false;
    }

    m_recording.reset();
    return true;
}

void MaterialPreviewRenderer::MarkSubmissionUncertain(std::uint64_t generation)
{
    if(m_recording && m_recording->generation == generation)
    {
        m_quarantined = true;
    }
}

namespace
{
    bool CreatePreviewBuffer(ID3D12Device* device, size_t bytes, D3D12_HEAP_TYPE type,
        Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;

        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
            type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;

        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &description, state, nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));
    }
}

void MaterialPreviewRenderer::PrepareMeshGPUJob(std::uint64_t ticket)
{
    if(std::any_of(m_meshJobs.begin(), m_meshJobs.end(), [ticket](const auto& job)
        { return job.ticket == ticket; }))
    {
        return;
    }

    // Completed images own no geometry. At most one job retains source/destination buffers.
    if(std::any_of(m_meshJobs.begin(), m_meshJobs.end(), [](const auto& job)
        { return job.vertices || job.indices; }))
    {
        return;
    }

    const auto* prepared = m_meshPreparation.Find(ticket);

    if(!prepared || (prepared->state != MeshThumbnailPreparation::State::Prepared &&
        prepared->state != MeshThumbnailPreparation::State::CacheHit))
    {
        return;
    }

    if(m_meshJobs.size() >= kMeshImageCapacity)
    {
        const auto victim = std::find_if(m_meshJobs.begin(), m_meshJobs.end(), [this](const auto& job)
        {
            return !job.recorded && !job.awaitingDisplaySubmission &&
                job.fence <= m_meshCompletedFence && job.displayFence <= m_meshCompletedFence;
        });

        if(victim == m_meshJobs.end())
        {
            return;
        }

        m_meshPreparation.Discard(victim->ticket);
        m_meshConsumers.erase(victim->ticket);
        // Remove expired ticket entries after the preparation enumeration completes.
        victim->color.Shutdown();
        m_meshJobs.erase(victim);
    }

    m_meshJobs.emplace_back();
    auto& job = m_meshJobs.back();
    job.ticket = ticket;
    job.identity = prepared->identity;
    job.diskSource = prepared->diskSource;
    job.diskHit = prepared->state == MeshThumbnailPreparation::State::CacheHit;

    if(job.diskHit)
    {
        if(!job.color.Initialize(m_device, kExtent, kExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
            *m_thumbnailRTVHeap, *m_heap) || !m_meshPreparation.TakeCachedPixels(ticket, job.cachedPixels))
        {
            job.error = "Could not prepare cached mesh thumbnail image.";
            job.stale = true;
            return;
        }

        const auto description = job.color.GetResource()->GetDesc();
        m_device->GetCopyableFootprints(&description, 0, 1, 0, &job.footprint, nullptr, nullptr, &job.imageBytes);
        return;
    }

    const auto& geometry = prepared->geometry;
    const size_t vertexBytes = geometry.vertices.size() * sizeof(Vertex);
    const size_t indexBytes = geometry.indices.size() * sizeof(std::uint32_t);
    const MeshBounds bounds{geometry.boundsMin, geometry.boundsMax, true};

    if(vertexBytes == 0 || indexBytes == 0 || vertexBytes > MeshThumbnailPreparation::kMaximumJobBytes ||
        indexBytes > MeshThumbnailPreparation::kMaximumJobBytes - vertexBytes ||
        geometry.metadata.submeshes.size() > 4096 ||
        !ValidateMeshMetadata(geometry.metadata, geometry.indices.size(), job.error) ||
        !FrameMeshBounds(bounds, job.framing, job.model, job.error))
    {
        job.error = "Mesh thumbnail geometry exceeds preview limits or has invalid ranges/bounds. " + job.error;
        job.stale = true;
        return;
    }

    if(!CreatePreviewBuffer(m_device, vertexBytes, D3D12_HEAP_TYPE_DEFAULT, job.vertices) ||
        !CreatePreviewBuffer(m_device, indexBytes, D3D12_HEAP_TYPE_DEFAULT, job.indices) ||
        !job.color.Initialize(m_device, kExtent, kExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
            *m_thumbnailRTVHeap, *m_heap))
    {
        job.error = "Could not allocate preview-owned mesh geometry or thumbnail target.";
        job.vertices.Reset();
        job.indices.Reset();
        job.color.Shutdown();
        job.stale = true;
        return;
    }

    if(!m_meshPreparation.TakeGeometry(ticket, job.geometry))
    {
        job.error = "Prepared mesh geometry became unavailable before upload.";
        job.stale = true;
    }

    const auto description = job.color.GetResource()->GetDesc();
    m_device->GetCopyableFootprints(&description, 0, 1, 0, &job.footprint, nullptr, nullptr, &job.imageBytes);
    // Readback is optional. Allocation failure never prevents rendering/display.
    CreatePreviewBuffer(m_device, static_cast<size_t>(job.imageBytes), D3D12_HEAP_TYPE_READBACK, job.readback);
}

bool MaterialPreviewRenderer::HasMeshWork() const
{
    if(std::any_of(m_meshJobs.begin(), m_meshJobs.end(), [](const auto& job) { return bool(job.staging); }))
    {
        return false;
    }

    return std::any_of(m_meshJobs.begin(), m_meshJobs.end(), [this](const auto& job)
    {
        const auto* prepared = m_meshPreparation.Find(job.ticket);
        return !job.stale && !job.rendered && !job.recorded && !job.staging &&
            (job.vertices || job.diskHit) && job.fence <= m_meshCompletedFence && prepared &&
            prepared->state != MeshThumbnailPreparation::State::Cancelled &&
            prepared->state != MeshThumbnailPreparation::State::Failed;
    });
}

bool MaterialPreviewRenderer::RecordMeshJob(const RecordingContext& context)
{
    for(auto& job : m_meshJobs)
    {
        if(job.stale || job.rendered || job.recorded || job.staging || (!job.vertices && !job.diskHit) ||
            job.fence > m_meshCompletedFence)
        {
            continue;
        }

        if(job.diskHit)
        {
            return RecordCachedMeshImage(context, job);
        }

        const auto* prepared = m_meshPreparation.Find(job.ticket);

        if(!prepared || prepared->state == MeshThumbnailPreparation::State::Cancelled ||
            prepared->state == MeshThumbnailPreparation::State::Failed)
        {
            continue;
        }

        const size_t vertexBytes = job.geometry.vertices.size() * sizeof(Vertex);
        const size_t indexBytes = job.geometry.indices.size() * sizeof(std::uint32_t);
        const size_t end = (std::min)(vertexBytes + indexBytes, job.copiedBytes + kMeshUploadBytes);
        const size_t bytes = end - job.copiedBytes;

        if(bytes == 0 || !CreatePreviewBuffer(m_device, bytes, D3D12_HEAP_TYPE_UPLOAD, job.staging))
        {
            job.error = "Could not allocate bounded mesh thumbnail upload staging.";
            job.stale = true;
            return true;
        }

        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};

        if(FAILED(job.staging->Map(0, &noRead, &mapped)))
        {
            job.staging.Reset();
            job.error = "Could not map mesh thumbnail staging buffer.";
            job.stale = true;
            return true;
        }

        const size_t vertexEnd = (std::min)(end, vertexBytes);
        const size_t vertexStart = (std::min)(job.copiedBytes, vertexBytes);
        const size_t vertexCopy = vertexEnd - vertexStart;
        const size_t indexStart = job.copiedBytes > vertexBytes ? job.copiedBytes - vertexBytes : 0;
        const size_t indexCopy = bytes - vertexCopy;

        if(vertexCopy)
        {
            std::memcpy(mapped, reinterpret_cast<const std::byte*>(job.geometry.vertices.data()) + vertexStart,
                vertexCopy);
        }

        if(indexCopy)
        {
            std::memcpy(static_cast<std::byte*>(mapped) + vertexCopy,
                reinterpret_cast<const std::byte*>(job.geometry.indices.data()) + indexStart, indexCopy);
        }

        job.staging->Unmap(0, nullptr);
        // Protect all referenced allocations before emitting any GPU commands.
        job.recorded = true;
        job.recordedBytes = bytes;

        // Default-heap buffers start/decay to COMMON at ExecuteCommandLists boundaries.
        // Each slice waits only by polling its fence; copy commands implicitly promote to COPY_DEST.
        if(vertexCopy)
        {
            context.commandList->CopyBufferRegion(job.vertices.Get(), vertexStart, job.staging.Get(), 0, vertexCopy);
        }

        if(indexCopy)
        {
            context.commandList->CopyBufferRegion(job.indices.Get(), indexStart, job.staging.Get(), vertexCopy, indexCopy);
        }

        if(end != vertexBytes + indexBytes)
        {
            return true;
        }

        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = {job.vertices.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            vertexCopy ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER};
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition = {job.indices.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            indexCopy ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_INDEX_BUFFER};
        context.commandList->ResourceBarrier(2, barriers);

        PreviewGeometry geometry;
        geometry.vertices = {job.vertices->GetGPUVirtualAddress(), static_cast<UINT>(vertexBytes), sizeof(Vertex)};
        geometry.indices = {job.indices->GetGPUVirtualAddress(), static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
        geometry.ranges = job.geometry.metadata.submeshes;
        geometry.bounds = {job.geometry.boundsMin, job.geometry.boundsMax, true};

        geometry.model = job.model;
        geometry.indexCount = static_cast<unsigned int>(job.geometry.indices.size());
        Material neutral;
        std::fill(std::begin(neutral.baseColor), std::end(neutral.baseColor), 0.5f);

        job.renderRecorded = RecordGeometryJob(context, neutral.GetGPUData(), {}, job.color, geometry, job.framing);

        if(job.renderRecorded)
        {
            RecordMeshReadback(context, job);
        }

        return job.renderRecorded;
    }

    return true;
}

void MaterialPreviewRenderer::PollMeshGPUFences(std::uint64_t completedFence)
{
    if(completedFence == UINT64_MAX || m_recording || m_quarantined)
    {
        return;
    }

    m_meshCompletedFence = (std::max)(m_meshCompletedFence, completedFence);

    for(auto& job : m_meshJobs)
    {
        if(job.recorded || job.fence > m_meshCompletedFence)
        {
            continue;
        }

        job.staging.Reset();

        if(job.rendered || job.stale)
        {
            job.vertices.Reset();
            job.indices.Reset();
            job.geometry = {};
            job.cachedPixels = {};
        }

        if(job.stale)
        {
            job.readback.Reset();
        }
    }
}

size_t MaterialPreviewRenderer::ReleaseIdleMeshThumbnailImages()
{
    if(m_recording || m_quarantined)
    {
        return 0;
    }

    size_t released = 0;

    for(auto job = m_meshJobs.begin(); job != m_meshJobs.end();)
    {
        const bool idle = !job->recorded && !job->awaitingDisplaySubmission &&
            job->fence <= m_meshCompletedFence && job->displayFence <= m_meshCompletedFence &&
            !job->vertices && !job->indices;

        if(!idle)
        {
            ++job;
            continue;
        }

        // Same ticket cleanup as capacity eviction in the mesh job admission path.
        m_meshPreparation.Discard(job->ticket);
        m_meshConsumers.erase(job->ticket);
        job->color.Shutdown();
        job = m_meshJobs.erase(job);
        ++released;
    }

    return released;
}

std::optional<MaterialPreviewRenderer::MeshThumbnailImage> MaterialPreviewRenderer::GetMeshThumbnailImage(
    const DescriptorHeap& heap, std::uint64_t ticket)
{
    if(m_recording || m_quarantined)
    {
        return std::nullopt;
    }

    for(auto& job : m_meshJobs)
    {
        const auto* prepared = m_meshPreparation.Find(ticket);

        if(job.ticket == ticket && job.ready && !job.stale && prepared &&
            prepared->state != MeshThumbnailPreparation::State::Cancelled &&
            prepared->state != MeshThumbnailPreparation::State::Failed)
        {
            job.awaitingDisplaySubmission = true;
            return MeshThumbnailImage{heap.GetGPUHandle(job.color.GetSRVIndex()), job.identity};
        }
    }

    return std::nullopt;
}

std::string MaterialPreviewRenderer::GetMeshThumbnailError(std::uint64_t ticket) const
{
    for(const auto& job : m_meshJobs)
    {
        if(job.ticket == ticket && !job.error.empty())
        {
            return job.error;
        }
    }

    const auto* prepared = m_meshPreparation.Find(ticket);
    return prepared ? prepared->error : "Mesh thumbnail request is no longer retained.";
}

bool MaterialPreviewRenderer::RecordCachedMeshImage(const RecordingContext& context, MeshGPUJob& job)
{
    if(job.cachedPixels.width != kExtent || job.cachedPixels.height != kExtent ||
        job.cachedPixels.pixels.size() != kExtent * kExtent * 4 || job.imageBytes > kMeshUploadBytes ||
        !CreatePreviewBuffer(m_device, static_cast<size_t>(job.imageBytes), D3D12_HEAP_TYPE_UPLOAD, job.staging))
    {
        job.error = "Cached mesh thumbnail pixels or upload allocation are invalid.";
        job.stale = true;
        return true;
    }

    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};

    if(FAILED(job.staging->Map(0, &noRead, &mapped)))
    {
        job.staging.Reset();
        job.error = "Could not map cached mesh thumbnail upload.";
        job.stale = true;
        return true;
    }

    for(size_t row = 0; row < kExtent; ++row)
    {
        std::memcpy(static_cast<unsigned char*>(mapped) + job.footprint.Offset + row * job.footprint.Footprint.RowPitch,
            job.cachedPixels.pixels.data() + row * kExtent * 4, kExtent * 4);
    }

    job.staging->Unmap(0, nullptr);
    job.recorded = true;
    job.renderRecorded = true;
    job.color.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = job.staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = job.footprint;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = job.color.GetResource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    context.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    job.color.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void MaterialPreviewRenderer::RecordMeshReadback(const RecordingContext& context, MeshGPUJob& job)
{
    if(!job.readback)
    {
        return;
    }

    job.color.Transition(context.commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = job.color.GetResource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = job.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = job.footprint;

    context.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    job.color.Transition(context.commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void MaterialPreviewRenderer::PersistMeshImage(MeshGPUJob& job)
{
    if(job.diskHit || job.persistenceAttempted || !job.readback || m_meshWriter.valid() ||
        !job.ready || job.stale || !job.rendered || job.fence > m_meshCompletedFence)
    {
        return;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(job.imageBytes)};

    if(FAILED(job.readback->Map(0, &readRange, &mapped)))
    {
        job.persistenceAttempted = true;
        job.readback.Reset();
        return;
    }

    MaterialThumbnailDiskCache::Image pixels{kExtent, kExtent, {}};

    try
    {
        pixels.pixels.resize(kExtent * kExtent * 4);

        for(size_t row = 0; row < kExtent; ++row)
        {
            std::memcpy(pixels.pixels.data() + row * kExtent * 4,
                static_cast<const unsigned char*>(mapped) + job.footprint.Offset + row * job.footprint.Footprint.RowPitch,
                kExtent * 4);
        }
    }
    catch(...)
    {
        const D3D12_RANGE noWrite{0, 0};
        job.readback->Unmap(0, &noWrite);
        job.readback.Reset();
        job.persistenceAttempted = true;
        return;
    }

    const D3D12_RANGE noWrite{0, 0};
    job.readback->Unmap(0, &noWrite);
    job.readback.Reset();
    job.persistenceAttempted = true;

    try
    {
        // Registry/project/source identity was checked by the current main-thread poll.
        // Worker pins the source and rehashes before publishing; no Renderer/project borrows.
        m_meshWriter = std::async(std::launch::async,
            [source = job.diskSource, digest = job.identity.sourceFingerprint, pixels = std::move(pixels)]()
        {
            HANDLE input = CreateFileW(source.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

            if(input == INVALID_HANDLE_VALUE)
            {
                return;
            }

            const std::shared_ptr<void> sourceLock(input, [](void* handle) { CloseHandle(handle); });

            try
            {
                MaterialThumbnailValidation validation;

                if(validation.Digest(source.path) == digest)
                {
                    MaterialThumbnailDiskCache::Store(source, pixels);
                }
            }
            catch(...)
            {
                // Optional disk I/O/hash failures do not invalidate a completed GPU image.
            }
        });
    }
    catch(...)
    {
        // A failed worker launch is a nonfatal cache miss on the next application run.
    }
}
