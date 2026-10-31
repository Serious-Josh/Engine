#include "PixelCausticPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <cstring>

namespace
{
    // CausticPixel.hlsl cbuffer Constants (b0): sixteen words, receiverPoint/totalArea, receiverView, the mixture
    // fraction row and the GPULight. receiverPoint/receiverView are unused by the raster-receiver estimator.
    struct EstimatorConstants
    {
        std::uint32_t words[16];
        float receiverPoint[4];
        float receiverView[4];
        float broad[4];
        float light[16];
    };

    static_assert(sizeof(EstimatorConstants) == 176);

    // CausticPixel.hlsl cbuffer RasterConstants (b1), as B3-T's RasterBinding.
    struct RasterConstants
    {
        float inverseCameraRelativeViewProjection[16];
        float cameraPosition[3];
        std::uint32_t depthIndex;
        std::uint32_t materialTextureIndex;
        std::uint32_t surfaceIndex;
        std::uint32_t geometricNormalIndex;
        std::uint32_t tupleOutputIndex;
        std::uint32_t resolution[2];
        std::uint32_t probePixel[2];
        std::uint32_t materialTableIndex;
        std::uint32_t identityIndex;
    };

    static_assert(sizeof(RasterConstants) == 120);

    // CausticPixel.hlsl cbuffer TileConstants (b2), one per tile.
    struct TileConstants
    {
        std::uint32_t tileStart;
        std::uint32_t tileCount;
        std::uint32_t sampleCount;
        std::uint32_t targetsIndex;
        std::uint32_t scratchIndex;
        std::uint32_t diffuseOutputIndex;
        std::uint32_t probesIndex;
        std::uint32_t tupleIndex;
        std::uint32_t scheduleDivisor;
        std::uint32_t injectFlags;
        std::uint32_t captureProbes;
        std::uint32_t tileCapacity;
        std::uint32_t probePixels[8];
        std::uint32_t faultIndex;
        std::uint32_t firstPixel;
        std::uint32_t specularOutputIndex;
        std::uint32_t probeOutputIndex;
    };

    static_assert(sizeof(TileConstants) == 96);

    struct ViewConstants
    {
        std::uint32_t diffuseIndex;
        std::uint32_t specularIndex;
        std::uint32_t faultLatchIndex;
        std::uint32_t outputIndex;
        std::uint32_t resolution[2];
        std::uint32_t view;
        std::uint32_t padding;
    };

    static_assert(sizeof(ViewConstants) == 32);

    // CausticPixelInsert.hlsl cbuffer InsertConstants (b0).
    struct InsertConstants
    {
        std::uint32_t causticDiffuseIndex;
        std::uint32_t causticSpecularIndex;
        std::uint32_t extractionDiffuseIndex;
        std::uint32_t extractionSpecularIndex;
        std::uint32_t latchIndex;
        std::uint32_t expectedEpoch;
        std::uint32_t resolution[2];
    };

    static_assert(sizeof(InsertConstants) == 32);

    // CausticPixelInsertTamper.hlsl cbuffer TamperConstants (b0), verification only.
    struct TamperConstants
    {
        std::uint32_t causticDiffuseIndex;
        std::uint32_t causticSpecularIndex;
        std::uint32_t extractionDiffuseIndex;
        std::uint32_t extractionSpecularIndex;
        std::uint32_t latchIndex;
        std::uint32_t mode;
        std::uint32_t pixel[2];
    };

    static_assert(sizeof(TamperConstants) == 32);

    // CausticRasterIdentityAudit.hlsl cbuffer AuditConstants (b0), verification only.
    struct IdentityAuditConstants
    {
        float inverseCameraRelativeViewProjection[16];
        float cameraPosition[3];
        std::uint32_t identityIndex;
        std::uint32_t outputIndex;
        std::uint32_t recordCapacity;
        std::uint32_t resolution[2];
        std::uint32_t diffuseIndex;
        std::uint32_t padding[3];
    };

    static_assert(sizeof(IdentityAuditConstants) == 112);

    constexpr std::uint32_t ProbeCount = 8;
    constexpr std::uint64_t ProbeAttemptBytes = std::uint64_t(ProbeCount) * 16 * 96;
    constexpr std::uint64_t ProbeTupleBytes = std::uint64_t(ProbeCount) * 192;
    constexpr std::uint64_t ProbeOutputBytes = std::uint64_t(ProbeCount) * 64;
    constexpr std::uint32_t TargetBytesPerTriangle = 32;
    constexpr std::uint32_t ScratchBytesPerReceiver = 64;
    constexpr D3D12_RESOURCE_STATES ShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    RootParameterDescription RootParameter(RootParameterType type, unsigned int shaderRegister)
    {
        RootParameterDescription parameter{};
        parameter.type = type;
        parameter.shaderRegister = shaderRegister;
        parameter.visibility = D3D12_SHADER_VISIBILITY_ALL;
        return parameter;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateReadback(ID3D12Device* device, std::uint64_t bytes)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource))))
        {
            return nullptr;
        }

        return resource;
    }

    std::uint64_t CommittedBytes(ID3D12Device* device, ID3D12Resource* resource)
    {
        if(!device || !resource)
        {
            return 0;
        }

        const auto description = resource->GetDesc();
        return device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    }

    void UAVBarrier(ID3D12GraphicsCommandList* commandList)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    }
}

bool PixelCausticPass::Refuse(std::string reason)
{
    m_reason = std::move(reason);
    return false;
}

