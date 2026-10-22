#include "PathTraceSpatialPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool PathTraceSpatialPass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_initializationFailed)
    {
        return false;
    }

    // The normal filter and (RTXPT D.3B.0) the diagnostic views share one source and root signature.
    const auto createPipeline = [&](const char* entryPoint, ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = "PathTraceSpatial.hlsl";
        shaderDescription.entryPoint = entryPoint;
        shaderDescription.stage = ShaderStage::Compute;
        Shader* computeShader = shaderLibrary.Load(shaderDescription);

        ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = computeShader;

        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        pipelineDescription.rootParameters.push_back(constants);

        return computeShader && pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription);
    };

    if(!createPipeline("CSMain", m_pipeline) || !createPipeline("CSDiagnostic", m_diagnosticPipeline))
    {
        Log::Error("The path tracing spatial pass could not be created.");
        m_pipeline.Shutdown();
        m_diagnosticPipeline.Shutdown();
        m_initializationFailed = true;
        return false;
    }

    m_initialized = true;
    return true;
}

bool PathTraceSpatialPass::InitializeAtrous(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_atrousInitialized)
    {
        return true;
    }

    if(m_atrousInitializationFailed)
    {
        return false;
    }

    const char* entryPoints[5] = {"CSFirst", "CSFirstPresent", "CSSecond", "CSFirstPresentDiagnostic",
        "CSSecondDiagnostic"};

    for(int index = 0; index < 5; ++index)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = "PathTraceSpatialAtrous.hlsl";
        shaderDescription.entryPoint = entryPoints[index];
        shaderDescription.stage = ShaderStage::Compute;
        Shader* computeShader = shaderLibrary.Load(shaderDescription);

        ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = computeShader;

        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        pipelineDescription.rootParameters.push_back(constants);

        if(!computeShader || !m_atrousPipelines[index].Initialize(graphicsDevice.GetDevice(), pipelineDescription))
        {
            Log::Error("The path tracing a-trous pipelines could not be created; the 5x5 spatial pass is used.");

            for(ComputePipeline& pipeline : m_atrousPipelines)
            {
                pipeline.Shutdown();
            }

            m_atrousInitializationFailed = true;
            return false;
        }
    }

    m_atrousInitialized = true;
    return true;
}

bool PathTraceSpatialPass::EnsureIntermediate(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(m_intermediateDiffuse.GetResource() && m_intermediateWidth == width && m_intermediateHeight == height)
    {
        return true;
    }

    ReleaseIntermediate();

    constexpr D3D12_RESOURCE_FLAGS unorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const wchar_t* names[2] = {L"Path Trace A-Trous Diffuse", L"Path Trace A-Trous Specular"};
    Texture* textures[2] = {&m_intermediateDiffuse, &m_intermediateSpecular};

    for(int index = 0; index < 2; ++index)
    {
        Texture& texture = *textures[index];

        if(!texture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, unorderedAccess,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !texture.CreateSRV(device, descriptorHeap) ||
            !texture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("The path tracing a-trous intermediate could not be created; the 5x5 spatial pass is used.");
            ReleaseIntermediate();
            return false;
        }

        texture.GetResource()->SetName(names[index]);
    }

    m_intermediateWidth = width;
    m_intermediateHeight = height;
    return true;
}

void PathTraceSpatialPass::ReleaseIntermediate()
{
    m_intermediateDiffuse.Shutdown();
    m_intermediateSpecular.Shutdown();
    m_intermediateWidth = 0;
    m_intermediateHeight = 0;
}

std::uint64_t PathTraceSpatialPass::GetIntermediateBytes() const
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(m_intermediateWidth) * m_intermediateHeight;
    return m_intermediateDiffuse.GetResource() ? pixels * 16u : 0u;
}

