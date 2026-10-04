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
        bool Update(ID3D12GraphicsCommandList4* commandList, unsigned int frameIndex, const Scene& scene);
        void Shutdown();

        const TopLevelAccelerationStructure& GetTopLevel() const;

    private:
        static constexpr std::size_t InvalidInstanceIndex = static_cast<std::size_t>(-1);

        std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> m_bottomLevels;
        std::unordered_map<const Mesh*, std::size_t> m_meshToBottomLevel;

        std::vector<RayTracingInstanceDescription> m_instances;
        std::vector<std::size_t> m_renderObjectToInstance;
        std::vector<const BottomLevelAccelerationStructure*> m_bottomLevelPointers;

        TopLevelAccelerationStructure m_topLevel;
};