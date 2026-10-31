#include "CausticPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// The contract headers use unparenthesized std::max/min, which the Windows macros would rewrite.
#pragma push_macro("max")
#pragma push_macro("min")
#undef max
#undef min
#include "Renderer/Caustics/CausticGeometryCertificate.h"
#include "Renderer/Caustics/CausticProposalContract.h"
#pragma pop_macro("min")
#pragma pop_macro("max")

namespace
{
    // Must match CausticTrace.hlsl CausticTraceConstants.
    struct alignas(16) TraceConstants
    {
        Matrix4 viewProjection;
        Matrix4 inverseViewProjection;
        float cameraPosition[3];
        std::uint32_t recordsIndex;
        float capAxis[3];
        float cosineHalfAngle;
        float inverseSolidAngle;
        float sphereProbability;
        std::uint32_t attempts;
        std::uint32_t seed;
        std::uint32_t lightOffset;
        std::uint32_t reflectorDense;
        std::uint32_t reflectorNative;
        std::uint32_t receiverDense;
        std::uint32_t receiverNative;
        std::uint32_t materialTable;
        std::uint32_t materialAverageTable;
        std::uint32_t materialCosineMapping;
        std::uint32_t resolution[2];
        std::uint32_t triangleCount;
        std::uint32_t padding;
        float cameraForward[3];
        float jacobianScale;
    };

    static_assert(sizeof(TraceConstants) == 240);

    // Must match CausticAccumulate.hlsl CausticAccumulateConstants.
    struct AccumulateConstants
    {
        std::uint32_t attempts;
        std::uint32_t pixels;
        std::uint32_t precisionBits;
        std::uint32_t resetHistory;
        std::uint32_t recordsIndex;
        std::uint32_t sumsIndex;
        std::uint32_t scalesIndex;
        std::uint32_t historyIndex;
        std::uint32_t controlIndex;
        std::uint32_t padding[3];
    };

    static_assert(sizeof(AccumulateConstants) == 48);

    // Must match CausticComposite.hlsl CausticCompositeConstants.
    struct CompositeConstants
    {
        std::uint32_t resolution[2];
        std::uint32_t outputIndex;
        std::uint32_t historyIndex;
        std::uint32_t controlIndex;
        std::uint32_t causticOnly;
        std::uint32_t padding[2];
    };

    static_assert(sizeof(CompositeConstants) == 32);

    RootParameterDescription RootParameter(RootParameterType type, unsigned int shaderRegister)
    {
        RootParameterDescription parameter{};
        parameter.type = type;
        parameter.shaderRegister = shaderRegister;
        parameter.visibility = D3D12_SHADER_VISIBILITY_ALL;
        return parameter;
    }

