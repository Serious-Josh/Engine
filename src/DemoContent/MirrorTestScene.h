#pragma once

#include "Math/Math.h"

#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/Material.h"
#include "Renderer/Pipeline.h"

#include "Scene/Scene.h"

#include "DemoContent/IDemoScene.h"

#include "Renderer/Geometry/DisplacementMeshGenerator.h"

#include <vector>
#include <array>
#include <cstdint>
#include <optional>

class Shader;
class ResourceUploader;
class DescriptorHeap;
class Renderer;

class MirrorTestScene : public IDemoScene
{
    public:
    MirrorTestScene() = default;
    MirrorTestScene(const MirrorTestScene&) = delete;
    MirrorTestScene& operator=(const MirrorTestScene&) = delete;
    MirrorTestScene(MirrorTestScene&&) = delete;
    MirrorTestScene& operator=(MirrorTestScene&&) = delete;
    ~MirrorTestScene() override;
    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader) override;

    void Shutdown() override;

    void Update(float deltaTime) override;

    void SetAspectRatio(float aspectRatio) override;

    Scene& GetScene() override;
    const Scene& GetScene() const override;

    std::vector<Material*> GetMaterials() override;

    void ResetCamera() override;
    void ResetDirectionalLight() override;

    bool HasPendingDisplacementGeometryUpdate() const;
    Mesh& GetDisplacementTestMesh();
    // Observation only. Resource/descriptor handles are borrowed from this owner.
    const Texture& GetDisplacementHeightTexture() const;
    // After Initialize(): returned LOD view borrows this scene's seed storage.
    // Do not retain it beyond the scene's lifetime or across reinitialization.
    DisplacementMeshSettings GetSeedDisplacementSettings() const;
    bool HasCurrentDisplacementSettings() const;
    // Requires HasCurrentDisplacementSettings(): camera selection is available
    // only after Update(). LOD view borrows member storage in
    // patchX * patchesPerAxis + patchZ
    // order and reflects subsequent Updates; do not retain past scene lifetime
    // or across reinitialization. Does not consume pending geometry updates.
    DisplacementMeshSettings GetCurrentDisplacementSettings() const;
    // Zero before the first camera selection and after Shutdown. Nonzero
    // revisions are process-unique and increase only for changed selections,
    // so acknowledgments cannot cross scene initialization lifetimes.
    std::uint64_t GetRequestedDisplacementRevision() const;
    // Logging from the most recent Update; Engine copies it into that frame's snapshot.
    std::optional<double> GetCameraPhaseLoggingMilliseconds() const
    {
        return m_cameraPhaseLoggingMilliseconds;
    }
    void MarkDisplacementGeometryUpdated(std::uint64_t revision);

    private:
    friend class Renderer; // Only Renderer receives mutable displacement heightmap access.
    Renderer* m_heightmapRegistrationOwner = nullptr;

    std::uint64_t m_heightmapRegistrationLifetime = 0;

    DescriptorHeap* m_heightmapDescriptorHeap = nullptr;

    bool m_initialized = false;

    std::optional<double> m_cameraPhaseLoggingMilliseconds;

    VertexLayout m_vertexLayout;

    Mesh m_sphereMesh;
    Mesh m_planeMesh;
    Mesh m_displacementTestMesh;

    // CPU heightmap retained for seed geometry and GPU heightmap initialization.
    std::vector<std::uint8_t> m_displacementHeightPixels;

    std::size_t m_displacementHeightWidth = 0;
    std::size_t m_displacementHeightHeight = 0;
    std::size_t m_displacementHeightRowPitch = 0;

    Texture m_sphereTexture;
    Texture m_normalTexture;
    Texture m_roughnessTexture;
    Texture m_armTexture;

    Texture m_whiteTexture;
    Texture m_displacementHeightTexture;

    Pipeline m_pipeline;

    Material m_mirrorAMaterial;
    Material m_mirrorBMaterial;
    Material m_coloredMaterial;
    Material m_floorMaterial;

    Material m_displacementMaterial;

    Scene m_scene;

    // Diagnostic camera-selected displacement LODs.
    // Does not affect the currently rendered mesh.
    static constexpr std::uint32_t kDisplacementPatchesPerAxis = 8;

    // Store scalar settings without a self-referencing span. The accessor binds
    // its returned view to the owned seed LODs, independently of camera LODs.
    DisplacementMeshSettings m_seedDisplacementSettings{};

    std::array<std::uint32_t, kDisplacementPatchesPerAxis * kDisplacementPatchesPerAxis> m_seedPatchLODs{};

    std::array<std::uint32_t, kDisplacementPatchesPerAxis * kDisplacementPatchesPerAxis> m_cameraPatchLODs{};

    bool m_cameraPatchLODsInitialized = false;

    std::uint64_t m_requestedDisplacementRevision = 0;

    bool m_displacementGeometryUpdatePending = false;
};
