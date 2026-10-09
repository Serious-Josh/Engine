#pragma once

#include "GBuffer.h"
#include "UploadBuffer.h"
#include "StructuredBuffer.h"
#include "CommandContext.h"
#include "DescriptorHeap.h"
#include "GraphicsDevice.h"
#include "GpuTimestampProfiler.h"
#include "RenderObject.h"
#include "RendererStatistics.h"
#include "ResourceUploader.h"
#include "SwapChain.h"
#include "EnvironmentMap.h"
#include "RenderTarget.h"

#include "Debug/DebugUI.h"

#include "Math/Vector3.h"
#include "Math/Matrix4.h"

#include "Shader/ShaderLibrary.h"

#include "RayTracing/RayTracingSceneResources.h"

#include "Passes/ToneMapPass.h"
#include "Passes/DeferredLightingPass.h"
#include "Passes/BRDFIntegrationPass.h"
#include "Passes/GGXAverageAlbedoPass.h"
#include "Passes/RayTracedShadowPass.h"
#include "Passes/RayTracedReflectionPass.h"
#include "Passes/ShadowDenoisePass.h"
#include "Passes/RayTracedReflectionTemporalPass.h"
#include "Passes/RayTracedReflectionDenoisePass.h"
#include "Passes/ReflectionContributionLimitPass.h"
#include "Passes/NrdReflectionDenoiser.h"
#include "Passes/NrdReflectionPreparePass.h"
#include "Passes/NrdReflectionResolvePass.h"
#include "Passes/SelectionMaskPass.h"
#include "Passes/RayTracedGIPass.h"

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>
#include <limits>

struct EnvironmentAsset
{
    std::string displayName;
    std::filesystem::path relativePath;
};

struct RendererOptions
{
    size_t selectedObject = (std::numeric_limits<size_t>::max)();

    Vector3 selectionOutlineColor{1.0f, 0.72f, 0.0f};
    unsigned int selectionOutlineThickness = 2;

    bool enableRTGI = true;
};

class Scene;
struct Material;

class Renderer
{
    public:
        bool Initialize(void* windowHandle, unsigned int width, unsigned int height, const std::filesystem::path& assetRoot);
        bool Render(const Scene& scene, const RendererOptions& options);
        void Shutdown();

        bool Resize(unsigned int width, unsigned int height);

        ID3D12Device* GetDevice();
        ResourceUploader& GetResourceUploader();
        DescriptorHeap& GetResourceDescriptorHeap();
        ShaderLibrary& GetShaderLibrary();
        DebugUI& GetDebugUI();

        bool InitializeSceneResources(const Scene& scene, const std::vector<Material*>& materials);

        void ReleaseSceneResources();
        void ResetSceneHistory();

        bool WaitForGPU();

        const std::vector<EnvironmentAsset>& GetAvailableEnvironments() const
        {
            return m_availableEnvironments;
        }

        size_t GetCurrentEnvironmentIndex() const
        {
            return m_currentEnvironmentIndex;
        }

        bool SetEnvironment(size_t index);

        const RendererStatistics& GetStatistics() const;

    private:
        bool InitializeDevice();
        bool InitializeSwapChain(void* windowHandle);
        void Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
        void DrawSelectionMask(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
        void BeginGeometryPass(ID3D12GraphicsCommandList* commandList);
        void EndGeometryPass(ID3D12GraphicsCommandList* commandList);
        bool InitializeMaterials(const std::vector<Material*>& materials);
        bool InitializeRayTracingScene(const Scene& scene);
        bool DiscoverEnvironments();
        bool PreprocessEnvironment(EnvironmentMap& environment);

        GraphicsDevice m_graphicsDevice;
        SwapChain m_swapChain;
        CommandContext m_commandContext;
        GpuTimestampProfiler m_gpuTimestampProfiler;
        uint64_t m_gpuProfilerFrameSerial = 0;

        ResourceUploader m_resourceUploader;
        DescriptorHeap m_resourceDescriptorHeap;
        DescriptorHeap m_rtvDescriptorHeap;
        DescriptorHeap m_dsvDescriptorHeap;

        DeferredLightingPass m_deferredLightingPass;
        ToneMapPass m_toneMapPass;
        BRDFIntegrationPass m_brdfIntegrationPass;
        GGXAverageAlbedoPass m_ggxAverageAlbedoPass;
        RayTracedShadowPass m_rayTracedShadowPass;
        RayTracedReflectionPass m_rayTracedReflectionPass;
        RayTracedReflectionTemporalPass m_rayTracedReflectionTemporalPass;
        RayTracedReflectionDenoisePass m_rayTracedReflectionDenoisePass;
        RayTracedGIPass m_rayTracedGIPass;
        ReflectionContributionLimitPass m_reflectionContributionLimitPass;
        NrdReflectionDenoiser m_nrdReflectionDenoiser;
        NrdReflectionPreparePass m_nrdReflectionPreparePass;
        NrdReflectionResolvePass m_nrdReflectionResolvePass;
        ShadowDenoisePass m_shadowDenoisePass;
        SelectionMaskPass m_selectionMaskPass;

        bool m_previousContributionLimiterEnabled = false;
        bool m_nrdExecutedLastFrame = false;
        bool m_nativeReflectionExecutedLastFrame = false;

        ShaderLibrary m_shaderLibrary;

        DebugUI m_debugUI;

        unsigned int m_width = 0;
        unsigned int m_height = 0;
        unsigned int m_shadowSampleIndex = 0;
        unsigned int m_reflectionSampleIndex = 0;
        uint32_t m_nrdFrameIndex = 0;

        GBuffer m_gBuffer;
        UploadBuffer m_uploadBuffer;
        StructuredBuffer m_materialBuffer;

        EnvironmentMap m_environment;
        RenderTarget m_sceneColor;
        RenderTarget m_selectionMask;

        Texture m_brdfIntegrationLUT;
        Texture m_ggxAverageAlbedoLUT;

        RayTracingSceneResources m_rayTracingScene;

        Matrix4 m_previousViewProjection{};
        Matrix4 m_previousView{};
        bool m_hasPreviousViewProjection = false;

        Matrix4 m_previousNrdView{};
        Matrix4 m_previousNrdProjection{};
        bool m_hasPreviousNrdCamera = false;

        Vector3 m_previousShadowLightDirection{};
        bool m_hasPreviousShadowLightDirection = false;

        std::filesystem::path m_assetRoot;
        std::vector<EnvironmentAsset> m_availableEnvironments;
        size_t m_currentEnvironmentIndex = 0;

        RendererStatistics m_statistics{};
        RendererStatistics m_currentFrameStatistics{};
        struct PendingRendererStatistics
        {
            RendererStatistics statistics{};
            bool valid = false;
        };

        PendingRendererStatistics m_pendingStatistics[GpuTimestampProfiler::FrameCount]{};
};