    void UavBarrier(ID3D12GraphicsCommandList* commandList)
    {
        // All caustic buffers stay in UAV state; one global barrier orders every pass.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    }

    std::uint32_t Groups(std::uint64_t count, std::uint32_t size)
    {
        return static_cast<std::uint32_t>((count + size - 1) / size);
    }
}

bool CausticPass::BuildEmissionProposal(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices,
    std::uint32_t primitiveCount, const Matrix4& model, const std::array<float, 3>& light, CausticFrameInput& input)
{
    using namespace CausticContracts;

    if(!primitiveCount || std::size_t(primitiveCount) * 3 > indices.size())
    {
        return false;
    }

    // World corners in binary64 (Matrix4 stores translation in column 3), then a box-centred sphere
    // whose radius is the largest corner distance, padded for the GPU's binary32 instance transform.
    const auto world = [&](const Vertex& vertex)
    {
        std::array<double, 3> point{};

        for(unsigned int row = 0; row < 3; ++row)
        {
            point[row] = double(model.m[row][3]);

            for(unsigned int column = 0; column < 3; ++column)
            {
                point[row] += double(model.m[row][column]) * double(vertex.position[column]);
            }
        }

        return point;
    };
    std::array<double, 3> minimum{HUGE_VAL, HUGE_VAL, HUGE_VAL};
    std::array<double, 3> maximum{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};

    for(std::size_t element = 0; element < std::size_t(primitiveCount) * 3; ++element)
    {
        if(indices[element] >= vertices.size())
        {
            return false;
        }

        const auto point = world(vertices[indices[element]]);

        for(unsigned int axis = 0; axis < 3; ++axis)
        {
            if(!std::isfinite(point[axis]))
            {
                return false;
            }

            minimum[axis] = (std::min)(minimum[axis], point[axis]);
            maximum[axis] = (std::max)(maximum[axis], point[axis]);
        }
    }

    const std::array<double, 3> center{(minimum[0] + maximum[0]) * 0.5, (minimum[1] + maximum[1]) * 0.5,
        (minimum[2] + maximum[2]) * 0.5};
    double radius = 0.0;

    for(std::size_t element = 0; element < std::size_t(primitiveCount) * 3; ++element)
    {
        const auto point = world(vertices[indices[element]]);
        radius = (std::max)(radius, std::sqrt((point[0] - center[0]) * (point[0] - center[0]) +
            (point[1] - center[1]) * (point[1] - center[1]) + (point[2] - center[2]) * (point[2] - center[2])));
    }

    radius += 1.0e-5 * (radius + std::abs(center[0]) + std::abs(center[1]) + std::abs(center[2]));

    // The bound and the current proposal belong to the same publication by construction here (one
    // snapshot); the contract still requires a valid, matching association.
    const auto identity = [](std::uint64_t value)
    {
        return CausticIdentity{static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)};
    };
    const CausticPublication publication{identity(input.identity[0]), identity(input.identity[2]),
        identity(input.identity[3]), identity(input.identity[4]), identity(input.identity[1]),
        identity(input.identity[1]), identity(input.identity[1])};
    Proposal proposal;

    if(!BuildProposal({publication, {center[0], center[1], center[2]}, radius, true},
        {light[0], light[1], light[2]}, publication, 0.25, proposal))
    {
        return false;
    }

    input.capAxis = {float(proposal.axis.x), float(proposal.axis.y), float(proposal.axis.z)};
    input.cosineHalfAngle = float(proposal.cosineHalfAngle);
    input.inverseSolidAngle = float(proposal.inverseSolidAngle);
    input.sphereProbability = float(proposal.sphereProbability);
    return true;
}

bool CausticPass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized || m_initializationFailed)
    {
        return m_initialized;
    }

    ID3D12Device* device = graphicsDevice.GetDevice();
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    m_doublePrecision = device && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.DoublePrecisionFloatShaderOps != FALSE;

    const auto shader = [&](const char* path, const char* entryPoint)
    {
        ShaderDescription description{};
        description.path = path;
        description.entryPoint = entryPoint;
        description.stage = ShaderStage::Compute;
        return shaderLibrary.Load(description);
    };
    const auto create = [&](ComputePipeline& pipeline, const char* path, const char* entryPoint, bool trace)
    {
        ComputePipelineDescription description{};
        description.computeShader = shader(path, entryPoint);
        description.rootParameters = {RootParameter(RootParameterType::ConstantBuffer, 0)};

        if(trace)
        {
            for(unsigned int index = 0; index < 7; ++index)
            {
                description.rootParameters.push_back(RootParameter(RootParameterType::ShaderResource, index));
            }
        }

        return description.computeShader && pipeline.Initialize(device, description);
    };

    // The accumulator's FP64 decode is part of its numerical contract; without FP64 caustics stay off.
    m_initialized = m_doublePrecision && create(m_trace, "CausticTrace.hlsl", "CSTrace", true) &&
        create(m_clear, "CausticAccumulate.hlsl", "CSClear", false) &&
        create(m_maximum, "CausticAccumulate.hlsl", "CSMaximum", false) &&
        create(m_initialize, "CausticAccumulate.hlsl", "CSInitialize", false) &&
        create(m_replay, "CausticAccumulate.hlsl", "CSReplay", false) &&
        create(m_decode, "CausticAccumulate.hlsl", "CSDecode", false) &&
        create(m_finish, "CausticAccumulate.hlsl", "CSFinish", false) &&
        create(m_invalidate, "CausticAccumulate.hlsl", "CSInvalidate", false) &&
        create(m_seal, "CausticAccumulate.hlsl", "CSSeal", false) &&
        create(m_validateComposite, "CausticComposite.hlsl", "CSValidateComposite", false) &&
        create(m_composite, "CausticComposite.hlsl", "CSComposite", false);

    if(!m_initialized)
    {
        Log::Warning(m_doublePrecision ? "Dedicated caustics are unavailable: pipeline creation failed." :
            "Dedicated caustics are unavailable: FP64 shader operations are unsupported.");
        Shutdown();
        m_initializationFailed = true;
        m_status = Status::Unavailable;
        return false;
    }

    m_status = Status::Refused;
    return true;
}