bool PixelCausticPass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized || m_initializationFailed)
    {
        return m_initialized;
    }

    ID3D12Device* device = graphicsDevice.GetDevice();
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool doublePrecision = device &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.DoublePrecisionFloatShaderOps != FALSE;

    const auto create = [&](ComputePipeline& pipeline, const char* path, const char* entryPoint, bool estimator)
    {
        ShaderDescription shader{};
        shader.path = path;
        shader.entryPoint = entryPoint;
        shader.stage = ShaderStage::Compute;
        ComputePipelineDescription description{};
        description.computeShader = shaderLibrary.Load(shader);
        description.rootParameters = {RootParameter(RootParameterType::ConstantBuffer, 0)};

        if(estimator)
        {
            for(unsigned int index = 0; index < 3; ++index)
            {
                description.rootParameters.push_back(RootParameter(RootParameterType::ShaderResource, index));
            }

            description.rootParameters.push_back(RootParameter(RootParameterType::ConstantBuffer, 1));
            description.rootParameters.push_back(RootParameter(RootParameterType::ConstantBuffer, 2));
            // P5D.2B receiver table (t3), root slot 6 so the earlier slots keep their indices.
            description.rootParameters.push_back(RootParameter(RootParameterType::ShaderResource, 3));
        }

        return description.computeShader && pipeline.Initialize(device, description);
    };

    // The FP64 reduction and proposal are part of the validated estimator; without FP64 the pass stays off.
    m_initialized = doublePrecision && create(m_classify, "CausticPixel.hlsl", "CSClassify", true) &&
        create(m_setup, "CausticPixel.hlsl", "CSSetup", true) &&
        create(m_sampleReduce, "CausticPixel.hlsl", "CSSampleReduce", true) &&
        create(m_publish, "CausticPixel.hlsl", "CSPublish", true) &&
        create(m_view, "CausticPixelView.hlsl", "CSView", false);
    m_initializationFailed = !m_initialized;

    // Insertion reads and writes the RGBA16F extraction through typed UAVs.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT halfSupport{DXGI_FORMAT_R16G16B16A16_FLOAT};
    const bool typedHalfUAV = m_initialized &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &halfSupport, sizeof(halfSupport))) &&
        (halfSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 &&
        (halfSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    m_insertionSupported = typedHalfUAV && create(m_insertBegin, "CausticPixelInsert.hlsl", "CSBegin", false) &&
        create(m_insertValidate, "CausticPixelInsert.hlsl", "CSValidate", false) &&
        create(m_insertApply, "CausticPixelInsert.hlsl", "CSInsert", false);

#if defined(ENGINE_HAS_VERIFICATION)
    m_insertTamperReady = m_insertionSupported &&
        create(m_insertTamper, "CausticPixelInsertTamper.hlsl", "CSTamper", false);

    // The identity audit traces the frame's TLAS: constants (b0) and the TLAS root SRV (t0).
    const auto createAudit = [&](ComputePipeline& pipeline, const char* entryPoint)
    {
        ShaderDescription shader{};
        shader.path = "CausticRasterIdentityAudit.hlsl";
        shader.entryPoint = entryPoint;
        shader.stage = ShaderStage::Compute;
        ComputePipelineDescription description{};
        description.computeShader = shaderLibrary.Load(shader);
        description.rootParameters = {RootParameter(RootParameterType::ConstantBuffer, 0),
            RootParameter(RootParameterType::ShaderResource, 0)};
        return description.computeShader && pipeline.Initialize(device, description);
    };
    m_identityAuditReady = createAudit(m_identityClear, "CSClear") && createAudit(m_identityAudit, "CSAudit") &&
        createAudit(m_identityCertification, "CSCertification");

    if(m_initialized)
    {
        m_diagnosticPipelinesReady = create(m_probeClassify, "CausticPixelProbes.hlsl", "CSClassify", true) &&
            create(m_probeSetup, "CausticPixelProbes.hlsl", "CSSetup", true) &&
            create(m_probeSampleReduce, "CausticPixelProbes.hlsl", "CSSampleReduce", true) &&
            create(m_probePublish, "CausticPixelProbes.hlsl", "CSPublish", true);
    }
#endif

    if(!m_initialized)
    {
        Log::Error(doublePrecision ? "Pixel-centred caustic pipelines could not be created." :
            "Pixel-centred caustics require FP64 shader operations.");
    }

    return m_initialized;
}

void PixelCausticPass::Shutdown()
{
    ReleaseResources();
    m_classify.Shutdown();
    m_setup.Shutdown();
    m_sampleReduce.Shutdown();
    m_publish.Shutdown();
    m_view.Shutdown();
    m_insertBegin.Shutdown();
    m_insertValidate.Shutdown();
    m_insertApply.Shutdown();
    m_insertionSupported = false;

#if defined(ENGINE_HAS_VERIFICATION)
    m_insertTamper.Shutdown();
    m_insertTamperReady = false;
    m_identityClear.Shutdown();
    m_identityAudit.Shutdown();
    m_identityCertification.Shutdown();
    m_identityAuditReady = false;
    m_probeClassify.Shutdown();
    m_probeSetup.Shutdown();
    m_probeSampleReduce.Shutdown();
    m_probePublish.Shutdown();
    m_diagnosticPipelinesReady = false;
#endif

    m_initialized = false;
    m_initializationFailed = false;
}

bool PixelCausticPass::EnsureResources(ID3D12Device* device, DescriptorHeap& heap, std::uint32_t width,
    std::uint32_t height, std::uint32_t triangleCount)
{
    // Resolution-sized resources are replaced only at a drained boundary (ReleaseResources).
    if(m_diffuse.GetResource() && (m_width != width || m_height != height))
    {
        return Refuse("caustic outputs belong to another extent; a drained release is required");
    }

    if(!m_diffuse.GetResource())
    {
        constexpr D3D12_RESOURCE_FLAGS unorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto createTexture = [&](Texture& texture, DXGI_FORMAT format, const wchar_t* name)
        {
            if(!texture.Initialize(device, width, height, 1, 1, format, unorderedAccess, initialState) ||
                !texture.CreateSRV(device, heap) || !texture.CreateUAV(device, heap))
            {
                return false;
            }

            texture.GetResource()->SetName(name);
            return true;
        };

        if(!createTexture(m_diffuse, OutputFormat, L"Pixel Caustic Diffuse") ||
            !createTexture(m_specular, OutputFormat, L"Pixel Caustic Specular") ||
            !createTexture(m_present, DXGI_FORMAT_R16G16B16A16_FLOAT, L"Pixel Caustic View") ||
            !m_scratch.InitializeGPUWritable(device, std::size_t(TileCapacity) * ScratchBytesPerReceiver) ||
            !m_scratch.CreateRawUAV(device, heap) ||
            !m_faultLatch.InitializeGPUWritable(device, FaultLatchBytes) || !m_faultLatch.CreateRawUAV(device, heap) ||
            !m_faultLatch.CreateRawSRV(device, heap))
        {
            ReleaseResources();
            return Refuse("pixel caustic output allocation failed");
        }

        for(auto& measurement : m_measurements)
        {
            measurement.readback = CreateReadback(device, FaultLatchBytes);

            if(!measurement.readback)
            {
                ReleaseResources();
                return Refuse("pixel caustic fault readback allocation failed");
            }
        }

        m_width = width;
        m_height = height;
    }

    // Tile targets are sized once for the bounded population, so a scene's triangle count never needs a
    // replacement (each recording indexes them with its own count as the stride).
    if(triangleCount > MaximumTriangles || (m_targets.GetResource() && m_targetTriangles != MaximumTriangles))
    {
        return Refuse("caustic proposal targets cannot hold this triangle count");
    }

    if(!m_targets.GetResource())
    {
        triangleCount = MaximumTriangles;

        if(!m_targets.InitializeGPUWritable(device, std::size_t(TileCapacity) * triangleCount * TargetBytesPerTriangle) ||
            !m_targets.CreateRawUAV(device, heap))
        {
            m_targets.Shutdown();
            return Refuse("pixel caustic proposal target allocation failed");
        }

        m_targetTriangles = triangleCount;
    }

    m_resourceBytes = CommittedBytes(device, m_diffuse.GetResource()) + CommittedBytes(device, m_specular.GetResource()) +
        CommittedBytes(device, m_present.GetResource()) + CommittedBytes(device, m_targets.GetResource()) +
        CommittedBytes(device, m_scratch.GetResource()) + CommittedBytes(device, m_faultLatch.GetResource());

    for(const auto& measurement : m_measurements)
    {
        m_resourceBytes += CommittedBytes(device, measurement.readback.Get());
    }

    if(m_resourceBytes > MaximumResourceBytes)
    {
        return Refuse("pixel caustic resources exceed their memory bound");
    }

    return true;
}

#if defined(ENGINE_HAS_VERIFICATION)
bool PixelCausticPass::EnsureDiagnosticResources(ID3D12Device* device, DescriptorHeap& heap, std::uint32_t triangleCount)
{
    if(!m_probeAttempts.GetResource())
    {
        if(!m_probeAttempts.InitializeGPUWritable(device, ProbeAttemptBytes) || !m_probeAttempts.CreateRawUAV(device, heap) ||
            !m_probeTuples.InitializeGPUWritable(device, ProbeTupleBytes) || !m_probeTuples.CreateRawUAV(device, heap) ||
            !m_probeOutputs.InitializeGPUWritable(device, ProbeOutputBytes) || !m_probeOutputs.CreateRawUAV(device, heap))
        {
            return false;
        }

        m_probeAttemptsReadback = CreateReadback(device, ProbeAttemptBytes);
        m_probeTuplesReadback = CreateReadback(device, ProbeTupleBytes);
        m_probeOutputsReadback = CreateReadback(device, ProbeOutputBytes);
        m_probeLatchReadback = CreateReadback(device, FaultLatchBytes);
    }

    if(!m_probeTargetsReadback || m_probeTriangles != triangleCount)
    {
        m_probeTargetsReadback = CreateReadback(device, std::uint64_t(ProbeCount) * triangleCount * TargetBytesPerTriangle);
        m_probeTriangles = triangleCount;
    }

    return m_probeAttemptsReadback && m_probeTuplesReadback && m_probeOutputsReadback && m_probeLatchReadback &&
        m_probeTargetsReadback;
}

bool PixelCausticPass::ReadDiagnosticIdentity(std::vector<std::uint32_t>& words) const
{
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(IdentityAuditBytes)};
    const D3D12_RANGE noWrites{0, 0};

    if(!m_identityAuditReadback || FAILED(m_identityAuditReadback->Map(0, &readRange, &mapped)))
    {
        return false;
    }

    words.resize(static_cast<std::size_t>(IdentityAuditBytes / 4));
    std::memcpy(words.data(), mapped, static_cast<std::size_t>(IdentityAuditBytes));
    m_identityAuditReadback->Unmap(0, &noWrites);
    return true;
}

