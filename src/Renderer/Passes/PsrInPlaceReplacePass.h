#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Buffer.h"
#include "Renderer/GBuffer.h"
#include "Renderer/Pipeline.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <cstdint>

// PSR.2 Stage 2 in-place replacement prototype (development only; PathTraceReferenceSettings::
// primarySurfaceReplacementInPlace). One fullscreen triangle recorded after the geometry pass on Path Tracing frames:
// PsrInPlaceReplace.hlsl traces the reflected ray of each PSR-eligible mirror pixel (identity.x bit 31) with inline
// RayQuery and overwrites its G-buffer with the reflected surface in virtual camera space. The existing material,
// surface, motion and geometric-normal targets are bound as render targets and the existing depth buffer as a writable
// DSV (SV_Depth, test ALWAYS); the identity target is only read. It owns only its pipeline; constants and the
// per-object transforms are frame uploads, and the TLAS is the frame's.
//
// Header-only, like EditorGridPass.
class PsrInPlaceReplacePass
{
public:
    // PsrInPlaceReplace.hlsl's PsrInPlaceConstants.
    struct Constants
    {
        Matrix4 cameraRelativeViewProjection;
        Matrix4 inverseCameraRelativeViewProjection;
        Matrix4 unjitteredViewProjection;
        Matrix4 previousViewProjection;
        float viewDepthRow[4] = {};
        float previousViewDepthRow[4] = {};
        float cameraPosition[3] = {};
        std::uint32_t identityIndex = 0;
        std::uint32_t instanceDataIndex = 0;
        std::uint32_t transformCount = 0;
        std::uint32_t resolution[2] = {};
        std::uint32_t materialBufferIndex = 0;
        // Diagnostic pipeline only: the raw UAVs of the per-pixel hit records (128 bytes per pixel) and of PSR.1's
        // records.
        std::uint32_t diagnosticHitsIndex = 0;
        std::uint32_t psr1RecordsIndex = 0;
        // The geometry pass's texture mip bias (CameraConstants::textureMipBias), for the mirror's recovered samples.
        float textureMipBias = 0.0f;
    };
    static_assert(sizeof(Constants) == 336);

    // PsrInPlaceReplace.hlsl's PsrObjectTransform: the geometry pass's ObjectConstants model and normal matrix, and the
    // current-to-previous world transform.
    struct ObjectTransform
    {
        Matrix4 model;
        Matrix4 normalMatrix;
        Matrix4 currentToPrevious;
    };
    static_assert(sizeof(ObjectTransform) == 192);

    // Also builds the two-bounce variant (PsrInPlaceReplaceK2.hlsl); if that alone fails, single-bounce stays available.
    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
    {
        if(!InitializePipeline(device, shaderLibrary, "PsrInPlaceReplace.hlsl", m_pipeline))
        {
            return false;
        }

        InitializePipeline(device, shaderLibrary, "PsrInPlaceReplaceK2.hlsl", m_chainPipeline);
        return true;
    }

    // Render-thread adoption; both source pipelines are complete and have never been submitted.
    void AdoptPrepared(PsrInPlaceReplacePass& prepared)
    {
        m_pipeline.Swap(prepared.m_pipeline);
        m_chainPipeline.Swap(prepared.m_chainPipeline);
    }

    // Multibounce PSR: whether the two-bounce variant exists.
    bool IsChainAvailable() const
    {
        return m_chainPipeline.IsInitialized();
    }

    // Development verification only: a variant of the pass (PsrDiagnostics/, the same root signature) that also writes
    // per-pixel hit records. Execute uses it when given a diagnostic buffer.
    bool InitializeDiagnostic(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* pixelShaderPath)
    {
        m_diagnosticPipeline.Shutdown();
        return InitializePipeline(device, shaderLibrary, pixelShaderPath, m_diagnosticPipeline);
    }

    bool IsInitialized() const
    {
        return m_pipeline.IsInitialized();
    }

    // Renderer calls this only after its existing GPU-confirmed teardown.
    void Shutdown()
    {
        m_diagnosticPipeline.Shutdown();
        m_chainPipeline.Shutdown();
        m_pipeline.Shutdown();
    }