void CausticPass::Shutdown()
{
    ReleaseResources();

    for(ComputePipeline* pipeline : {&m_trace, &m_clear, &m_maximum, &m_initialize, &m_replay, &m_decode,
        &m_finish, &m_invalidate, &m_seal, &m_validateComposite, &m_composite})
    {
        pipeline->Shutdown();
    }

    m_initialized = false;
}

void CausticPass::ReleaseResources()
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.state != MeasurementState::Empty)
        {
            Log::Error("Caustic resource release requires completed or proven-discarded submissions.");
            std::abort();
        }

        measurement.readback.Reset();
    }

    m_activeMeasurement = UINT_MAX;
    m_records.Shutdown();
    m_sums.Shutdown();
    m_scales.Shutdown();
    m_history.Shutdown();
    m_control.Shutdown();
    m_present.Shutdown();
    m_width = 0;
    m_height = 0;
    m_presentFormat = DXGI_FORMAT_UNKNOWN;
    m_resourceBytes = 0;
    ResetHistory();
}

void CausticPass::ResetHistory()
{
    m_hasIdentity = false;
    m_resetPending = true;
    m_historyFrames = 0;
}

bool CausticPass::Refuse(std::string reason)
{
    m_status = m_faulted ? Status::Faulted : m_initialized ? Status::Refused : Status::Unavailable;
    m_reason = std::move(reason);
    return false;
}