bool PixelCausticPass::ReadDiagnosticProbes(std::vector<std::uint8_t>& attempts, std::vector<std::uint8_t>& tuples,
    std::vector<std::uint8_t>& targets, std::vector<std::uint8_t>& outputs, std::vector<std::uint8_t>& latch) const
{
    const auto read = [](ID3D12Resource* resource, std::uint64_t bytes, std::vector<std::uint8_t>& data)
    {
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
        const D3D12_RANGE noWrites{0, 0};

        if(!resource || FAILED(resource->Map(0, &readRange, &mapped)))
        {
            return false;
        }

        data.resize(static_cast<std::size_t>(bytes));
        std::memcpy(data.data(), mapped, data.size());
        resource->Unmap(0, &noWrites);
        return true;
    };

    return read(m_probeAttemptsReadback.Get(), ProbeAttemptBytes, attempts) &&
        read(m_probeTuplesReadback.Get(), ProbeTupleBytes, tuples) &&
        read(m_probeTargetsReadback.Get(), std::uint64_t(ProbeCount) * m_probeTriangles * TargetBytesPerTriangle, targets) &&
        read(m_probeOutputsReadback.Get(), ProbeOutputBytes, outputs) &&
        read(m_probeLatchReadback.Get(), FaultLatchBytes, latch);
}

bool PixelCausticPass::RecordTextureReadback(ID3D12GraphicsCommandList* commandList, ID3D12Device* device,
    Texture& texture, TextureReadback& readback)
{
    const auto description = texture.GetResource()->GetDesc();
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &readback.footprint, &readback.rows, &readback.rowBytes, &bytes);

    if(!readback.buffer || readback.buffer->GetDesc().Width < bytes)
    {
        readback.buffer = CreateReadback(device, bytes);
    }

    if(!readback.buffer)
    {
        return false;
    }

    const auto previous = texture.GetSubresourceState(0);
    texture.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture.GetResource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.footprint;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    texture.Transition(commandList, previous);
    return true;
}