void PathTraceSpatialPass::ExecuteAtrous(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    const PathTraceSpatialConstants& spatial,
    std::uint32_t iterations,
    unsigned int width,
    unsigned int height)
{
    const bool twoIterations = iterations >= 2u;

    if(!m_atrousInitialized || !commandList || width == 0 || height == 0 ||
        (twoIterations && (!m_intermediateDiffuse.GetResource() || m_intermediateWidth != width ||
            m_intermediateHeight != height)))
    {
        return;
    }

    PathTraceAtrousConstants constants{};
    constants.inverseCameraRelativeViewProjection = spatial.inverseCameraRelativeViewProjection;

    for(int axis = 0; axis < 3; ++axis)
    {
        constants.cameraForward[axis] = spatial.cameraForward[axis];
    }

    constants.depthSigma = spatial.depthSigma;
    constants.resolution[0] = width;
    constants.resolution[1] = height;
    constants.temporalDiffuseIndex = spatial.diffuseIndex;
    constants.temporalSpecularIndex = spatial.specularIndex;
    constants.momentsIndex = spatial.momentsIndex;
    constants.depthIndex = spatial.depthIndex;
    constants.surfaceIndex = spatial.surfaceIndex;
    constants.geometricNormalIndex = spatial.geometricNormalIndex;
    constants.materialIndex = spatial.materialIndex;
    constants.outputIndex = spatial.outputIndex;
    constants.luminanceSigma = spatial.luminanceSigma;
    constants.normalPower = spatial.normalPower;
    constants.geometricNormalThreshold = spatial.geometricNormalThreshold;
    constants.specularRoughnessMinimum = spatial.specularRoughnessMinimum;
    constants.specularRoughnessTolerance = spatial.specularRoughnessTolerance;
    constants.diffuseMaximumHistory = spatial.diffuseMaximumHistory;
    constants.visualizationMode = spatial.visualizationMode;
    constants.visualizationSignal = spatial.visualizationSignal;
    constants.exposureScale = spatial.exposureScale;
    constants.differenceScale = spatial.differenceScale;

    m_lastConstants = spatial;
    m_lastConstants.resolution[0] = width;
    m_lastConstants.resolution[1] = height;
    const bool diagnostics = spatial.visualizationMode != 0u;
    const UINT groupsX = (width + 7u) / 8u;
    const UINT groupsY = (height + 7u) / 8u;

    const auto dispatch = [&](ComputePipeline& pipeline, const PathTraceAtrousConstants& values)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&values, sizeof(PathTraceAtrousConstants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            Log::Error("Failed to allocate path tracing a-trous constants.");
            return false;
        }

        pipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch(groupsX, groupsY, 1);
        return true;
    };

    if(!twoIterations)
    {
        dispatch(m_atrousPipelines[diagnostics ? 3 : 1], constants);
        return;
    }

    // Iteration 1 writes the intermediate; iteration 2 reads it and writes the presentation.
    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_intermediateDiffuse.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_intermediateSpecular.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PathTraceAtrousConstants first = constants;
    first.intermediateDiffuseIndex = m_intermediateDiffuse.GetUAVIndex();
    first.intermediateSpecularIndex = m_intermediateSpecular.GetUAVIndex();

    if(!dispatch(m_atrousPipelines[0], first))
    {
        return;
    }

    m_intermediateDiffuse.Transition(commandList, shaderRead);
    m_intermediateSpecular.Transition(commandList, shaderRead);
    PathTraceAtrousConstants second = constants;
    second.intermediateDiffuseIndex = m_intermediateDiffuse.GetDescriptorIndex();
    second.intermediateSpecularIndex = m_intermediateSpecular.GetDescriptorIndex();
    dispatch(m_atrousPipelines[diagnostics ? 4 : 2], second);
}

void PathTraceSpatialPass::Shutdown()
{
    ReleaseIntermediate();

    for(ComputePipeline& pipeline : m_atrousPipelines)
    {
        pipeline.Shutdown();
    }

    m_atrousInitialized = false;
    m_atrousInitializationFailed = false;
    m_pipeline.Shutdown();
    m_diagnosticPipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
}

void PathTraceSpatialPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    PathTraceSpatialConstants constants,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized || !commandList || width == 0 || height == 0)
    {
        return;
    }

    constants.resolution[0] = width;
    constants.resolution[1] = height;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants,
        sizeof(PathTraceSpatialConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        Log::Error("Failed to allocate path tracing spatial constants.");
        return;
    }

    m_lastConstants = constants;
    (constants.visualizationMode != 0u ? m_diagnosticPipeline : m_pipeline).Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
}