bool CausticPass::EnsureResources(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap,
    std::uint32_t width, std::uint32_t height, DXGI_FORMAT outputFormat)
{
    if(m_width == width && m_height == height && m_presentFormat == outputFormat && m_records.GetResource())
    {
        return true;
    }

    // Reallocation happens only at drained boundaries (ReleaseResources); a size change otherwise refuses.
    if(m_records.GetResource())
    {
        return Refuse("caustic resources are sized for another resolution until the next drained resize");
    }

    const std::uint64_t pixels = std::uint64_t(width) * height;
    const std::uint64_t sizes[5] = {std::uint64_t(AttemptsPerFrame) * 16, pixels * 24, pixels * 4, pixels * 16, 16};
    std::uint64_t total = 0;

    // Actual allocation sizes, including the present copy, against the caustic-owned budget.
    for(const auto size : sizes)
    {
        const D3D12_RESOURCE_DESC description{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
            {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const auto allocated = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;

        if(allocated > (256ull << 20))
        {
            return Refuse("caustic buffer exceeds the 256 MiB per-resource bound");
        }

        total += allocated;
    }

    D3D12_RESOURCE_DESC present{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, outputFormat, {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    total += device->GetResourceAllocationInfo(0, 1, &present).SizeInBytes;

    if(total > MaximumResourceBytes)
    {
        return Refuse("caustic resources would exceed the 512 MiB budget at this resolution");
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC readbackDescription{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16, 1, 1, 1,
        DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    total += m_measurements.size() * device->GetResourceAllocationInfo(0, 1, &readbackDescription).SizeInBytes;

    if(total > MaximumResourceBytes)
    {
        return Refuse("caustic readbacks would exceed the resource budget");
    }

    for(auto& measurement : m_measurements)
    {
        if(FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&measurement.readback))))
        {
            ReleaseResources();
            return Refuse("caustic readback allocation failed");
        }
    }

    const std::uint32_t zero[4]{};
    bool created = m_records.InitializeGPUWritable(device, static_cast<size_t>(sizes[0])) && m_records.CreateRawUAV(device, heap) &&
        m_sums.InitializeGPUWritable(device, static_cast<size_t>(sizes[1])) && m_sums.CreateRawUAV(device, heap) &&
        m_scales.InitializeGPUWritable(device, static_cast<size_t>(sizes[2])) && m_scales.CreateRawUAV(device, heap) &&
        m_history.InitializeGPUWritable(device, static_cast<size_t>(sizes[3])) && m_history.CreateRawUAV(device, heap) &&
        m_control.InitializeGPUWritableSeeded(device, uploader, zero, sizeof(zero), sizeof(zero), D3D12_RESOURCE_STATE_COMMON) &&
        m_control.CreateRawUAV(device, heap) &&
        m_present.Initialize(device, width, height, 1, 1, outputFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && m_present.CreateUAV(device, heap);

    if(!created)
    {
        ReleaseResources();
        return Refuse("caustic resource allocation failed");
    }

    m_width = width;
    m_height = height;
    m_presentFormat = outputFormat;
    m_resourceBytes = total;
    ResetHistory();
    return true;
}

bool CausticPass::Record(ID3D12GraphicsCommandList* commandList, ID3D12Device* device, ResourceUploader& uploader,
    DescriptorHeap& heap, UploadBuffer& uploadBuffer, const CausticFrameInput& input, std::uint32_t seed,
    const CommandContext& context, std::uint64_t generation)
{
    CollectCompleted(context);

    if(m_faulted)
    {
        return Refuse("dedicated caustic epoch is faulted; explicit acknowledgement and drain required");
    }

    if(!m_initialized)
    {
        return Refuse("caustic pipelines are unavailable");
    }

    if(!commandList || !input.width || !input.height || input.width > MaximumDimension || input.height > MaximumDimension)
    {
        return Refuse("unsupported caustic resolution");
    }

    for(const auto& row : input.viewProjection.m)
    {
        for(float value : row)
        {
            if(!std::isfinite(value))
            {
                return Refuse("nonfinite caustic camera matrix");
            }
        }
    }

    for(const auto& row : input.inverseViewProjection.m)
    {
        for(float value : row)
        {
            if(!std::isfinite(value))
            {
                return Refuse("nonfinite caustic inverse camera matrix");
            }
        }
    }

    if(!std::isfinite(input.jacobianScale) || !(input.jacobianScale > 0.0f) ||
        !input.topLevel || !input.materialTable || !input.lightTable || !input.instanceData ||
        !input.slotMaterials || !input.vertices || !input.indices)
    {
        return Refuse("invalid caustic camera measure or resource binding");
    }

    // The present copy mirrors the Reference output format; it is created with the history.
    if(input.outputFormat != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !EnsureResources(device, uploader, heap, input.width, input.height, input.outputFormat))
    {
        return false;
    }

    // Any change of accumulated meaning restarts the caustic history (only the caustic history).
    const bool sameIdentity = m_hasIdentity && m_identity == input.identity && m_lightRecord == input.lightRecord &&
        std::memcmp(&m_viewProjection, &input.viewProjection, sizeof(Matrix4)) == 0;
    const bool reset = m_resetPending || !sameIdentity;

    TraceConstants trace{};
    trace.viewProjection = input.viewProjection;
    trace.inverseViewProjection = input.inverseViewProjection;
    trace.recordsIndex = m_records.GetUAVDescriptorIndex();
    trace.cosineHalfAngle = input.cosineHalfAngle;
    trace.inverseSolidAngle = input.inverseSolidAngle;
    trace.sphereProbability = input.sphereProbability;
    trace.attempts = AttemptsPerFrame;
    trace.seed = seed;
    trace.lightOffset = input.lightOffset;
    trace.reflectorDense = input.reflectorDense;
    trace.reflectorNative = input.reflectorNative;
    trace.receiverDense = input.receiverDense;
    trace.receiverNative = input.receiverNative;
    trace.materialTable = input.materialEnergyTable;
    trace.materialAverageTable = input.materialAverageTable;
    trace.materialCosineMapping = input.materialCosineMapping;
    trace.resolution[0] = input.width;
    trace.resolution[1] = input.height;
    trace.jacobianScale = input.jacobianScale;
    trace.triangleCount = input.triangleCount;

    for(unsigned int axis = 0; axis < 3; ++axis)
    {
        trace.cameraPosition[axis] = input.cameraPosition[axis];
        trace.cameraForward[axis] = input.cameraForward[axis];
        trace.capAxis[axis] = input.capAxis[axis];
    }

    AccumulateConstants accumulate{};
    accumulate.attempts = AttemptsPerFrame;
    accumulate.pixels = input.width * input.height;
    accumulate.precisionBits = PrecisionBits;
    accumulate.resetHistory = reset ? 1u : 0u;
    accumulate.recordsIndex = m_records.GetUAVDescriptorIndex();
    accumulate.sumsIndex = m_sums.GetUAVDescriptorIndex();
    accumulate.scalesIndex = m_scales.GetUAVDescriptorIndex();
    accumulate.historyIndex = m_history.GetUAVDescriptorIndex();
    accumulate.controlIndex = m_control.GetUAVDescriptorIndex();

    const auto traceAddress = uploadBuffer.Allocate(&trace, sizeof(trace), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const auto accumulateAddress = uploadBuffer.Allocate(&accumulate, sizeof(accumulate), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(!traceAddress || !accumulateAddress)
    {
        return Refuse("caustic constants could not be allocated");
    }

    if(!PrepareMeasurement(context, generation, reset))
    {
        return false;
    }

    m_identity = input.identity;
    m_lightRecord = input.lightRecord;
    m_viewProjection = input.viewProjection;
    m_hasIdentity = true;
    m_resetPending = false;
    m_historyFrames = reset ? 1 : m_historyFrames + 1;
    m_accumulateAddress = accumulateAddress;

    ID3D12DescriptorHeap* heaps[] = {heap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);

    for(Buffer* buffer : {&m_records, &m_sums, &m_scales, &m_history, &m_control})
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if(m_profiler)
    {
        m_profiler->BeginRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticTrace);
    }

    // Light-first tracing: one deposit record per attempt, including misses.
    m_trace.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, traceAddress);
    const D3D12_GPU_VIRTUAL_ADDRESS resources[7] = {input.topLevel, input.materialTable, input.lightTable,
        input.instanceData, input.slotMaterials, input.vertices, input.indices};

    for(unsigned int index = 0; index < 7; ++index)
    {
        commandList->SetComputeRootShaderResourceView(1 + index, resources[index]);
    }

    commandList->Dispatch(Groups(AttemptsPerFrame, 64), 1, 1);
    UavBarrier(commandList);

    if(m_profiler)
    {
        m_profiler->EndRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticTrace);
        m_profiler->BeginRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticAccumulate);
    }

    // Adaptive RGB64: clear, per-pixel maximum, scale, exact replay, decode/fold, frame accounting.
    const std::uint64_t pixels = accumulate.pixels;
    const std::pair<ComputePipeline*, std::uint32_t> passes[7] = {{&m_clear, Groups(pixels > 64 ? pixels : 64, 256)},
        {&m_maximum, Groups(AttemptsPerFrame, 256)}, {&m_initialize, Groups(pixels, 256)},
        {&m_replay, Groups(AttemptsPerFrame, 256)}, {&m_decode, Groups(pixels, 256)},
        {&m_invalidate, Groups(pixels, 256)}, {&m_finish, 1}};

    const GpuTimestampProfiler::Region regions[7] = {GpuTimestampProfiler::Region::CausticClear,
        GpuTimestampProfiler::Region::CausticMaximum, GpuTimestampProfiler::Region::CausticInitialize,
        GpuTimestampProfiler::Region::CausticReplay, GpuTimestampProfiler::Region::CausticDecode,
        GpuTimestampProfiler::Region::CausticInvalidate, GpuTimestampProfiler::Region::CausticFinish};
    unsigned int passIndex = 0;

    for(const auto& [pipeline, groups] : passes)
    {
        if(m_profiler)
        {
            m_profiler->BeginRegion(commandList, m_frameSlot, regions[passIndex]);
        }
        pipeline->Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, accumulateAddress);
        commandList->Dispatch(groups, 1, 1);
        UavBarrier(commandList);

        if(m_profiler)
        {
            m_profiler->EndRegion(commandList, m_frameSlot, regions[passIndex]);
        }

        ++passIndex;
    }

    CaptureResult(commandList);

    for(Buffer* buffer : {&m_records, &m_sums, &m_scales, &m_history})
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_COMMON);
    }

    if(m_profiler)
    {
        m_profiler->EndRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticAccumulate);
    }

    ++m_recordedFrames;
    m_status = Status::Recorded;
    m_reason.clear();
    return true;
}