bool PixelCausticPass::ReadDiagnosticInsertion(InsertionCapture& capture) const
{
    if(!m_insertionCaptured)
    {
        return false;
    }

    const auto read = [](const TextureReadback& readback, std::vector<std::uint8_t>& data)
    {
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readback.buffer->GetDesc().Width)};
        const D3D12_RANGE noWrites{0, 0};

        if(!readback.buffer || FAILED(readback.buffer->Map(0, &readRange, &mapped)))
        {
            return false;
        }

        data.resize(static_cast<std::size_t>(readback.rowBytes * readback.rows));

        for(UINT row = 0; row < readback.rows; ++row)
        {
            std::memcpy(data.data() + row * readback.rowBytes, static_cast<const std::uint8_t*>(mapped) +
                readback.footprint.Offset + std::size_t(row) * readback.footprint.Footprint.RowPitch,
                static_cast<std::size_t>(readback.rowBytes));
        }

        readback.buffer->Unmap(0, &noWrites);
        return true;
    };

    std::vector<std::uint8_t>* targets[] = {&capture.diffuseBefore, &capture.specularBefore, &capture.rawBefore,
        &capture.diffuseAfter, &capture.specularAfter, &capture.rawAfter, &capture.causticDiffuse,
        &capture.causticSpecular};

    for(std::size_t index = 0; index < m_insertionReadbacks.size(); ++index)
    {
        targets[index]->clear();

        if(m_insertionReadbacks[index].buffer && !read(m_insertionReadbacks[index], *targets[index]))
        {
            return false;
        }
    }

    capture.width = m_insertionCaptureWidth;
    capture.height = m_insertionCaptureHeight;
    std::vector<std::uint8_t> attempts, tuples, targetsUnused, outputs;
    return ReadDiagnosticProbes(attempts, tuples, targetsUnused, outputs, capture.latch);
}
#endif

