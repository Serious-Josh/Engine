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

class Shader;
class ResourceUploader;
class DescriptorHeap;

class MirrorTestScene : public IDemoScene
{
public:
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

    bool GenerateCameraDisplacementGeometry(DisplacementGeometry& geometry) const;

    bool HasPendingDisplacementGeometryUpdate() const;
    Mesh& GetDisplacementTestMesh();
    void MarkDisplacementGeometryUpdated();

private:
    VertexLayout m_vertexLayout;

    Mesh m_sphereMesh;
    Mesh m_planeMesh;
    Mesh m_displacementTestMesh;

    // CPU heightmap retained for runtime displacement regeneration.
    std::vector<std::uint8_t> m_displacementHeightPixels;
    std::size_t m_displacementHeightWidth = 0;
    std::size_t m_displacementHeightHeight = 0;
    std::size_t m_displacementHeightRowPitch = 0;

    Texture m_sphereTexture;
    Texture m_normalTexture;
    Texture m_roughnessTexture;
    Texture m_armTexture;
    Texture m_whiteTexture;

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

    std::array<std::uint32_t,
        kDisplacementPatchesPerAxis * kDisplacementPatchesPerAxis>
        m_cameraPatchLODs{};

    bool m_cameraPatchLODsInitialized = false;
    bool m_displacementGeometryUpdatePending = false;
};