bool CausticPass::Present(ID3D12GraphicsCommandList* commandList, DescriptorHeap& heap, UploadBuffer& uploadBuffer,
    Texture& referenceOutput, RenderTarget& sceneColor, bool causticOnly)
{
    if(m_status != Status::Recorded || !m_present.GetResource() ||
        referenceOutput.GetResource()->GetDesc().Format != m_presentFormat)
    {
        return false;
    }

    CompositeConstants composite{};
    composite.resolution[0] = m_width;
    composite.resolution[1] = m_height;
    composite.outputIndex = m_present.GetUAVIndex();
    composite.historyIndex = m_history.GetUAVDescriptorIndex();
    composite.controlIndex = m_control.GetUAVDescriptorIndex();
    composite.causticOnly = causticOnly ? 1u : 0u;
    const auto address = uploadBuffer.Allocate(&composite, sizeof(composite), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(!address)
    {
        return false;
    }

    if(m_profiler)
    {
        m_profiler->BeginRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticComposite);
    }

    // Reference output -> present copy (the output stays untouched), add, -> scene colour.
    referenceOutput.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(m_present.GetResource(), referenceOutput.GetResource());
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    m_history.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_control.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_validateComposite.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch(Groups(m_width, 8), Groups(m_height, 8), 1);
    UavBarrier(commandList);
    m_invalidate.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, m_accumulateAddress);
    commandList->Dispatch(Groups(std::uint64_t(m_width) * m_height, 256), 1, 1);
    UavBarrier(commandList);
    m_seal.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, m_accumulateAddress);
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList);
    m_composite.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch(Groups(m_width, 8), Groups(m_height, 8), 1);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(sceneColor.GetResource(), m_present.GetResource());
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_present.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    referenceOutput.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CaptureResult(commandList);
    m_history.Transition(commandList, D3D12_RESOURCE_STATE_COMMON);

    if(m_profiler)
    {
        m_profiler->EndRegion(commandList, m_frameSlot, GpuTimestampProfiler::Region::CausticComposite);
    }

    return true;
}

