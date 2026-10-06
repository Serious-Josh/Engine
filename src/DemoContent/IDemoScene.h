#pragma once

#include <vector>

struct Material;

class DescriptorHeap;
class ID3D12Device;
class ResourceUploader;
class Scene;
class Shader;

class IDemoScene
{
    public:
        virtual ~IDemoScene() = default;

        virtual bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Shader* vertexShader, const Shader* pixelShader) = 0;

        virtual void Shutdown() = 0;
        virtual void Update(float deltaTime) = 0;

        virtual Scene& GetScene() = 0;
        virtual const Scene& GetScene() const = 0;

        virtual std::vector<Material*> GetMaterials() = 0;

        virtual void ResetCamera() = 0;
        virtual void ResetDirectionalLight() = 0;

        virtual void SetAspectRatio(float aspectRatio) = 0;
};