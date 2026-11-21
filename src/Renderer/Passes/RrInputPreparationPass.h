#pragma once

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/Texture.h"
#include "Renderer/UploadBuffer.h"

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

// RR.1B: prepares the inputs of DLSS Ray Reconstruction from this frame's path-tracing extraction and the G-buffer
// (RrInputPreparation.hlsl has the contracts): the per-frame noisy colour, linear diffuse and specular albedo, the
// world-space normal with roughness, and reflection-aware specular motion vectors, all at the render extent. Depth and
// the ordinary motion vectors stay DlssInputPreparationPass's. Owns its targets (tracked Textures, so every barrier is
// R.1-transactional); created on first use at the render extent and released on resize or when the preparation stops.
// The colour goes to its own target, never to the scene colour: RR.1C's evaluation binds it directly as RR's colour input,
// and outside RR frames nothing consumes the outputs. Header-only, as DlssInputPreparationPass.
class RrInputPreparationPass
{
public:
    // Perceptual roughness below which a reflection is specular enough for virtual-image motion: RTXPT's
    // PathTracer::kSpecularRoughnessThreshold (0.25), the bound it uses for the same specular motion vectors. Rougher lobes
    // blur the reflection over a cone whose image has no single motion; they keep the surface motion.
    static constexpr float SpecularMotionRoughnessThreshold = 0.25f;

    // Bytes per render pixel: colour 8, two albedos 4 + 4, normal and roughness 8, specular motion 4; the diagnostic
    // targets add 8 (throughput) and 16 (RR.1C: a two-mirror chain's second mirror).
    static constexpr std::uint64_t BytesPerPixel = 8 + 4 + 4 + 8 + 4;
    static constexpr std::uint64_t DiagnosticBytesPerPixel = 8 + 16;

    struct Inputs
    {
        Matrix4 inverseViewProjection;
        Matrix4 viewProjection;
        Matrix4 previousViewProjection;
        Vector3 cameraPosition;
        DepthBuffer* depth = nullptr;
        RenderTarget* material = nullptr;
        RenderTarget* surface = nullptr;
        RenderTarget* geometricNormal = nullptr;
        RenderTarget* gBufferMotion = nullptr;
        Texture* diffuseExtraction = nullptr;
        Texture* specularExtraction = nullptr;
        Texture* presentation = nullptr;
        // DlssInputPreparationPass's motion vectors (RG16F, DLSS convention), left NON_PIXEL_SHADER_RESOURCE.
        Texture* motionVectors = nullptr;
        std::uint32_t materialBufferIndex = 0;
        // PSR.2 bindings of this frame (all or none): PsrDenoiseConstants, the replacement pass's transforms and (RR.1C)
        // the TLAS a two-mirror chain's second mirror is retraced against.
        D3D12_GPU_VIRTUAL_ADDRESS psrConstants = 0;
        D3D12_GPU_VIRTUAL_ADDRESS psrTransforms = 0;
        D3D12_GPU_VIRTUAL_ADDRESS topLevel = 0;
        // RR.1C exposure experiment: the noisy colour is multiplied by this (1 in production).
        float colorScale = 1.0f;
    };

    const std::string& FailureReason() const { return m_failure; }

