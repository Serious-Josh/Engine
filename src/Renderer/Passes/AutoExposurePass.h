#pragma once

#include "Core/Log.h"
#include "Renderer/Buffer.h"
#include "Renderer/CommandContext.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"
#include "Scene/CameraExposure.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <limits>
#include <string>

// Physical Lighting & Exposure, Checkpoint 2: the camera exposure of each frame (AutoExposure.hlsl). Meters the
// render-extent HDR scene colour before DLSS and tone mapping, adapts on the GPU and leaves one state buffer that the
// tone mapper and the DLSS exposure texture both read, so every consumer applies the same multiplier exactly once.
// Nothing upstream (lighting, accumulation, SHARC, denoisers) reads it. A small readback ring reports the state to
// the CPU a few frames late, without waits. Header-only so that it needs no build-system change.
class AutoExposurePass
{
public:
    // The GPU state as last read back (a few frames old).
    struct Readback
    {
        bool valid = false;
        float effectiveEV100 = 0.0f;
        float multiplier = 1.0f;
        float targetEV100 = 0.0f;
        float meteredEV100 = 0.0f;
        std::uint32_t validPixels = 0;
        std::uint32_t invalidPixels = 0;
    };

    struct Settings
    {
        CameraExposure exposure;
        float referenceEV100 = 0.0f;
        float deltaSeconds = 0.0f;
        // Snap to the target this frame (first frame, camera cut, scene load).
        bool reset = false;
    };

    static constexpr std::uint32_t StateBytes = 32;
    static constexpr std::uint32_t HistogramBytes = 257 * 4;

    const std::string& FailureReason() const { return m_failure; }