bool PixelCausticPass::Record(ID3D12GraphicsCommandList* commandList, ID3D12Device* device, DescriptorHeap& heap,
    UploadBuffer& uploadBuffer, const PixelCausticFrameInput& input, const PixelCausticInsertionTarget* insertion,
    const CommandContext& context, std::uint64_t generation, GpuTimestampProfiler& profiler, unsigned int frameSlot)
{
    m_recordedThisFrame = false;

    // Stage 1: everything fallible, before any command.
    if(!m_initialized || !commandList || !device)
    {
        return Refuse("pixel caustic pipelines are unavailable");
    }

    std::uint32_t attempts = AttemptsPerReceiver;
    std::uint32_t injectFlags = 0;
    std::uint32_t scheduleDivisor = 1;
    std::uint32_t seed = input.seed;
    bool probes = false;
    std::array<std::uint32_t, 8> probePixels{};
    probePixels.fill(UINT32_MAX);

#if defined(ENGINE_HAS_VERIFICATION)
    if(m_diagnostics.enabled)
    {
        if(!m_diagnosticPipelinesReady)
        {
            return Refuse("pixel caustic diagnostic pipelines are unavailable");
        }

        probes = true;
        attempts = m_diagnostics.attemptsPerReceiver;
        injectFlags = m_diagnostics.injectFlags;
        scheduleDivisor = m_diagnostics.scheduleDivisor;
        seed = m_diagnostics.seedOverride ? m_diagnostics.seed : seed;
        probePixels = m_diagnostics.probePixels;
    }
#endif

    const auto triangleCount = static_cast<std::uint32_t>(input.triangles.size());

    if(!input.topLevel || !input.materialTable || input.materialTableDescriptor == UINT_MAX || input.epoch == 0 ||
        !input.width || !input.height || input.width > MaximumDimension || input.height > MaximumDimension ||
        triangleCount == 0 || triangleCount > MaximumTriangles || !(input.totalArea > 0.0f) ||
        attempts == 0 || attempts > MaximumAttemptsPerReceiver || scheduleDivisor == 0)
    {
        return Refuse("pixel caustic frame input is outside the validated bounds");
    }

    if(!generation || !context.GetSubmissionLifetime())
    {
        return Refuse("invalid pixel caustic recording provenance");
    }

    Measurement* measurement = nullptr;

    for(auto& candidate : m_measurements)
    {
        if(candidate.state == MeasurementState::Empty)
        {
            measurement = &candidate;
        }
        else if(candidate.generation == generation)
        {
            return Refuse("this recording already holds a pixel caustic measurement");
        }
    }

    if(!measurement)
    {
        return Refuse("bounded pixel caustic measurement capacity exhausted");
    }

    if(!EnsureResources(device, heap, input.width, input.height, triangleCount))
    {
        return false;
    }

#if defined(ENGINE_HAS_VERIFICATION)
    if(probes && !EnsureDiagnosticResources(device, heap, triangleCount))
    {
        return Refuse("pixel caustic diagnostic allocation failed");
    }
#endif

    EstimatorConstants constants{};
    constants.words[0] = 0;
    constants.words[1] = seed;
    constants.words[2] = 1; // Proposal M: the validated broad + truncated-Cauchy mixture.
    constants.words[3] = triangleCount;
    constants.words[4] = input.reflectorMaterial;
    constants.words[5] = input.receiverMaterial;
    constants.words[6] = input.materialEnergyTable;
    constants.words[7] = input.materialAverageTable;
    constants.words[8] = input.materialCosineMapping;
    constants.words[9] = UINT32_MAX;
    constants.words[10] = input.reflectorDense;
    constants.words[11] = input.reflectorNative;
    constants.words[12] = input.receiverDense;
    constants.words[13] = input.receiverNative;
    constants.words[14] = input.epoch;
    constants.words[15] = input.epoch;
    constants.receiverPoint[3] = input.totalArea;
    constants.broad[0] = 0.1f;

    // P5D.2B: automatic receiver flag (receiverView.w) and table entry count (broad.y), both as exact uint32.
    if(input.automaticReceivers && (input.receiverTable.empty() || input.identityIndex == UINT_MAX))
    {
        return Refuse("automatic receivers need a receiver table and the raster identity target");
    }

    const std::uint32_t automaticReceivers = input.automaticReceivers ? 1u : 0u;
    const auto receiverEntries = static_cast<std::uint32_t>(input.receiverTable.size());
    std::memcpy(&constants.receiverView[3], &automaticReceivers, sizeof(automaticReceivers));
    std::memcpy(&constants.broad[1], &receiverEntries, sizeof(receiverEntries));
    std::memcpy(constants.light, input.lightRecord.data(), sizeof(constants.light));

    RasterConstants raster{};
    std::memcpy(raster.inverseCameraRelativeViewProjection, &input.inverseCameraRelativeViewProjection.m[0][0],
        sizeof(raster.inverseCameraRelativeViewProjection));
    std::memcpy(raster.cameraPosition, input.cameraPosition.data(), sizeof(raster.cameraPosition));
    raster.depthIndex = input.depthIndex;
    raster.materialTextureIndex = input.materialIndex;
    raster.surfaceIndex = input.surfaceIndex;
    raster.geometricNormalIndex = input.geometricNormalIndex;
    raster.tupleOutputIndex = UINT32_MAX;
    raster.resolution[0] = input.width;
    raster.resolution[1] = input.height;
    raster.materialTableIndex = input.materialTableDescriptor;
    raster.identityIndex = input.identityIndex;

    const std::uint32_t pixels = input.width * input.height;
    const std::uint32_t tileCount = (pixels + TileCapacity - 1) / TileCapacity;
    constexpr std::size_t ConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    std::vector<std::uint8_t> tileData(std::size_t(tileCount) * ConstantAlignment);

    for(std::uint32_t tile = 0; tile < tileCount; ++tile)
    {
        TileConstants values{};
        values.tileStart = tile * TileCapacity;
        values.tileCount = (std::min)(TileCapacity, pixels - values.tileStart);
        values.sampleCount = attempts;
        values.targetsIndex = m_targets.GetUAVDescriptorIndex();
        values.scratchIndex = m_scratch.GetUAVDescriptorIndex();
        values.diffuseOutputIndex = m_diffuse.GetUAVIndex();
        values.probesIndex = UINT32_MAX;
        values.tupleIndex = UINT32_MAX;
        values.scheduleDivisor = scheduleDivisor;
        values.injectFlags = injectFlags;
        values.captureProbes = probes ? 1u : 0u;
        values.tileCapacity = TileCapacity;
        std::memcpy(values.probePixels, probePixels.data(), sizeof(values.probePixels));
        values.faultIndex = m_faultLatch.GetUAVDescriptorIndex();
        values.firstPixel = 0;
        values.specularOutputIndex = m_specular.GetUAVIndex();
        values.probeOutputIndex = UINT32_MAX;

#if defined(ENGINE_HAS_VERIFICATION)
        if(probes)
        {
            values.probesIndex = m_probeAttempts.GetUAVDescriptorIndex();
            values.tupleIndex = m_probeTuples.GetUAVDescriptorIndex();
            values.probeOutputIndex = m_probeOutputs.GetUAVDescriptorIndex();
        }
#endif

        std::memcpy(tileData.data() + std::size_t(tile) * ConstantAlignment, &values, sizeof(values));
    }

    const D3D12_GPU_VIRTUAL_ADDRESS geometryAddress = uploadBuffer.Allocate(input.triangles.data(),
        input.triangles.size() * sizeof(PixelCausticTriangle), ConstantAlignment);
    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(constants),
        ConstantAlignment);
    const D3D12_GPU_VIRTUAL_ADDRESS rasterAddress = uploadBuffer.Allocate(&raster, sizeof(raster), ConstantAlignment);
    const D3D12_GPU_VIRTUAL_ADDRESS tileAddress = uploadBuffer.Allocate(tileData.data(), tileData.size(),
        ConstantAlignment);

    // The receiver table lives in this recording's upload ring, retained with the frame like the triangle records.
    // The explicit path binds one inert entry so the root SRV is always valid; its shader branch never reads it.
    const std::array<std::uint32_t, 4> inertReceiver{0u, UINT32_MAX, UINT32_MAX, 0u};
    const D3D12_GPU_VIRTUAL_ADDRESS receiverTableAddress = input.automaticReceivers ?
        uploadBuffer.Allocate(input.receiverTable.data(), input.receiverTable.size() * sizeof(inertReceiver), ConstantAlignment) :
        uploadBuffer.Allocate(inertReceiver.data(), sizeof(inertReceiver), ConstantAlignment);

    if(!geometryAddress || !constantsAddress || !rasterAddress || !tileAddress || !receiverTableAddress)
    {
        return Refuse("pixel caustic upload space is unavailable");
    }

    // The insertion transaction's targets and constants, before any command.
    D3D12_GPU_VIRTUAL_ADDRESS insertAddress = 0;
    const UINT viewGroupsX = (input.width + 7) / 8;
    const UINT viewGroupsY = (input.height + 7) / 8;

    if(insertion)
    {
        const auto matches = [&](const Texture* texture, DXGI_FORMAT format)
        {
            if(!texture || !texture->GetResource())
            {
                return false;
            }

            const auto description = texture->GetResource()->GetDesc();
            return description.Format == format && description.Width == input.width &&
                description.Height == input.height && texture->GetUAVIndex() != UINT_MAX;
        };

        if(!m_insertionSupported)
        {
            return Refuse("caustic insertion needs RGBA16F typed UAV loads and stores");
        }

        if(!matches(insertion->diffuse, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !matches(insertion->specular, DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            return Refuse("the extraction textures are not this extent's RGBA16F signals");
        }

        InsertConstants values{};
        values.causticDiffuseIndex = m_diffuse.GetDescriptorIndex();
        values.causticSpecularIndex = m_specular.GetDescriptorIndex();
        values.extractionDiffuseIndex = insertion->diffuse->GetUAVIndex();
        values.extractionSpecularIndex = insertion->specular->GetUAVIndex();
        values.latchIndex = m_faultLatch.GetUAVDescriptorIndex();
        values.expectedEpoch = input.epoch;
        values.resolution[0] = input.width;
        values.resolution[1] = input.height;
        insertAddress = uploadBuffer.Allocate(&values, sizeof(values), ConstantAlignment);

        if(!insertAddress)
        {
            return Refuse("pixel caustic insertion upload space is unavailable");
        }
    }

    // CSBegin writes the sentinel through the diffuse UAV; it needs its own constants with the UAV index.
    D3D12_GPU_VIRTUAL_ADDRESS beginAddress = 0;

    if(insertion)
    {
        InsertConstants values{};
        values.causticDiffuseIndex = m_diffuse.GetUAVIndex();
        values.latchIndex = m_faultLatch.GetUAVDescriptorIndex();
        values.expectedEpoch = input.epoch;
        values.resolution[0] = input.width;
        values.resolution[1] = input.height;
        beginAddress = uploadBuffer.Allocate(&values, sizeof(values), ConstantAlignment);

        if(!beginAddress)
        {
            return Refuse("pixel caustic insertion upload space is unavailable");
        }
    }

#if defined(ENGINE_HAS_VERIFICATION)
    D3D12_GPU_VIRTUAL_ADDRESS tamperAddress = 0;

    if(insertion && m_diagnostics.enabled && m_diagnostics.insertionTamper != 0)
    {
        if(!m_insertTamperReady)
        {
            return Refuse("pixel caustic tamper pipeline is unavailable");
        }

        TamperConstants values{};
        values.causticDiffuseIndex = m_diffuse.GetUAVIndex();
        values.causticSpecularIndex = m_specular.GetUAVIndex();
        values.extractionDiffuseIndex = insertion->diffuse->GetUAVIndex();
        values.extractionSpecularIndex = insertion->specular->GetUAVIndex();
        values.latchIndex = m_faultLatch.GetUAVDescriptorIndex();
        values.mode = m_diagnostics.insertionTamper;
        values.pixel[0] = m_diagnostics.tamperPixel[0];
        values.pixel[1] = m_diagnostics.tamperPixel[1];
        tamperAddress = uploadBuffer.Allocate(&values, sizeof(values), ConstantAlignment);

        if(!tamperAddress)
        {
            return Refuse("pixel caustic tamper upload space is unavailable");
        }
    }

    const bool captureInsertion = insertion && m_diagnostics.enabled && m_diagnostics.captureInsertion;
    m_insertionCaptured = false;

    D3D12_GPU_VIRTUAL_ADDRESS identityAuditAddress = 0;

    if(m_diagnostics.enabled && m_diagnostics.identityAudit)
    {
        if(!m_identityAuditReady || input.identityIndex == UINT_MAX)
        {
            return Refuse("raster identity audit is unavailable (pipeline or identity target missing)");
        }

        if(!m_identityAuditBuffer.GetResource())
        {
            if(!m_identityAuditBuffer.InitializeGPUWritable(device, IdentityAuditBytes) ||
                !m_identityAuditBuffer.CreateRawUAV(device, heap))
            {
                m_identityAuditBuffer.Shutdown();
                return Refuse("raster identity audit allocation failed");
            }

            m_identityAuditReadback = CreateReadback(device, IdentityAuditBytes);
        }

        IdentityAuditConstants values{};
        std::memcpy(values.inverseCameraRelativeViewProjection, &input.inverseCameraRelativeViewProjection.m[0][0],
            sizeof(values.inverseCameraRelativeViewProjection));
        std::memcpy(values.cameraPosition, input.cameraPosition.data(), sizeof(values.cameraPosition));
        values.identityIndex = input.identityIndex;
        values.outputIndex = m_identityAuditBuffer.GetUAVDescriptorIndex();
        values.recordCapacity = IdentityAuditRecords;
        values.resolution[0] = input.width;
        values.resolution[1] = input.height;
        values.diffuseIndex = m_diffuse.GetDescriptorIndex();
        identityAuditAddress = m_identityAuditReadback ? uploadBuffer.Allocate(&values, sizeof(values), ConstantAlignment) : 0;

        if(!identityAuditAddress)
        {
            return Refuse("raster identity audit upload space or readback is unavailable");
        }
    }
#endif

    // Stage 2: record. The pre-recording states are kept so a discarded list restores them exactly.
    const auto diffuseState = m_diffuse.GetSubresourceState(0);
    const auto specularState = m_specular.GetSubresourceState(0);
    const auto targetsState = m_targets.GetResourceState();
    const auto scratchState = m_scratch.GetResourceState();
    const auto latchState = m_faultLatch.GetResourceState();
    m_discardStates = {diffuseState, specularState, targetsState, scratchState, latchState};

    profiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::PixelCausticEstimate);
    ID3D12DescriptorHeap* heaps[] = {heap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    m_diffuse.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_specular.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_targets.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_scratch.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_faultLatch.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

#if defined(ENGINE_HAS_VERIFICATION)
    if(probes)
    {
        m_probeAttempts.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_probeTuples.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_probeOutputs.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
#endif

    // Transaction step 1: the unpublished sentinel in every pixel, before any tile publishes.
    if(insertion)
    {
        m_insertBegin.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, beginAddress);
        commandList->Dispatch(viewGroupsX, viewGroupsY, 1);
    }

#if defined(ENGINE_HAS_VERIFICATION)
    // P5D.2A identity audit: this frame's G-buffer identity against this frame's TLAS, before the estimator.
    // The buffer is returned to its prior state, so a discarded list leaves the tracked state unchanged.
    const auto auditState = m_identityAuditBuffer.GetResourceState();

    if(identityAuditAddress)
    {
        m_identityAuditBuffer.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_identityClear.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, identityAuditAddress);
        commandList->SetComputeRootShaderResourceView(1, input.topLevel);
        commandList->Dispatch(1, 1, 1);
        UAVBarrier(commandList);
        m_identityAudit.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, identityAuditAddress);
        commandList->SetComputeRootShaderResourceView(1, input.topLevel);
        commandList->Dispatch((input.width + 7) / 8, (input.height + 7) / 8, 1);
        UAVBarrier(commandList);
    }
