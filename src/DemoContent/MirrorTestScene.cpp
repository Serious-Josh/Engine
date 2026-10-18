#include "Platform/Windows/WindowsWindow.h"
#include "DemoContent/MirrorTestScene.h"
#include "DemoContent/SceneRegistry.h"

#include "Renderer/ResourceUploader.h"
#include "Renderer/Renderer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Vertex.h"
#include "Renderer/TangentGenerator.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/Geometry/DisplacementMeshGenerator.h"
#include "Renderer/DDSLoader.h"

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"

#include "Core/Log.h"

#include <cstdint>
#include <array>
#include <vector>
#include <algorithm>
#include <limits>
#include <cmath>
#include <atomic>
#include <cstdlib>

namespace
{
    std::uint64_t AllocateDisplacementRevision()
    {
        static std::atomic<std::uint64_t> next{1};
        std::uint64_t revision = next.load(std::memory_order_relaxed);
        while (revision != UINT64_MAX)
        {
            if (next.compare_exchange_weak(revision, revision + 1, std::memory_order_relaxed))
            {
                return revision;
            }
        }
        // Never wrap and let an old acknowledgment match a new request.

        Log::Error("Displacement request revisions exhausted.");
        std::abort();
    }
}

REGISTER_DEMO_SCENE("Mirror Test", MirrorTestScene);

MirrorTestScene::~MirrorTestScene()
{
    if (m_heightmapRegistrationOwner)
    {
        Log::Error("Mirror Test destroyed with a registered displacement heightmap.");
        std::abort();
    }
}