    bool Ensure(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& heap)
    {
        m_failure.clear();

        if(m_ready)
        {
            return true;
        }

        const auto pipeline = [&](const char* entryPoint, ComputePipeline& target)
        {
            ShaderDescription shaderDescription{};
            shaderDescription.path = "AutoExposure.hlsl";
            shaderDescription.entryPoint = entryPoint;
            shaderDescription.stage = ShaderStage::Compute;
            Shader* shader = shaderLibrary.Load(shaderDescription);

            if(!shader)
            {
                return false;
            }

            ComputePipelineDescription description{};
            description.computeShader = shader;
            RootParameterDescription constants{};
            constants.type = RootParameterType::ConstantBuffer;
            constants.shaderRegister = 0;
            constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
            description.rootParameters.push_back(constants);
            return target.Initialize(device, description);
        };

        if(!pipeline("CSClear", m_clear) || !pipeline("CSHistogram", m_histogramPipeline) || !pipeline("CSAdapt", m_adapt))
        {
            m_failure = "Failed to create the exposure pipelines.";
            Log::Error(m_failure);
            return false;
        }

        const bool created = m_histogram.InitializeGPUWritable(device, HistogramBytes) && m_histogram.CreateRawUAV(device, heap) &&
            m_state.InitializeGPUWritable(device, StateBytes) && m_state.CreateRawUAV(device, heap) &&
            m_state.CreateRawSRV(device, heap);

        for(auto& slot : m_slots)
        {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = StateBytes;
            description.Height = description.DepthOrArraySize = description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if(!created || FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback))))
            {
                m_failure = "Failed to create the exposure buffers.";
                Log::Error(m_failure);
                Shutdown();
                return false;
            }
        }

        m_histogram.GetResource()->SetName(L"Exposure Histogram");
        m_state.GetResource()->SetName(L"Exposure State");
        m_needsClear = true;
        m_ready = true;
        return true;
    }

    // Leaves the state readable by pixel and compute shaders (tone mapping, DLSS input preparation).
    bool Record(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, RenderTarget& sceneColor,
        const Settings& settings, unsigned int frameIndex)
    {
        m_failure.clear();
        struct Constants
        {
            std::uint32_t sceneColorIndex;
            std::uint32_t histogramIndex;
            std::uint32_t stateIndex;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t mode;
            std::uint32_t flags;
            float referenceEV100;
            float manualEffectiveEV100;
            float compensation;
            float meteringMinEV100;
            float meteringMaxEV100;
            float lowPercentile;
            float highPercentile;
            float brighteningSpeed;
            float darkeningSpeed;
            float deltaSeconds;
            float padding[3];
        };

        const CameraExposure& exposure = settings.exposure;
        Constants constants{};
        constants.sceneColorIndex = sceneColor.GetSRVIndex();
        constants.histogramIndex = m_histogram.GetUAVDescriptorIndex();
        constants.stateIndex = m_state.GetUAVDescriptorIndex();
        constants.width = sceneColor.GetWidth();
        constants.height = sceneColor.GetHeight();
        constants.mode = static_cast<std::uint32_t>(exposure.mode);
        constants.flags = (settings.reset ? 1u : 0u) | (exposure.locked ? 2u : 0u);
        constants.referenceEV100 = settings.referenceEV100;
        constants.manualEffectiveEV100 = float(Photometry::ManualEV100(exposure) - double(exposure.compensation));
        constants.compensation = exposure.compensation;
        constants.meteringMinEV100 = exposure.meteringMinEV100;
        constants.meteringMaxEV100 = exposure.meteringMaxEV100;
        constants.lowPercentile = exposure.lowPercentile;
        constants.highPercentile = exposure.highPercentile;
        constants.brighteningSpeed = exposure.brighteningSpeed;
        constants.darkeningSpeed = exposure.darkeningSpeed;
        constants.deltaSeconds = settings.deltaSeconds;
        const D3D12_GPU_VIRTUAL_ADDRESS address =
            uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            m_failure = "Failed to allocate exposure constants.";
            Log::Error(m_failure);
            return false;
        }

        sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_histogram.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_state.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto uavBarrier = [&](Buffer& buffer)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = buffer.GetResource();
            commandList->ResourceBarrier(1, &barrier);
        };

        if(m_needsClear)
        {
            m_clear.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, address);
            commandList->Dispatch(1, 1, 1);
            uavBarrier(m_histogram);
            uavBarrier(m_state);
            m_needsClear = false;
        }

        m_histogramPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch((constants.width + 15u) / 16u, (constants.height + 15u) / 16u, 1);
        uavBarrier(m_histogram);

        m_adapt.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch(1, 1, 1);
        uavBarrier(m_state);

        Slot& slot = m_slots[frameIndex % m_slots.size()];

        if(slot.fence == 0)
        {
            m_state.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->CopyBufferRegion(slot.readback, 0, m_state.GetResource(), 0, StateBytes);
            slot.recorded = true;
        }

        m_state.Transition(commandList,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

    // The state buffer itself (verification readback); left readable by pixel and compute shaders after Record.
    Buffer& GetStateBuffer() noexcept { return m_state; }

    // Raw SRV of the state (word 1: the multiplier).
    unsigned int GetStateSRVIndex() const noexcept { return m_state.GetDescriptorIndex(); }
    bool IsReady() const noexcept { return m_ready; }

    // After the frame's submission: the fence that marks its copy complete.
    void MarkSubmitted(unsigned int frameIndex, std::uint64_t fence)
    {
        Slot& slot = m_slots[frameIndex % m_slots.size()];

        if(slot.recorded)
        {
            slot.fence = fence;
            slot.recorded = false;
        }
    }

    // A recorded copy that was never submitted (an aborted frame) is dropped.
    void DiscardUnsubmitted()
    {
        for(auto& slot : m_slots)
        {
            slot.recorded = false;
        }
    }

    void Poll(std::uint64_t completedFence)
    {
        if(completedFence == (std::numeric_limits<std::uint64_t>::max)())
        {
            return;
        }

        for(auto& slot : m_slots)
        {
            if(slot.fence == 0 || slot.fence > completedFence)
            {
                continue;
            }

            void* data = nullptr;
            D3D12_RANGE range{0, StateBytes};

            if(SUCCEEDED(slot.readback->Map(0, &range, &data)))
            {
                std::uint32_t words[8];
                std::memcpy(words, data, StateBytes);
                D3D12_RANGE written{0, 0};
                slot.readback->Unmap(0, &written);

                if(slot.fence >= m_latestFence)
                {
                    m_latestFence = slot.fence;
                    std::memcpy(&m_readback.effectiveEV100, &words[0], 4);
                    std::memcpy(&m_readback.multiplier, &words[1], 4);
                    std::memcpy(&m_readback.targetEV100, &words[2], 4);
                    std::memcpy(&m_readback.meteredEV100, &words[3], 4);
                    m_readback.validPixels = words[4];
                    m_readback.invalidPixels = words[5];
                    m_readback.valid = words[6] != 0;
                }
            }

            slot.fence = 0;
        }
    }

    const Readback& GetReadback() const noexcept { return m_readback; }

    // Only after the renderer's checked GPU drain.
    void Shutdown()
    {
        for(auto& slot : m_slots)
        {
            if(slot.readback)
            {
                slot.readback->Release();
            }

            slot = {};
        }

        m_histogram.Shutdown();
        m_state.Shutdown();
        m_clear.Shutdown();
        m_histogramPipeline.Shutdown();
        m_adapt.Shutdown();
        m_ready = false;
        m_readback = {};
        m_latestFence = 0;
    }

private:
    struct Slot
    {
        ID3D12Resource* readback = nullptr;
        std::uint64_t fence = 0;
        bool recorded = false;
    };

    std::string m_failure;
    ComputePipeline m_clear;
    ComputePipeline m_histogramPipeline;
    ComputePipeline m_adapt;
    Buffer m_histogram;
    Buffer m_state;
    std::array<Slot, CommandContext::FrameCount + 1> m_slots{};
    Readback m_readback;
    std::uint64_t m_latestFence = 0;
    bool m_ready = false;
    bool m_needsClear = false;
};