#endif

    ComputePipeline* stages[] = {&m_classify, &m_setup, &m_sampleReduce, &m_publish};

#if defined(ENGINE_HAS_VERIFICATION)
    if(probes)
    {
        stages[0] = &m_probeClassify;
        stages[1] = &m_probeSetup;
        stages[2] = &m_probeSampleReduce;
        stages[3] = &m_probePublish;
    }
#endif

    for(std::uint32_t tile = 0; tile < tileCount; ++tile)
    {
        const std::uint32_t start = tile * TileCapacity;
        const std::uint32_t count = (std::min)(TileCapacity, pixels - start);

        // Orders this tile's reuse of the scratch and target buffers after the previous tile.
        UAVBarrier(commandList);

        for(std::uint32_t stage = 0; stage < 4; ++stage)
        {
            if(stage != 0)
            {
                UAVBarrier(commandList);
            }

            // Each pipeline owns its root signature, so binding one discards the root arguments.
            stages[stage]->Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, constantsAddress);
            commandList->SetComputeRootShaderResourceView(1, input.topLevel);
            commandList->SetComputeRootShaderResourceView(2, geometryAddress);
            commandList->SetComputeRootShaderResourceView(3, input.materialTable);
            commandList->SetComputeRootConstantBufferView(4, rasterAddress);
            commandList->SetComputeRootConstantBufferView(5, tileAddress + std::uint64_t(tile) * ConstantAlignment);
            commandList->SetComputeRootShaderResourceView(6, receiverTableAddress);
            commandList->Dispatch((count + 63) / 64, 1, 1);
        }