    // After EndGeometryPass. Every G-buffer resource is returned to the state it was found in. Records nothing (and
    // returns false) without the pipeline or its inputs. diagnosticHits (development verification) selects the
    // diagnostic pipeline; its UAV index must be in the constants. chain selects the two-bounce variant.
    bool Execute(ID3D12GraphicsCommandList* commandList, GBuffer& gBuffer, D3D12_GPU_VIRTUAL_ADDRESS constants,
        D3D12_GPU_VIRTUAL_ADDRESS transforms, D3D12_GPU_VIRTUAL_ADDRESS topLevel, unsigned int width,
        unsigned int height, Buffer* diagnosticHits = nullptr, bool chain = false) const
    {
        const Pipeline& pipeline = diagnosticHits ? m_diagnosticPipeline : chain ? m_chainPipeline : m_pipeline;

        if(!pipeline.IsInitialized() || !commandList || constants == 0 || transforms == 0 || topLevel == 0 ||
            width == 0 || height == 0)
        {
            return false;
        }

        RenderTarget* targets[] = {&gBuffer.GetMaterial(), &gBuffer.GetSurface(), &gBuffer.GetMotion(),
            &gBuffer.GetGeometricNormal()};
        DepthBuffer& depth = gBuffer.GetDepth();
        RenderTarget& identity = gBuffer.GetIdentity();
        D3D12_RESOURCE_STATES previousTargetStates[4]{};
        D3D12_CPU_DESCRIPTOR_HANDLE targetViews[4]{};

        for(int index = 0; index < 4; ++index)
        {
            previousTargetStates[index] = targets[index]->GetState();
            targets[index]->Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            targetViews[index] = targets[index]->GetRTV();
        }

        // EndGeometryPass leaves identity readable by pixel shaders; it is never bound for writing here.
        const D3D12_RESOURCE_STATES previousDepthState = depth.GetState();
        const D3D12_RESOURCE_STATES previousIdentityState = identity.GetState();
        depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        if((previousIdentityState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0)
        {
            identity.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        const D3D12_RESOURCE_STATES previousDiagnosticState = diagnosticHits ? diagnosticHits->GetResourceState() :
            D3D12_RESOURCE_STATE_COMMON;

        if(diagnosticHits)
        {
            diagnosticHits->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE depthView = depth.GetDSV();
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        commandList->OMSetRenderTargets(4, targetViews, FALSE, &depthView);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        pipeline.Bind(commandList);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->SetGraphicsRootConstantBufferView(0, constants);
        commandList->SetGraphicsRootShaderResourceView(1, transforms);
        commandList->SetGraphicsRootShaderResourceView(2, topLevel);
        commandList->DrawInstanced(3, 1, 0, 0);

        // Unbind before restoring the tracked states for later consumers.
        commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        if(diagnosticHits)
        {
            diagnosticHits->Transition(commandList, previousDiagnosticState);
        }

        for(int index = 0; index < 4; ++index)
        {
            targets[index]->Transition(commandList, previousTargetStates[index]);
        }

        depth.Transition(commandList, previousDepthState);
        identity.Transition(commandList, previousIdentityState);
        return true;
    }

private:
    bool InitializePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* pixelShaderPath,
        Pipeline& pipeline)
    {
        const Shader* vertexShader = shaderLibrary.Load({"PsrInPlaceReplace.hlsl", "VSMain", ShaderStage::Vertex});
        const Shader* pixelShader = shaderLibrary.Load({pixelShaderPath, "PSMain", ShaderStage::Pixel});

        if(!vertexShader || !pixelShader)
        {
            return false;
        }

        PipelineDescription pipelineDescription;
        pipelineDescription.vertexShader = vertexShader;
        pipelineDescription.pixelShader = pixelShader;
        // The geometry pass's first four targets, in its order (Renderer::BeginGeometryPass).
        pipelineDescription.renderTargets.count = 4;
        pipelineDescription.renderTargets.formats[0] = GBuffer::MaterialFormat;
        pipelineDescription.renderTargets.formats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipelineDescription.renderTargets.formats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipelineDescription.renderTargets.formats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Replaced pixels write their depth unconditionally; discarded pixels write nothing.
        pipelineDescription.depthEnabled = true;
        pipelineDescription.depthFormat = DXGI_FORMAT_D32_FLOAT;
        pipelineDescription.depthComparison = D3D12_COMPARISON_FUNC_ALWAYS;
        pipelineDescription.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipelineDescription.rootParameters.push_back(
            {RootParameterType::ConstantBuffer, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL});
        pipelineDescription.rootParameters.push_back(
            {RootParameterType::ShaderResource, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL});
        pipelineDescription.rootParameters.push_back(
            {RootParameterType::ShaderResource, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL});
        // s0: Triangle.hlsl's material sampler (the default trilinear clamp) for the mirror's normal map; s1: the path
        // tracer's material sampler (trilinear wrap, RayTracingPipeline) for the reflected material.
        D3D12_STATIC_SAMPLER_DESC materialSampler{};
        materialSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        materialSampler.MaxAnisotropy = 1;
        materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        materialSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        materialSampler.MaxLOD = D3D12_FLOAT32_MAX;
        materialSampler.ShaderRegister = 1;
        materialSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        pipelineDescription.additionalSamplers.push_back(materialSampler);

        return pipeline.Initialize(device, pipelineDescription);
    }

    Pipeline m_pipeline;
    Pipeline m_chainPipeline;
    Pipeline m_diagnosticPipeline;
};