bool MirrorTestScene::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    if (m_heightmapRegistrationOwner)
    {
        Log::Error("Cannot reinitialize Mirror Test while its heightmap is registered.");
        return false;
    }

    m_initialized = false;
    m_vertexLayout = CreateVertexLayout();

    m_cameraPatchLODsInitialized = false;
    m_requestedDisplacementRevision = 0;
    m_displacementGeometryUpdatePending = false;

    if (!vertexShader || !pixelShader)
    {
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;

    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = &m_vertexLayout;

    pipelineDescription.renderTargets.count = 4;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.renderTargets.formats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 9, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_ALL});

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX});

    pipelineDescription.depthEnabled = true;
    pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;

    if (!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    SphereGeometry sphere = SphereGenerator::Generate(1.0f, 32, 64);
    TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);

    if (!m_sphereMesh.Initialize(device, uploader, descriptorHeap, sphere.vertices.data(), sphere.vertices.size(),
                                 m_vertexLayout, sphere.indices.data(), sphere.indices.size()))
    {
        return false;
    }

    const Vertex planeVertices[] = {{{-1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
                                    {{-1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
                                    {{1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
                                    {{1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}}};

    const unsigned int planeIndices[] = {0, 1, 2, 0, 2, 3};

    if (!m_planeMesh.Initialize(device, uploader, descriptorHeap, planeVertices, 4, m_vertexLayout, planeIndices, 6))
    {
        return false;
    }

    RetainEditorGeometry(m_sphereMesh, std::move(sphere.vertices), std::move(sphere.indices));
    RetainEditorGeometry(m_planeMesh, std::vector<Vertex>(std::begin(planeVertices), std::end(planeVertices)),
        std::vector<uint32_t>(std::begin(planeIndices), std::end(planeIndices)));
    // GPU-generated displacement has no authoritative CPU triangle snapshot.

    // Temporary CPU displacement heightmap validation.
    DirectX::ScratchImage displacementDDS;

    if (!DDSLoader::Load((WindowsWindow::GetExecutableDirectory() / "assets/Textures/Materials/stacked_brick_wall_disp_4k.dds"), displacementDDS))
    {
        return false;
    }

    const DirectX::TexMetadata& displacementMetadata = displacementDDS.GetMetadata();

    if (displacementMetadata.format != DXGI_FORMAT_BC4_UNORM)
    {
        Log::Error("Displacement heightmap must use BC4 UNORM.");

        return false;
    }

    DirectX::ScratchImage decompressedHeightmap;

    constexpr std::size_t displacementMip = 4;

    if (displacementMetadata.mipLevels <= displacementMip)
    {
        Log::Error("Displacement DDS does not contain mip level 5.");

        return false;
    }

    const DirectX::Image* sourceHeightImage = displacementDDS.GetImage(displacementMip, 0, 0);

    if (!sourceHeightImage)
    {
        Log::Error("Displacement DDS has no base mip.");

        return false;
    }

    const HRESULT decompressResult =
        DirectX::Decompress(*sourceHeightImage, DXGI_FORMAT_R8_UNORM, decompressedHeightmap);

    if (FAILED(decompressResult))
    {
        Log::Error("Failed to decompress displacement heightmap.");
        return false;
    }

    const DirectX::Image* heightImage = decompressedHeightmap.GetImage(0, 0, 0);

    if (!heightImage || !heightImage->pixels)
    {
        Log::Error("Displacement heightmap has no valid pixels.");

        return false;
    }

    std::uint8_t minHeight = 255;
    std::uint8_t maxHeight = 0;

    for (std::size_t y = 0; y < heightImage->height; ++y)
    {
        const std::uint8_t* row = heightImage->pixels + y * heightImage->rowPitch;

        for (std::size_t x = 0; x < heightImage->width; ++x)
        {
            const std::uint8_t value = row[x];

            minHeight = (std::min)(minHeight, value);

            maxHeight = (std::max)(maxHeight, value);
        }
    }

    Log::Info("Displacement heightmap: " + std::to_string(heightImage->width) + "x" +
              std::to_string(heightImage->height) + ", min=" + std::to_string(minHeight) +
              ", max=" + std::to_string(maxHeight));

    m_displacementHeightWidth = heightImage->width;
    m_displacementHeightHeight = heightImage->height;
    m_displacementHeightRowPitch = heightImage->rowPitch;

    const std::size_t heightDataSize = m_displacementHeightRowPitch * m_displacementHeightHeight;

    m_displacementHeightPixels.assign(heightImage->pixels, heightImage->pixels + heightDataSize);

    // Retain a GPU-readable R8 heightmap for compute displacement generation.

    if (m_displacementHeightWidth > UINT_MAX || m_displacementHeightHeight > UINT_MAX)
    {
        Log::Error("Displacement heightmap dimensions exceed texture limits.");
        return false;
    }

    const unsigned int heightWidth = static_cast<unsigned int>(m_displacementHeightWidth);

    const unsigned int heightHeight = static_cast<unsigned int>(m_displacementHeightHeight);

    if (!m_displacementHeightTexture.Initialize(device, heightWidth, heightHeight, 1, 1, DXGI_FORMAT_R8_UNORM,
                                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))
    {
        Log::Error("Failed to initialize GPU displacement heightmap.");
        return false;
    }

    const TextureSubresourceData heightSubresource{m_displacementHeightPixels.data(), m_displacementHeightRowPitch,
                                                   heightDataSize};

    if (!uploader.UploadTexture(m_displacementHeightTexture.GetResource(), &heightSubresource, 1))
    {
        Log::Error("Failed to upload GPU displacement heightmap.");
        return false;
    }

    if (!m_displacementHeightTexture.CreateSRV(device, descriptorHeap, DXGI_FORMAT_R8_UNORM))
    {
        Log::Error("Failed to create GPU displacement heightmap SRV.");
        return false;
    }

    const DisplacementHeightmap heightmap{m_displacementHeightPixels.data(), m_displacementHeightWidth,
                                          m_displacementHeightHeight, m_displacementHeightRowPitch};

    m_seedDisplacementSettings = DisplacementMeshSettings{};

    m_seedPatchLODs.fill(0u);
    // Match the generator's patchX * patchesPerAxis + patchZ ordering.
    m_seedPatchLODs[3 * 8 + 3] = 1u;
    m_seedPatchLODs[4 * 8 + 3] = 1u;
    m_seedPatchLODs[3 * 8 + 4] = 1u;
    const DisplacementMeshSettings displacementSettings = GetSeedDisplacementSettings();
    DisplacementGeometry displacement = DisplacementMeshGenerator::Generate(heightmap, displacementSettings);

    auto& gridVertices = displacement.vertices;
    auto& gridIndices = displacement.indices;

    Log::Info("Displacement geometry: vertices=" + std::to_string(gridVertices.size()) + ", indices=" +
              std::to_string(gridIndices.size()) + ", triangles=" + std::to_string(gridIndices.size() / 3) +
              ", patches=" + std::to_string(displacement.patches.size()));

    if (gridVertices.empty() ||
        gridIndices.empty() ||
        gridIndices.size() % 3 != 0 ||
        displacement.patches.size() != displacementSettings.patchesPerAxis * displacementSettings.patchesPerAxis)
    {
        Log::Error("Unexpected displacement prototype geometry counts.");
        return false;
    }

    if (std::any_of(gridIndices.begin(), gridIndices.end(), [&](std::uint32_t index) { return index >= gridVertices.size(); }))
    {
        Log::Error("Unexpected displacement prototype geometry counts.");
        return false;
    }

    TangentGenerator::GenerateTangents(gridVertices, gridIndices);

    // Full-resolution capacity remains independent of the seed's stitched LODs.
    const std::uint64_t gridResolution =
        std::uint64_t{displacementSettings.patchesPerAxis} * displacementSettings.subdivisionsPerPatch;

    if (gridResolution == 0 || gridResolution >= UINT_MAX)
    {
        Log::Error("Invalid displacement buffer capacity.");
        return false;
    }

    const std::uint64_t side = gridResolution + 1;

    // Bound products before multiplying, including the API's UINT byte-view limits.

    if (side > (UINT_MAX / sizeof(Vertex)) / side ||
        gridResolution > (UINT_MAX / sizeof(unsigned int) / 6u) / gridResolution)
    {

        Log::Error("Displacement buffer capacity exceeds mesh view limits.");
        return false;
    }

    const size_t vertexCapacity = static_cast<size_t>(side * side);
    const size_t indexCapacity = static_cast<size_t>(6u * gridResolution * gridResolution);

    const float height0 = displacementSettings.displacementScale * -displacementSettings.displacementMidpoint;
    const float height1 = displacementSettings.displacementScale * (1.0f - displacementSettings.displacementMidpoint);

    if (!std::isfinite(height0) || !std::isfinite(height1))
    {
        Log::Error("Invalid displacement bounds.");
        return false;
    }

    MeshBounds displacementBounds{};
    displacementBounds.min = Vector3{-1.0f, (std::min)(height0, height1), -1.0f};

    displacementBounds.max = Vector3{1.0f, (std::max)(height0, height1), 1.0f};
    displacementBounds.valid = true;

    for (const Vertex& vertex : gridVertices)
    {
        if (!std::isfinite(vertex.position[0]) ||
            !std::isfinite(vertex.position[1]) ||
            !std::isfinite(vertex.position[2]) ||
            vertex.position[0] < -1.0f)
        {
            Log::Error("Displacement seed exceeds valid surface bounds.");
            return false;
        }

        if (vertex.position[0] > 1.0f ||
            vertex.position[2] < -1.0f ||
            vertex.position[2] > 1.0f)
        {
            Log::Error("Displacement seed exceeds valid surface bounds.");
            return false;
        }

        displacementBounds.min.y = (std::min)(displacementBounds.min.y, vertex.position[1]);
        displacementBounds.max.y = (std::max)(displacementBounds.max.y, vertex.position[1]);
    }

    if (!m_displacementTestMesh.InitializeGPUWritableSeeded(
            device, uploader, descriptorHeap, m_vertexLayout, gridVertices.data(), gridVertices.size(),
            gridIndices.data(), gridIndices.size(), vertexCapacity, indexCapacity, displacementBounds))
    {
        return false;
    }

    // Brick diffuse texture — BC1 with sRGB sampling.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                (WindowsWindow::GetExecutableDirectory() / "assets/Textures/Materials/stacked_brick_wall_diff_4k.dds"), m_sphereTexture, true))
    {
        return false;
    }

    // Neutral plane texture

    const std::uint8_t whitePixel[4] = {255, 255, 255, 255};

    if (!m_whiteTexture.Initialize(device, whitePixel, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4))
    {
        return false;
    }

    if (!uploader.UploadTexture(m_whiteTexture.GetResource(), m_whiteTexture.GetInitialData(),
                                m_whiteTexture.GetInitialDataSize()))
    {
        return false;
    }

    if (!m_whiteTexture.CreateSRV(device, descriptorHeap, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return false;
    }

    // Brick normal map — BC5 with linear sampling.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                (WindowsWindow::GetExecutableDirectory() / "assets/Textures/Materials/stacked_brick_wall_nor_dx_4k.dds"), m_normalTexture, false))
    {
        return false;
    }

    // Brick roughness map — BC4 with linear sampling.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                (WindowsWindow::GetExecutableDirectory() / "assets/Textures/Materials/stacked_brick_wall_rough_4k.dds"), m_roughnessTexture, false))
    {
        return false;
    }

    // Brick ARM map — BC1 with linear sampling.

    if (!DDSLoader::LoadTexture(device, uploader, descriptorHeap,
                                (WindowsWindow::GetExecutableDirectory() / "assets/Textures/Materials/stacked_brick_wall_arm_4k.dds"), m_armTexture, false))
    {
        return false;
    }

    // Mirror A material

    m_mirrorAMaterial.pipeline = &m_pipeline;
    m_mirrorAMaterial.texture = &m_whiteTexture;

    m_mirrorAMaterial.model = MaterialModel::MetallicRoughness;

    m_mirrorAMaterial.roughness = 0.0f;
    m_mirrorAMaterial.metallic = 1.0f;

    m_mirrorAMaterial.baseColor[0] = 1.0f;
    m_mirrorAMaterial.baseColor[1] = 1.0f;
    m_mirrorAMaterial.baseColor[2] = 1.0f;

    // Mirror B material

    m_mirrorBMaterial.pipeline = &m_pipeline;
    m_mirrorBMaterial.texture = &m_whiteTexture;

    m_mirrorBMaterial.model = MaterialModel::MetallicRoughness;

    m_mirrorBMaterial.roughness = 0.10f;
    m_mirrorBMaterial.metallic = 1.0f;

    m_mirrorBMaterial.baseColor[0] = 1.0f;
    m_mirrorBMaterial.baseColor[1] = 1.0f;
    m_mirrorBMaterial.baseColor[2] = 1.0f;

    // Red sphere material

    m_coloredMaterial.pipeline = &m_pipeline;
    m_coloredMaterial.texture = &m_sphereTexture;
    m_coloredMaterial.normalTexture = &m_normalTexture;
    m_coloredMaterial.roughnessTexture = &m_roughnessTexture;

    m_coloredMaterial.armTexture = &m_armTexture;
    m_coloredMaterial.preferARM = true;

    m_coloredMaterial.model = MaterialModel::MetallicRoughness;

    m_coloredMaterial.roughness = 1.0f;
    m_coloredMaterial.metallic = 0.0f;

    m_coloredMaterial.baseColor[0] = 1.0f;
    m_coloredMaterial.baseColor[1] = 0.02f;
    m_coloredMaterial.baseColor[2] = 0.02f;

    m_displacementMaterial = m_coloredMaterial;
    m_displacementMaterial.normalTexture = &m_normalTexture;
    m_displacementMaterial.armTexture = nullptr;
    m_displacementMaterial.preferARM = false;

    // Floor material

    m_floorMaterial.pipeline = &m_pipeline;
    m_floorMaterial.texture = &m_whiteTexture;

    m_floorMaterial.model = MaterialModel::Dielectric;

    m_floorMaterial.roughness = 0.7f;
    m_floorMaterial.ior = 1.5f;

    m_floorMaterial.baseColor[0] = 0.25f;
    m_floorMaterial.baseColor[1] = 0.25f;
    m_floorMaterial.baseColor[2] = 0.25f;

    // Mirror A

    RenderObject mirrorA{};
    mirrorA.mesh = &m_planeMesh;
    mirrorA.material = &m_mirrorAMaterial;

    mirrorA.transform.position = Vector3{-1.5f, 1.0f, 2.0f};

    mirrorA.transform.rotation = Vector3{ToRadians(-90.0f), ToRadians(-42.7f), 0.0f};

    mirrorA.transform.scale = Vector3{2.0f, 1.0f, 2.5f};

    m_scene.AddRenderObject(mirrorA);

    // Mirror B

    RenderObject mirrorB{};
    mirrorB.mesh = &m_planeMesh;
    mirrorB.material = &m_mirrorBMaterial;

    mirrorB.transform.position = Vector3{2.5f, 1.0f, 3.0f};

    mirrorB.transform.rotation = Vector3{ToRadians(-90.0f), ToRadians(115.0f), 0.0f};

    mirrorB.transform.scale = Vector3{1.5f, 1.0f, 2.0f};

    m_scene.AddRenderObject(mirrorB);

    // Red terminal object

    RenderObject coloredSphere{};
    coloredSphere.mesh = &m_sphereMesh;
    coloredSphere.material = &m_coloredMaterial;

    coloredSphere.transform.position = Vector3{1.2f, 1.0f, 2.8f};

    coloredSphere.transform.scale = Vector3{0.75f, 0.75f, 0.75f};

    m_scene.AddRenderObject(coloredSphere);

    // Floor

    RenderObject floor{};
    floor.mesh = &m_planeMesh;
    floor.material = &m_floorMaterial;

    floor.transform.position = Vector3{0.0f, -1.0f, 2.0f};

    floor.transform.scale = Vector3{7.0f, 1.0f, 7.0f};

    m_scene.AddRenderObject(floor);

    // Temporary geometry-displacement test surface.
    RenderObject displacementTest{};

    displacementTest.mesh = &m_displacementTestMesh;
    displacementTest.material = &m_displacementMaterial;

    displacementTest.transform.position = Vector3{0.0f, -0.8f, 0.0f};

    displacementTest.transform.scale = Vector3{1.0f, 1.0f, 1.0f};

    m_scene.AddRenderObject(displacementTest);

    ResetCamera();
    ResetDirectionalLight();

    m_heightmapDescriptorHeap = &descriptorHeap;
    m_initialized = true;
    return true;
}

void MirrorTestScene::Shutdown()
{
    if (m_heightmapRegistrationOwner)
    {
        Log::Error("Mirror Test shutdown rejected: displacement heightmap is still registered.");
        std::abort();
    }

    m_initialized = false;
    m_heightmapDescriptorHeap = nullptr;
    m_cameraPatchLODsInitialized = false;
    m_requestedDisplacementRevision = 0;

    m_displacementGeometryUpdatePending = false;
    ClearEditorGeometry();
    m_scene.ClearRenderObjects();

    m_sphereTexture.Shutdown();
    m_normalTexture.Shutdown();
    m_roughnessTexture.Shutdown();

    m_armTexture.Shutdown();
    m_whiteTexture.Shutdown();
    m_displacementHeightTexture.Shutdown();

    m_sphereMesh.Shutdown();
    m_planeMesh.Shutdown();
    m_displacementTestMesh.Shutdown();

    m_displacementHeightPixels.clear();
    m_displacementHeightWidth = 0;
    m_displacementHeightHeight = 0;
    m_displacementHeightRowPitch = 0;

    m_pipeline.Shutdown();
}

void MirrorTestScene::Update(float deltaTime)
{
    (void)deltaTime;

    if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
    {
        m_cameraPhaseLoggingMilliseconds.reset();
    }

    const Vector3& cameraPosition = m_scene.GetCamera().GetPosition();

    constexpr float lodDistance = 3.0f;
    constexpr float lodDistanceSquared = lodDistance * lodDistance;

    constexpr float surfaceMin = -1.0f;
    constexpr float surfaceSize = 2.0f;
    constexpr float surfaceY = -0.8f;

    constexpr float patchSize = surfaceSize / static_cast<float>(kDisplacementPatchesPerAxis);

    for (std::uint32_t patchX = 0; patchX < kDisplacementPatchesPerAxis; ++patchX)
    {
        for (std::uint32_t patchZ = 0; patchZ < kDisplacementPatchesPerAxis; ++patchZ)
        {
            const float centerX = surfaceMin + (static_cast<float>(patchX) + 0.5f) * patchSize;

            const float centerZ = surfaceMin + (static_cast<float>(patchZ) + 0.5f) * patchSize;

            const float dx = cameraPosition.x - centerX;
            const float dy = cameraPosition.y - surfaceY;
            const float dz = cameraPosition.z - centerZ;

            const float distanceSquared = dx * dx + dy * dy + dz * dz;

            const std::uint32_t lod = distanceSquared < lodDistanceSquared ? 0u : 1u;

            const std::size_t patchIndex = patchX * kDisplacementPatchesPerAxis + patchZ;

            m_cameraPatchLODs[patchIndex] = lod;
        }
    }

    const bool lodMapChanged = !m_cameraPatchLODsInitialized || m_cameraPatchLODs != m_scene.GetDisplacementPatchLODs();

    m_cameraPatchLODsInitialized = true;

    if (lodMapChanged)
    {
        m_scene.SetDisplacementPatchLODs(m_cameraPatchLODs);
        m_requestedDisplacementRevision = AllocateDisplacementRevision();
        m_displacementGeometryUpdatePending = true;

        if constexpr (Renderer::kSuppressDisplacementLifecycleLoggingForDiagnostics)
        {
            if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
            {
                m_cameraPhaseLoggingMilliseconds = 0.0;
            }
        }
        else
        {
            CpuTiming::Clock::time_point loggingStart{};

            if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
            {
                loggingStart = CpuTiming::Clock::now();
            }
            Log::Info("Displacement camera LOD map changed.");
            if constexpr (Renderer::IsFrameHitchDiagnosticsEnabled())
            {
                m_cameraPhaseLoggingMilliseconds = CpuTiming::MillisecondsSince(loggingStart);
            }
        }
    }
}

bool MirrorTestScene::HasPendingDisplacementGeometryUpdate() const
{
    return m_displacementGeometryUpdatePending;
}

Mesh& MirrorTestScene::GetDisplacementTestMesh()
{
    return m_displacementTestMesh;
}

DisplacementMeshSettings MirrorTestScene::GetSeedDisplacementSettings() const
{
    DisplacementMeshSettings settings = m_seedDisplacementSettings;
    settings.patchLODs = m_seedPatchLODs;
    return settings;
}

bool MirrorTestScene::HasCurrentDisplacementSettings() const
{
    return m_cameraPatchLODsInitialized;
}

DisplacementMeshSettings MirrorTestScene::GetCurrentDisplacementSettings() const
{
    DisplacementMeshSettings settings = m_seedDisplacementSettings;

    settings.patchLODs = m_cameraPatchLODs;
    return settings;
}

const Texture& MirrorTestScene::GetDisplacementHeightTexture() const
{
    return m_displacementHeightTexture;
}

std::uint64_t MirrorTestScene::GetRequestedDisplacementRevision() const
{
    return m_requestedDisplacementRevision;
}

void MirrorTestScene::MarkDisplacementGeometryUpdated(std::uint64_t revision)
{
    if (m_cameraPatchLODsInitialized && revision != 0 && revision == m_requestedDisplacementRevision)
    {
        m_displacementGeometryUpdatePending = false;
    }
}

void MirrorTestScene::SetAspectRatio(float aspectRatio)
{
    m_scene.GetCamera().SetAspectRatio(aspectRatio);
}

Scene& MirrorTestScene::GetScene()
{
    return m_scene;
}

const Scene& MirrorTestScene::GetScene() const
{
    return m_scene;
}

std::vector<Material*> MirrorTestScene::GetMaterials()
{
    return {&m_mirrorAMaterial, &m_mirrorBMaterial, &m_coloredMaterial, &m_displacementMaterial, &m_floorMaterial};
}

void MirrorTestScene::ResetCamera()
{
    Camera& camera = m_scene.GetCamera();

    camera.SetPosition(Vector3{0.0f, 1.0f, -7.0f});

    camera.SetTarget(Vector3{0.0f, 0.75f, 1.5f});
}

void MirrorTestScene::ResetDirectionalLight()
{
    DirectionalLight& light = m_scene.GetDirectionalLight();

    light.direction = Vector3{-0.3f, -1.0f, 0.2f}.Normalized();

    light.intensity = 3.0f;
    light.ambientStrength = 0.0f;
    light.angularRadius = ToRadians(5.0f);
}