#if defined(ENGINE_HAS_VERIFICATION)
        // Diagnostic: each selected probe's proposal targets, copied before the next tile reuses the table.
        if(probes)
        {
            UAVBarrier(commandList);
            m_targets.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

            for(std::uint32_t probe = 0; probe < ProbeCount; ++probe)
            {
                if(probePixels[probe] >= start && probePixels[probe] < start + count)
                {
                    const std::uint64_t rowBytes = std::uint64_t(triangleCount) * TargetBytesPerTriangle;
                    commandList->CopyBufferRegion(m_probeTargetsReadback.Get(), probe * rowBytes, m_targets.GetResource(),
                        (probePixels[probe] - start) * rowBytes, rowBytes);
                }
            }

            m_targets.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
#endif
    }

    UAVBarrier(commandList);

#if defined(ENGINE_HAS_VERIFICATION)
    // Verification: corrupt one completed publication (or the latch) before the transaction validates it.
    if(tamperAddress)
    {
        insertion->specular->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_insertTamper.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, tamperAddress);
        commandList->Dispatch(1, 1, 1);
        UAVBarrier(commandList);
    }
#endif

    // Stage 3: publish to consumers.
    m_diffuse.Transition(commandList, ShaderRead);
    m_specular.Transition(commandList, ShaderRead);

#if defined(ENGINE_HAS_VERIFICATION)
    // P5D.2B whole-image certification audit: every certified pixel against exact raster/TLAS agreement, after the
    // estimator published its states. The audit buffer returns to its prior state (discard-safe).
    if(identityAuditAddress)
    {
        m_identityCertification.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, identityAuditAddress);
        commandList->SetComputeRootShaderResourceView(1, input.topLevel);
        commandList->Dispatch((input.width + 7) / 8, (input.height + 7) / 8, 1);
        UAVBarrier(commandList);
        m_identityAuditBuffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyBufferRegion(m_identityAuditReadback.Get(), 0, m_identityAuditBuffer.GetResource(), 0,
            IdentityAuditBytes);
        m_identityAuditBuffer.Transition(commandList, auditState);
    }
#endif

    // Transaction steps 3-5: validate the whole image, then insert all or nothing, before any denoiser reads.
    if(insertion)
    {
#if defined(ENGINE_HAS_VERIFICATION)
        if(captureInsertion)
        {
            RecordTextureReadback(commandList, device, *insertion->diffuse, m_insertionReadbacks[0]);
            RecordTextureReadback(commandList, device, *insertion->specular, m_insertionReadbacks[1]);

            if(insertion->rawDiffuseHitDistance && insertion->rawDiffuseHitDistance->GetResource())
            {
                RecordTextureReadback(commandList, device, *insertion->rawDiffuseHitDistance, m_insertionReadbacks[2]);
            }
            else
            {
                m_insertionReadbacks[2].buffer.Reset();
            }
        }
#endif

        insertion->diffuse->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        insertion->specular->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_insertValidate.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, insertAddress);
        commandList->Dispatch(viewGroupsX, viewGroupsY, 1);
        UAVBarrier(commandList);
        m_insertApply.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, insertAddress);
        commandList->Dispatch(viewGroupsX, viewGroupsY, 1);
        UAVBarrier(commandList);

#if defined(ENGINE_HAS_VERIFICATION)
        if(captureInsertion)
        {
            RecordTextureReadback(commandList, device, *insertion->diffuse, m_insertionReadbacks[3]);
            RecordTextureReadback(commandList, device, *insertion->specular, m_insertionReadbacks[4]);

            if(insertion->rawDiffuseHitDistance && insertion->rawDiffuseHitDistance->GetResource())
            {
                RecordTextureReadback(commandList, device, *insertion->rawDiffuseHitDistance, m_insertionReadbacks[5]);
            }
            else
            {
                m_insertionReadbacks[5].buffer.Reset();
            }

            RecordTextureReadback(commandList, device, m_diffuse, m_insertionReadbacks[6]);
            RecordTextureReadback(commandList, device, m_specular, m_insertionReadbacks[7]);
            m_insertionCaptured = true;
            m_insertionCaptureWidth = input.width;
            m_insertionCaptureHeight = input.height;
        }
#endif
    }

    // Copy the latch (estimator and insertion words) for completion-time evidence.
    m_faultLatch.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(measurement->readback.Get(), 0, m_faultLatch.GetResource(), 0, FaultLatchBytes);

#if defined(ENGINE_HAS_VERIFICATION)
    if(probes)
    {
        commandList->CopyBufferRegion(m_probeLatchReadback.Get(), 0, m_faultLatch.GetResource(), 0, FaultLatchBytes);

        for(Buffer* buffer : {&m_probeAttempts, &m_probeTuples, &m_probeOutputs})
        {
            buffer->Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        commandList->CopyBufferRegion(m_probeAttemptsReadback.Get(), 0, m_probeAttempts.GetResource(), 0, ProbeAttemptBytes);
        commandList->CopyBufferRegion(m_probeTuplesReadback.Get(), 0, m_probeTuples.GetResource(), 0, ProbeTupleBytes);
        commandList->CopyBufferRegion(m_probeOutputsReadback.Get(), 0, m_probeOutputs.GetResource(), 0, ProbeOutputBytes);

        for(Buffer* buffer : {&m_probeAttempts, &m_probeTuples, &m_probeOutputs})
        {
            buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        m_diagnosticCapture = {input.epoch, seed, attempts, input.triangles, input.totalArea,
            input.inverseCameraRelativeViewProjection, input.cameraPosition};
    }
#endif

    m_faultLatch.Transition(commandList, ShaderRead);
    profiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::PixelCausticEstimate);

    measurement->state = MeasurementState::Recorded;
    measurement->generation = generation;
    measurement->lifetime = context.GetSubmissionLifetime();
    measurement->fence = 0;
    measurement->epoch = input.epoch;
    measurement->insertion = insertion != nullptr;
    m_recordedThisFrame = true;
    m_reason = "recorded";
    return true;
}

