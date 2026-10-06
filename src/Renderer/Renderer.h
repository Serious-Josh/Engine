#pragma once

#include "GBuffer.h"
#include "UploadBuffer.h"
#include "StructuredBuffer.h"
#include "Renderer/CommandContext.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderObject.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/SwapChain.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Debug/DebugUI.h"
#include "Renderer/EnvironmentMap.h"
#include "RenderTarget.h"
#include "Math/Vector3.h"
#include "Math/Matrix4.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "Renderer/Passes/DeferredLightingPass.h"
#include "Renderer/Passes/BRDFIntegrationPass.h"
#include "Renderer/Passes/GGXAverageAlbedoPass.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"
#include "Renderer/Passes/RayTracedShadowPass.h"
#include "Passes/ShadowDenoisePass.h"
#include "Renderer/Passes/RayTracedReflectionPass.h"
#include "Passes/RayTracedReflectionTemporalPass.h"
#include "Passes/RayTracedReflectionDenoisePass.h"
#include "Passes/ReflectionContributionLimitPass.h"
#include "Passes/NrdReflectionDenoiser.h"
#include "Passes/NrdReflectionPreparePass.h"
#include "Passes/NrdReflectionResolvePass.h"

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <vector>

struct EnvironmentAsset
{
    std::string displayName;
    std::filesystem::path relativePath;
};

class Scene;
struct Material;

class Renderer
{
    public:
        bool Initialize(void* windowHandle, unsigned int width, unsigned int height, const std::filesystem::path& assetRoot);
        void Render(const Scene& scene);
        void Shutdown();

        void Resize(unsigned int width, unsigned int height);

        ID3D12Device* GetDevice();
        ResourceUploader& GetResourceUploader();
        DescriptorHeap& GetResourceDescriptorHeap();
        ShaderLibrary& GetShaderLibrary();
        DebugUI& GetDebugUI();

        bool InitializeSceneResources(const Scene& scene, const std::vector<Material*>& materials);

        void ReleaseSceneResources();
        void ResetSceneHistory();

        void WaitForGPU();

        const std::vector<EnvironmentAsset>& GetAvailableEnvironments() const
        {
            return m_availableEnvironments;
        }

        size_t GetCurrentEnvironmentIndex() const
        {
            return m_currentEnvironmentIndex;
        }

        bool SetEnvironment(size_t index);

    private:
        bool InitializeDevice();
        bool InitializeSwapChain(void* windowHandle);
        void Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object, D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
        void BeginGeometryPass(ID3D12GraphicsCommandList* commandList);
        void EndGeometryPass(ID3D12GraphicsCommandList* commandList);
        bool InitializeMaterials(const std::vector<Material*>& materials);
        bool InitializeRayTracingScene(const Scene& scene);
        bool DiscoverEnvironments();
        bool PreprocessEnvironment(EnvironmentMap& environment);

        GraphicsDevice m_graphicsDevice;
        SwapChain m_swapChain;
        CommandContext m_commandContext;

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
        ReflectionContributionLimitPass m_reflectionContributionLimitPass;
        bool m_previousContributionLimiterEnabled = false;
        bool m_nrdExecutedLastFrame = false;
        bool m_nativeReflectionExecutedLastFrame = false;
        NrdReflectionDenoiser m_nrdReflectionDenoiser;
        NrdReflectionPreparePass m_nrdReflectionPreparePass;
        NrdReflectionResolvePass m_nrdReflectionResolvePass;
        ShadowDenoisePass m_shadowDenoisePass;

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

        Texture m_brdfIntegrationLUT;
        Texture m_ggxAverageAlbedoLUT;

        RayTracingSceneResources m_rayTracingScene;

        Matrix4 m_previousViewProjection{};
        bool m_hasPreviousViewProjection = false;
        Matrix4 m_previousView{};
        Matrix4 m_previousProjection{};
        bool m_hasPreviousNrdCamera = false;

        Vector3 m_previousShadowLightDirection{};
        bool m_hasPreviousShadowLightDirection = false;

        std::filesystem::path m_assetRoot;
        std::vector<EnvironmentAsset> m_availableEnvironments;
        size_t m_currentEnvironmentIndex = 0;
};