#pragma once

#include "Renderer/Passes/ToneMapPass.h"

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct HWND__;  

using HWND = HWND__*;

class DescriptorHeap;

enum class ReflectionDenoiserMode { Nrd, Native };

class DebugUI
{
    public:
        DebugUI() = default;

        DebugUI(const DebugUI&) = delete;
        DebugUI& operator=(const DebugUI&) = delete;

        bool Initialize(HWND windowsHandle, ID3D12Device* device, ID3D12CommandQueue* commandQueue, DescriptorHeap& descriptorHeap);
        void Shutdown();

        void BeginFrame();
        void DrawPerformanceWindow(const std::vector<std::string>& environmentNames, size_t currentEnvironmentIndex);
        void EndFrame(ID3D12GraphicsCommandList* commandList);

        void SetTLASUpdated(bool updated);
        ReflectionDenoiserMode GetReflectionDenoiserMode() const { return static_cast<ReflectionDenoiserMode>(m_reflectionDenoiserMode); }
        bool IsReflectionContributionLimiterEnabled() const { return m_contributionLimiterEnabled; }
        ToneMapDebugView GetToneMapDebugView() const { return static_cast<ToneMapDebugView>(m_toneMapDebugView); }

        bool HasEnvironmentChangeRequest() const
        {
            return m_requestedEnvironmentIndex >= 0;
        }

        size_t ConsumeEnvironmentChangeRequest()
        {
            const size_t index =
                static_cast<size_t>(m_requestedEnvironmentIndex);

            m_requestedEnvironmentIndex = -1;

            return index;
        }

    private:
        DescriptorHeap* m_descriptorHeap = nullptr;

        bool m_tlasUpdated = false;
        int m_toneMapDebugView = 0;
        int m_reflectionDenoiserMode = static_cast<int>(ReflectionDenoiserMode::Nrd);
        bool m_contributionLimiterEnabled = true;
        std::uint64_t m_totalTLASUpdates = 0;

        int m_requestedEnvironmentIndex = -1;
};
