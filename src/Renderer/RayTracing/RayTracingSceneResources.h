#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"

#include <d3d12.h>
#include <memory>
#include <vector>
#include <unordered_map>

class Mesh;
class Scene;

class RayTracingSceneResources
{
    public:
        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Scene& scene);
        bool Update(ID3D12GraphicsCommandList4* commandList, const Scene& scene);
        void Shutdown();

        const TopLevelAccelerationStructure& GetTopLevel() const;

    private:
        std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> m_bottomLevels;
        std::unordered_map<const Mesh*, std::size_t> m_meshToBottomLevel;
        TopLevelAccelerationStructure m_topLevel;
};