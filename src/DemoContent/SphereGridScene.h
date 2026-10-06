#pragma once

#include "Math/Math.h"
#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/VertexLayout.h"
#include "Renderer/RenderObject.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Material.h"
#include "Renderer/Pipeline.h"
#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"
#include "DemoContent/IDemoScene.h"

#include <vector>
#include <array>

class Shader;

enum class DemoUpdateMode
{
    None,
    RotateLight,
    MoveSphere,
    OrbitCamera
};

class SphereGridScene : public IDemoScene
{
    public:
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader) override;
        void Shutdown() override;

        void Update(float deltaTime) override;

        void SetAspectRatio(float aspectRatio) override;

        Scene& GetScene() override;
        const Scene& GetScene() const override;
        std::vector<Material*> GetMaterials() override;

        void ResetCamera() override;
        void ResetDirectionalLight() override;

        void SetUpdateMode(DemoUpdateMode mode);
        DemoUpdateMode GetUpdateMode() const;

        void SetLightRotationSpeed(float speed);
        float GetLightRotationSpeed() const;
        void SetCameraOrbitSpeed(float speed);
        float GetCameraOrbitSpeed() const;
        void SetLightVerticalAngle(float angle);
        float GetLightVerticalAngle() const;

    private:
        void UpdateRotatingLight(float deltaTime);
        void UpdateMovingSphere(float deltaTime);
        void UpdateOrbitingCamera(float deltaTime);

        VertexLayout m_vertexLayout;
        Mesh m_mesh;
        Mesh m_planeMesh;
        Texture m_texture;
        Pipeline m_pipeline;

        static constexpr unsigned int MaterialSampleCount = 5;

        std::array<Material, MaterialSampleCount> m_dielectricMaterials;
        std::array<Material, MaterialSampleCount> m_conductorMaterials;
        std::array<Material, MaterialSampleCount> m_metallicRoughnessMaterials;
        Material m_floorMaterial;

        Scene m_scene;

        float m_lightAngle = 0.0f;
        float m_lightRotationSpeed = 1.0f;
        float m_cameraOrbitAngle = 0.0f;
        float m_cameraOrbitSpeed = 0.25f;
        float m_lightVerticalAngle = ToRadians(50);

        DemoUpdateMode m_updateMode = DemoUpdateMode::None;
};