    // Creates the pipeline (and, with psr, its PSR variant) once and the targets for this extent (recreated when it
    // changes). diagnosticThroughput adds the throughput and second-mirror targets.
    bool Ensure(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& heap, unsigned int width,
        unsigned int height, bool psr, bool diagnosticThroughput)
    {
        m_failure.clear();

        if(!m_pipelineReady && !CreatePipeline(device, shaderLibrary, false, m_pipeline))
        {
            return false;
        }

        m_pipelineReady = true;

        if(psr && !m_psrPipelineReady && !m_psrPipelineFailed)
        {
            m_psrPipelineReady = CreatePipeline(device, shaderLibrary, true, m_psrPipeline);
            m_psrPipelineFailed = !m_psrPipelineReady;
        }

        if(m_color.GetResource() && m_width == width && m_height == height &&
            (m_throughput.GetResource() != nullptr) == diagnosticThroughput)
        {
            return true;
        }

        ReleaseTextures();
        const MemoryCategoryScope memoryScope(MemoryCategory::DenoisersUpscaling);
        const bool created = Create(device, heap, m_color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RR Noisy Colour") &&
            Create(device, heap, m_diffuseAlbedo, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, L"RR Diffuse Albedo") &&
            Create(device, heap, m_specularAlbedo, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, L"RR Specular Albedo") &&
            Create(device, heap, m_normalRoughness, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RR Normal Roughness") &&
            Create(device, heap, m_specularMotion, width, height, DXGI_FORMAT_R16G16_FLOAT, L"RR Specular Motion") &&
            (!diagnosticThroughput ||
                (Create(device, heap, m_throughput, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RR PSR Throughput") &&
                Create(device, heap, m_secondMirror, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    L"RR PSR Second Mirror")));

        if(!created)
        {
            m_failure = "Failed to create the Ray Reconstruction input targets.";
            Log::Error(m_failure);
            ReleaseTextures();
            return false;
        }

        m_width = width;
        m_height = height;
        return true;
    }

    // Leaves every output in NON_PIXEL_SHADER_RESOURCE (the state RR evaluation reads, DLSS Programming Guide 3.4).
    bool Record(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, const Inputs& inputs)
    {
        m_failure.clear();
        m_recordedPsrVariant = false;

        if(!m_color.GetResource() || !inputs.depth || !inputs.material || !inputs.surface || !inputs.geometricNormal ||
            !inputs.gBufferMotion || !inputs.diffuseExtraction || !inputs.specularExtraction || !inputs.presentation ||
            !inputs.motionVectors)
        {
            m_failure = "Ray Reconstruction input preparation is missing an input.";
            return false;
        }

        struct Constants
        {
            Matrix4 inverseViewProjection;
            Matrix4 viewProjection;
            Matrix4 previousViewProjection;
            float cameraPosition[3];
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t depthIndex;
            std::uint32_t materialIndex;
            std::uint32_t surfaceIndex;
            std::uint32_t geometricNormalIndex;
            std::uint32_t gBufferMotionIndex;
            std::uint32_t materialBufferIndex;
            std::uint32_t diffuseExtractionIndex;
            std::uint32_t specularExtractionIndex;
            std::uint32_t presentationIndex;
            std::uint32_t motionIndex;
            std::uint32_t colorOutputIndex;
            std::uint32_t diffuseAlbedoOutputIndex;
            std::uint32_t specularAlbedoOutputIndex;
            std::uint32_t normalRoughnessOutputIndex;
            std::uint32_t specularMotionOutputIndex;
            std::uint32_t throughputOutputIndex;
            float specularMotionRoughnessThreshold;
            float colorScale;
            std::uint32_t secondMirrorOutputIndex;
        };

        static_assert(sizeof(Constants) == 3 * 64 + 24 * 4, "Must match RrInputPreparationConstants");

        Constants constants{};
        constants.inverseViewProjection = inputs.inverseViewProjection;
        constants.viewProjection = inputs.viewProjection;
        constants.previousViewProjection = inputs.previousViewProjection;
        constants.cameraPosition[0] = inputs.cameraPosition.x;
        constants.cameraPosition[1] = inputs.cameraPosition.y;
        constants.cameraPosition[2] = inputs.cameraPosition.z;
        constants.width = m_width;
        constants.height = m_height;
        constants.depthIndex = inputs.depth->GetSRVIndex();
        constants.materialIndex = inputs.material->GetSRVIndex();
        constants.surfaceIndex = inputs.surface->GetSRVIndex();
        constants.geometricNormalIndex = inputs.geometricNormal->GetSRVIndex();
        constants.gBufferMotionIndex = inputs.gBufferMotion->GetSRVIndex();
        constants.materialBufferIndex = inputs.materialBufferIndex;
        constants.diffuseExtractionIndex = inputs.diffuseExtraction->GetDescriptorIndex();
        constants.specularExtractionIndex = inputs.specularExtraction->GetDescriptorIndex();
        constants.presentationIndex = inputs.presentation->GetDescriptorIndex();
        constants.motionIndex = inputs.motionVectors->GetDescriptorIndex();
        constants.colorOutputIndex = m_color.GetUAVIndex();
        constants.diffuseAlbedoOutputIndex = m_diffuseAlbedo.GetUAVIndex();
        constants.specularAlbedoOutputIndex = m_specularAlbedo.GetUAVIndex();
        constants.normalRoughnessOutputIndex = m_normalRoughness.GetUAVIndex();
        constants.specularMotionOutputIndex = m_specularMotion.GetUAVIndex();
        constants.throughputOutputIndex = m_throughput.GetResource() ? m_throughput.GetUAVIndex() : 0xFFFFFFFFu;
        constants.specularMotionRoughnessThreshold = SpecularMotionRoughnessThreshold;
        constants.colorScale = inputs.colorScale;
        constants.secondMirrorOutputIndex = m_secondMirror.GetResource() ? m_secondMirror.GetUAVIndex() : 0xFFFFFFFFu;

        const D3D12_GPU_VIRTUAL_ADDRESS address =
            uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            m_failure = "Failed to allocate Ray Reconstruction input preparation constants.";
            Log::Error(m_failure);
            return false;
        }

        constexpr D3D12_RESOURCE_STATES shaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        inputs.depth->Transition(commandList, shaderRead);
        inputs.material->Transition(commandList, shaderRead);
        inputs.surface->Transition(commandList, shaderRead);
        inputs.geometricNormal->Transition(commandList, shaderRead);
        inputs.gBufferMotion->Transition(commandList, shaderRead);
        inputs.diffuseExtraction->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inputs.specularExtraction->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inputs.presentation->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inputs.motionVectors->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        for(Texture* output : Outputs())
        {
            output->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        const bool psr = inputs.psrConstants != 0 && inputs.psrTransforms != 0 && inputs.topLevel != 0 &&
            m_psrPipelineReady;
        (psr ? m_psrPipeline : m_pipeline).Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);

        if(psr)
        {
            commandList->SetComputeRootConstantBufferView(1, inputs.psrConstants);
            commandList->SetComputeRootShaderResourceView(2, inputs.psrTransforms);
            commandList->SetComputeRootShaderResourceView(3, inputs.topLevel);
        }

        commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);

        for(Texture* output : Outputs())
        {
            output->Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        m_recordedPsrVariant = psr;
        return true;
    }

    Texture& GetColor() noexcept { return m_color; }
    Texture& GetDiffuseAlbedo() noexcept { return m_diffuseAlbedo; }
    Texture& GetSpecularAlbedo() noexcept { return m_specularAlbedo; }
    Texture& GetNormalRoughness() noexcept { return m_normalRoughness; }
    Texture& GetSpecularMotion() noexcept { return m_specularMotion; }
    Texture& GetThroughput() noexcept { return m_throughput; }
    // RR.1C diagnostics: per replaced two-mirror pixel, rgb the second mirror's delta weight F_B and a its material index
    // plus half its |N.V| (index + cosine / 2, the cosine in (0, 1]); -1 elsewhere.
    Texture& GetSecondMirror() noexcept { return m_secondMirror; }
    bool RecordedPsrVariant() const noexcept { return m_recordedPsrVariant; }
    bool HasTextures() const noexcept { return m_color.GetResource() != nullptr; }

    std::uint64_t GetResourceBytes() const noexcept
    {
        const std::uint64_t pixels = static_cast<std::uint64_t>(m_width) * m_height;
        return HasTextures() ? pixels * (BytesPerPixel + (m_throughput.GetResource() ? DiagnosticBytesPerPixel : 0)) : 0;
    }

    // Only when no recorded work reads the targets (Texture::Shutdown defers nothing): after a GPU drain.
    void ReleaseTextures()
    {
        for(Texture* output : Outputs())
        {
            output->Shutdown();
        }

        m_throughput.Shutdown();
        m_secondMirror.Shutdown();
        m_width = 0;
        m_height = 0;
    }

    void Shutdown()
    {
        ReleaseTextures();
        m_pipeline.Shutdown();
        m_psrPipeline.Shutdown();
        m_pipelineReady = false;
        m_psrPipelineReady = false;
        m_psrPipelineFailed = false;
    }

private:
    // b0 constants; the PSR variant adds b1 (PsrDenoiseConstants), t0 (the replacement pass's transforms) and s0, exactly as
    // NRD's PSR preparation (NrdPathTracePreparePass), and (RR.1C) t1, the TLAS, and s1, the path tracer's material sampler,
    // for the second mirror of a two-mirror chain.
    bool CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, bool psr, ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = psr ? "RrInputPreparationPsr.hlsl" : "RrInputPreparation.hlsl";
        shaderDescription.entryPoint = "CSMain";
        shaderDescription.stage = ShaderStage::Compute;
        Shader* computeShader = shaderLibrary.Load(shaderDescription);

        if(!computeShader)
        {
            m_failure = std::string("Failed to load ") + shaderDescription.path + ".";
            Log::Error(m_failure);
            return false;
        }

        ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = computeShader;
        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        pipelineDescription.rootParameters.push_back(constants);

        if(psr)
        {
            constants.shaderRegister = 1;
            pipelineDescription.rootParameters.push_back(constants);
            RootParameterDescription transforms{};
            transforms.type = RootParameterType::ShaderResource;
            transforms.shaderRegister = 0;
            transforms.visibility = D3D12_SHADER_VISIBILITY_ALL;
            pipelineDescription.rootParameters.push_back(transforms);
            pipelineDescription.staticSamplers.push_back(PsrDenoiseMirrorSampler(0));
            RootParameterDescription topLevel{};
            topLevel.type = RootParameterType::ShaderResource;
            topLevel.shaderRegister = 1;
            topLevel.visibility = D3D12_SHADER_VISIBILITY_ALL;
            pipelineDescription.rootParameters.push_back(topLevel);
            // The path tracer's material sampler (RayTracingPipeline: trilinear wrap), as PsrInPlaceReplacePass's s1.
            D3D12_STATIC_SAMPLER_DESC materialSampler = PsrDenoiseMirrorSampler(1);
            materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            pipelineDescription.staticSamplers.push_back(materialSampler);
        }

        if(!pipeline.Initialize(device, pipelineDescription))
        {
            m_failure = std::string("Failed to create the ") + shaderDescription.path + " pipeline.";
            Log::Error(m_failure);
            return false;
        }

        return true;
    }

    static bool Create(ID3D12Device* device, DescriptorHeap& heap, Texture& texture, unsigned int width,
        unsigned int height, DXGI_FORMAT format, const wchar_t* name)
    {
        constexpr D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const bool created = texture.Initialize(device, width, height, 1, 1, format, uav,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && texture.CreateSRV(device, heap) &&
            texture.CreateUAV(device, heap);

        if(created)
        {
            texture.GetResource()->SetName(name);
        }

        return created;
    }

    // The outputs that exist (the throughput target only with diagnostics).
    std::vector<Texture*> Outputs()
    {
        std::vector<Texture*> outputs{&m_color, &m_diffuseAlbedo, &m_specularAlbedo, &m_normalRoughness, &m_specularMotion};

        if(m_throughput.GetResource())
        {
            outputs.push_back(&m_throughput);
            outputs.push_back(&m_secondMirror);
        }

        return outputs;
    }

    std::string m_failure;
    ComputePipeline m_pipeline;
    ComputePipeline m_psrPipeline;
    bool m_pipelineReady = false;
    bool m_psrPipelineReady = false;
    bool m_psrPipelineFailed = false;
    bool m_recordedPsrVariant = false;
    Texture m_color;
    Texture m_diffuseAlbedo;
    Texture m_specularAlbedo;
    Texture m_normalRoughness;
    Texture m_specularMotion;
    Texture m_throughput;
    Texture m_secondMirror;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
};