bool PixelCausticPass::RecordView(ID3D12GraphicsCommandList* commandList, DescriptorHeap& heap,
    UploadBuffer& uploadBuffer, PixelCausticView view, RenderTarget& sceneColor, GpuTimestampProfiler& profiler,
    unsigned int frameSlot)
{
    if(!m_recordedThisFrame || view == PixelCausticView::None || !commandList || !sceneColor.GetResource() ||
        sceneColor.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT || sceneColor.GetWidth() != m_width)
    {
        return false;
    }

    ViewConstants constants{};
    constants.diffuseIndex = m_diffuse.GetDescriptorIndex();
    constants.specularIndex = m_specular.GetDescriptorIndex();
    constants.faultLatchIndex = m_faultLatch.GetDescriptorIndex();
    constants.outputIndex = m_present.GetUAVIndex();
    constants.resolution[0] = m_width;
    constants.resolution[1] = m_height;
    constants.view = static_cast<std::uint32_t>(view);
    const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(constants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(!address)
    {
        return false;
    }

    profiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::PixelCausticView);
    ID3D12DescriptorHeap* heaps[] = {heap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_view.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(sceneColor.GetResource(), m_present.GetResource());

    // As PathTraceReferencePass::CopyToSceneColor leaves it: tone mapping transitions scene colour itself.
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    profiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::PixelCausticView);
    return true;
}

void PixelCausticPass::CollectCompleted(const CommandContext& context)
{
    const auto completed = context.GetCompletedFenceValue();
    std::uint64_t newestFence = 0;

    for(auto& measurement : m_measurements)
    {
        if(measurement.state != MeasurementState::Submitted || measurement.lifetime != context.GetSubmissionLifetime() ||
            completed == UINT64_MAX || completed < measurement.fence)
        {
            continue;
        }

        const D3D12_RANGE readRange{0, FaultLatchBytes};
        const D3D12_RANGE noWrites{0, 0};
        void* mapped = nullptr;
        std::uint32_t words[8]{};

        if(SUCCEEDED(measurement.readback->Map(0, &readRange, &mapped)))
        {
            std::memcpy(words, mapped, sizeof(words));
            measurement.readback->Unmap(0, &noWrites);
        }
        else
        {
            words[0] = 0x80000000u;
        }

        // Only a latch carrying this recording's own epoch proves the GPU executed it.
        if(words[3] != measurement.epoch)
        {
            words[0] |= 0x80000000u;
            Log::Error("Pixel caustic completion did not carry its epoch " + std::to_string(measurement.epoch) + ".");
        }
        else if(m_completedFrames != UINT64_MAX)
        {
            ++m_completedFrames;
        }

        if(words[0] != 0 && m_faultedFrames != UINT64_MAX)
        {
            ++m_faultedFrames;
        }

        // The insertion words are meaningful only for a recording that carried the transaction.
        const bool insertionRefused = measurement.insertion && (words[0] != 0 || words[5] != 0);

        if(measurement.insertion)
        {
            if(insertionRefused && m_refusedInsertions != UINT64_MAX)
            {
                ++m_refusedInsertions;
            }
            else if(!insertionRefused && m_insertedFrames != UINT64_MAX)
            {
                ++m_insertedFrames;
            }
        }

        // Several recordings can complete together; the newest submission's result is the last one.
        if(measurement.fence >= newestFence)
        {
            newestFence = measurement.fence;
            m_lastCompletedFaults = words[0];
            m_lastCompletedEpoch = words[3];
            m_lastCompletedInsertion = {measurement.insertion, words[3], measurement.insertion ? words[5] : 0u,
                measurement.insertion ? words[6] : 0u, measurement.insertion ? words[7] : 0u};
        }

        measurement.state = MeasurementState::Empty;
    }
}

void PixelCausticPass::ArmSubmission(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Recorded)
        {
            measurement.state = MeasurementState::Uncertain;
        }
    }
}

void PixelCausticPass::ConfirmSubmission(std::uint64_t generation, std::uint64_t fence, std::uint64_t lifetime)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Uncertain &&
            measurement.lifetime == lifetime && fence && fence != UINT64_MAX)
        {
            measurement.fence = fence;
            measurement.state = MeasurementState::Submitted;
        }
    }
}

void PixelCausticPass::ProvenNotSubmitted(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Uncertain)
        {
            measurement.state = MeasurementState::Recorded;
        }
    }
}

void PixelCausticPass::DiscardRecording(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Recorded)
        {
            measurement.state = MeasurementState::Empty;
            m_recordedThisFrame = false;
            m_reason = "pixel caustic recording was proven discarded";
            RestoreStableStates();
        }
    }
}

void PixelCausticPass::RestoreStableStates()
{
    // The discarded list's transitions never executed: return CPU tracking to the states it began from.
    m_diffuse.RestoreStatesAfterAbort({m_discardStates[0]});
    m_specular.RestoreStatesAfterAbort({m_discardStates[1]});
    m_targets.RestoreStateAfterAbort(m_discardStates[2]);
    m_scratch.RestoreStateAfterAbort(m_discardStates[3]);
    m_faultLatch.RestoreStateAfterAbort(m_discardStates[4]);
}

std::uint64_t PixelCausticPass::GetResourceBytes() const noexcept
{
    return m_diffuse.GetResource() ? m_resourceBytes : 0;
}

void PixelCausticPass::ReleaseResources()
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.state != MeasurementState::Empty)
        {
            Log::Error("Pixel caustic resource release requires completed or proven-discarded submissions.");
            std::abort();
        }

        measurement.readback.Reset();
    }

    m_diffuse.Shutdown();
    m_specular.Shutdown();
    m_present.Shutdown();
    m_targets.Shutdown();
    m_scratch.Shutdown();
    m_faultLatch.Shutdown();
    m_width = 0;
    m_height = 0;
    m_targetTriangles = 0;
    m_resourceBytes = 0;
    m_recordedThisFrame = false;

#if defined(ENGINE_HAS_VERIFICATION)
    m_probeAttempts.Shutdown();
    m_probeTuples.Shutdown();
    m_probeOutputs.Shutdown();
    m_probeAttemptsReadback.Reset();
    m_probeTuplesReadback.Reset();
    m_probeTargetsReadback.Reset();
    m_probeOutputsReadback.Reset();
    m_probeLatchReadback.Reset();
    m_probeTriangles = 0;

    for(auto& readback : m_insertionReadbacks)
    {
        readback.buffer.Reset();
    }

    m_insertionCaptured = false;
#endif
}