void CausticPass::CollectCompleted(const CommandContext& context)
{
    const auto completed = context.GetCompletedFenceValue();

    for(auto& measurement : m_measurements)
    {
        if(measurement.state != MeasurementState::Submitted ||
            measurement.lifetime != context.GetSubmissionLifetime() || completed == UINT64_MAX ||
            completed < measurement.fence)
        {
            continue;
        }

        const D3D12_RANGE readRange{0, 16};
        const D3D12_RANGE noWrites{0, 0};
        void* mapped = nullptr;

        if(FAILED(measurement.readback->Map(0, &readRange, &mapped)))
        {
            m_faulted = true;
            m_lastFault = 0x80000000u;
            m_status = Status::Faulted;
            m_reason = "completed caustic result readback failed; epoch quarantined";
            Log::Error("Completed caustic result could not be read; epoch quarantined.");
            continue;
        }

        std::uint32_t words[4]{};
        std::memcpy(words, mapped, sizeof(words));
        measurement.readback->Unmap(0, &noWrites);
        if(m_completedMeasurements != UINT64_MAX)
        {
            ++m_completedMeasurements;
        }

        if(words[0] != 0)
        {
            m_faulted = true;
            m_lastFault = words[0];
            m_status = Status::Faulted;
            m_reason = "completed caustic epoch invalidated by numerical fault flags " + std::to_string(words[0]);

            if(m_faultObservations != UINT64_MAX)
            {
                ++m_faultObservations;
            }
            Log::Error("Dedicated caustic numerical fault: epoch=" + std::to_string(measurement.epoch) +
                " generation=" + std::to_string(measurement.generation) +
                " fence=" + std::to_string(measurement.fence) + " flags=" + std::to_string(words[0]));
        }

        measurement.state = MeasurementState::Empty;
    }
}

