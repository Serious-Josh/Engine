#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"

#include <d3d12.h>

class Mesh;
class Scene;

class RayTracingSceneResources
{
    public:
        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Mesh& mesh, const Scene& scene);
        void Shutdown();

        const TopLevelAccelerationStructure& GetTopLevel() const;

    private:
        BottomLevelAccelerationStructure m_bottomLevel;
        TopLevelAccelerationStructure m_topLevel;
};