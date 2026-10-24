#pragma once

#include "Core/Log.h"
#include "Math/Matrix4.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/Texture.h"
#include "Renderer/UploadBuffer.h"

#include <d3d12.h>

// UPSCALING.3B: prepares DLSS-specific inputs from Engine's render-extent G-buffer (DlssInputPreparation.hlsl): the
// DLSS motion-vector texture (RG16F, sky motion included) and the 1x1 exposure texture (R16F). Engine's own motion
// vectors are only read. Created and recorded only while RendererOptions::dlssInputPreparation is on; header-only so
// that it needs no build-system change.
class DlssInputPreparationPass
{
public:
    struct Matrices
    {
        // Render (jittered) projection, camera-relative: reconstructs each pixel's sample direction.
        Matrix4 inverseCameraRelativeRenderViewProjection;
        // Unjittered current and previous view-projections (the motion-vector convention).
        Matrix4 viewProjection;
        Matrix4 previousViewProjection;
    };

    // Creates the pipeline once and the textures for this render extent (recreated when it changes).
    bool Ensure(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& heap, unsigned int width,
        unsigned int height)
    {
        if(!m_pipelineReady)
        {
            ShaderDescription shaderDescription{};
            shaderDescription.path = "DlssInputPreparation.hlsl";
            shaderDescription.entryPoint = "CSMain";
            shaderDescription.stage = ShaderStage::Compute;
            Shader* computeShader = shaderLibrary.Load(shaderDescription);

            if(!computeShader)
            {
                Log::Error("Failed to load the DLSS input preparation shader.");
                return false;
            }

            ComputePipelineDescription pipelineDescription{};
            pipelineDescription.computeShader = computeShader;
            RootParameterDescription constants{};
            constants.type = RootParameterType::ConstantBuffer;
            constants.shaderRegister = 0;
            constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
            pipelineDescription.rootParameters.push_back(constants);

            if(!m_pipeline.Initialize(device, pipelineDescription))
            {
                Log::Error("Failed to create the DLSS input preparation pipeline.");
                return false;
            }

            m_pipelineReady = true;
        }

        if(m_motionVectors.GetResource() && m_width == width && m_height == height)
        {
            return true;
        }

        ReleaseTextures();
        constexpr D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr D3D12_RESOURCE_STATES shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const bool created =
            m_motionVectors.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16_FLOAT, uav, shaderRead) &&
            m_motionVectors.CreateSRV(device, heap) && m_motionVectors.CreateUAV(device, heap) &&
            m_exposure.Initialize(device, 1, 1, 1, 1, DXGI_FORMAT_R16_FLOAT, uav, shaderRead) &&
            m_exposure.CreateSRV(device, heap) && m_exposure.CreateUAV(device, heap);

        if(!created)
        {
            Log::Error("Failed to create the DLSS input preparation textures.");
            ReleaseTextures();
            return false;
        }

        m_motionVectors.GetResource()->SetName(L"DLSS Motion Vectors");
        m_exposure.GetResource()->SetName(L"DLSS Exposure");
        m_width = width;
        m_height = height;
        return true;
    }

    // Leaves both outputs in NON_PIXEL_SHADER_RESOURCE, the state DLSS evaluation requires (Programming Guide 3.4).
    bool Record(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, RenderTarget& motion,
        DepthBuffer& depth, const Matrices& matrices, float exposure)
    {
        struct Constants
        {
            Matrix4 inverseCameraRelativeRenderViewProjection;
            Matrix4 viewProjection;
            Matrix4 previousViewProjection;
            unsigned int motionIndex;
            unsigned int depthIndex;
            unsigned int outputMotionIndex;
            unsigned int exposureIndex;
            unsigned int width;
            unsigned int height;
            float exposure;
            unsigned int padding;
        };

        Constants constants{};
        constants.inverseCameraRelativeRenderViewProjection = matrices.inverseCameraRelativeRenderViewProjection;
        constants.viewProjection = matrices.viewProjection;
        constants.previousViewProjection = matrices.previousViewProjection;
        constants.motionIndex = motion.GetSRVIndex();
        constants.depthIndex = depth.GetSRVIndex();
        constants.outputMotionIndex = m_motionVectors.GetUAVIndex();
        constants.exposureIndex = m_exposure.GetUAVIndex();
        constants.width = m_width;
        constants.height = m_height;
        constants.exposure = exposure;

        const D3D12_GPU_VIRTUAL_ADDRESS address =
            uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            Log::Error("Failed to allocate DLSS input preparation constants.");
            return false;
        }

        constexpr D3D12_RESOURCE_STATES shaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        motion.Transition(commandList, shaderRead);
        depth.Transition(commandList, shaderRead);
        m_motionVectors.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_exposure.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_pipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);

        m_motionVectors.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_exposure.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

    Texture& GetMotionVectors() noexcept { return m_motionVectors; }
    Texture& GetExposure() noexcept { return m_exposure; }

    std::uint64_t GetResourceBytes() const noexcept
    {
        return m_motionVectors.GetResource() ? static_cast<std::uint64_t>(m_width) * m_height * 4u + 2u : 0u;
    }

    void ReleaseTextures()
    {
        m_motionVectors.Shutdown();
        m_exposure.Shutdown();
        m_width = 0;
        m_height = 0;
    }

    void Shutdown()
    {
        ReleaseTextures();
        m_pipeline.Shutdown();
        m_pipelineReady = false;
    }

private:
    ComputePipeline m_pipeline;
    bool m_pipelineReady = false;
    Texture m_motionVectors;
    Texture m_exposure;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
};