bool CausticPass::AcknowledgeFaultAndReset(const CommandContext& context)
{
    CollectCompleted(context);

    for(const auto& measurement : m_measurements)
    {
        if(measurement.state != MeasurementState::Empty)
        {
            return false;
        }
    }

    if(!m_faulted)
    {
        return false;
    }

    // CPU evidence survives reset and resource replacement. Only the new GPU
    // epoch clears its latch; no faulted history is ever resumed.
    m_faulted = false;
    m_status = Status::Refused;
    m_reason = "fault acknowledged; a new dedicated epoch is required";
    ResetHistory();
    return true;
}

bool CausticPass::PrepareMeasurement(const CommandContext& context, std::uint64_t generation, bool reset)
{
    if(!generation || !context.GetSubmissionLifetime() || m_faulted)
    {
        return Refuse("invalid caustic recording provenance or faulted epoch");
    }

    unsigned int available = UINT_MAX;

    for(unsigned int index = 0; index < m_measurements.size(); ++index)
    {
        const auto& measurement = m_measurements[index];

        if(measurement.state == MeasurementState::Empty)
        {
            available = index;
        }
        else if(reset || measurement.state == MeasurementState::Uncertain ||
            measurement.generation == generation)
        {
            return Refuse("caustic epoch reset or reuse requires resolved prior recordings");
        }
    }

    if(available == UINT_MAX || (reset && m_epoch == UINT64_MAX))
    {
        return Refuse("bounded caustic result capacity or epoch identity exhausted");
    }

    if(reset)
    {
        ++m_epoch;
    }

    auto& measurement = m_measurements[available];
    measurement.state = MeasurementState::Recorded;
    measurement.generation = generation;
    measurement.lifetime = context.GetSubmissionLifetime();
    measurement.fence = 0;
    measurement.epoch = m_epoch;
    m_activeMeasurement = available;
    return true;
}

void CausticPass::CaptureResult(ID3D12GraphicsCommandList* commandList)
{
    m_control.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(m_measurements[m_activeMeasurement].readback.Get(), 0,
        m_control.GetResource(), 0, 16);
    m_control.Transition(commandList, D3D12_RESOURCE_STATE_COMMON);
}

void CausticPass::ArmSubmission(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Recorded)
        {
            measurement.state = MeasurementState::Uncertain;
        }
    }
}

void CausticPass::ConfirmSubmission(std::uint64_t generation, std::uint64_t fence, std::uint64_t lifetime)
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

void CausticPass::ProvenNotSubmitted(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Uncertain)
        {
            measurement.state = MeasurementState::Recorded;
        }
    }
}

void CausticPass::DiscardRecording(std::uint64_t generation)
{
    for(auto& measurement : m_measurements)
    {
        if(measurement.generation == generation && measurement.state == MeasurementState::Recorded)
        {
            measurement.state = MeasurementState::Empty;
            m_status = Status::Refused;
            m_reason = "caustic recording was proven discarded";
            // Resource states return to the stable boundary of a completed pass.
            for(Buffer* buffer : {&m_records, &m_sums, &m_scales, &m_history, &m_control})
            {
                buffer->RestoreStateAfterAbort(D3D12_RESOURCE_STATE_COMMON);
            }
            ResetHistory();
        }
    }
}
