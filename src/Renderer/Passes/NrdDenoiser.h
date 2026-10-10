#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Texture.h"

#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>

#include <NRD.h>
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperD3D12.h>

#include <NRDIntegration.h>

class DescriptorHeap;

class NrdDenoiser
{
    public:

        enum class Signal
        {
            Combined,
            Specular,
            Diffuse
        };

        bool Initialize(
            ID3D12Device* device,
            ID3D12CommandQueue* graphicsQueue,
            DescriptorHeap& descriptorHeap,
            uint32_t width,
            uint32_t height,
            uint32_t queuedFrameCount,
            Signal signal = Signal::Combined);
        bool Resize(
            DescriptorHeap& descriptorHeap,
            uint32_t width,
            uint32_t height);
        void Shutdown();

        bool SetFrameSettings(
            const Matrix4& view,
            const Matrix4& projection,
            const Matrix4& previousView,
            const Matrix4& previousProjection,
            uint32_t frameIndex,
            float denoisingRange,
            bool resetHistory);

        void Denoise(
            ID3D12GraphicsCommandList* commandList,
            Texture& motion,
            Texture& normalRoughness,
            Texture& viewZ,
            Texture& radianceHitDist,
            Signal signal = Signal::Combined);

        Texture& GetOutputTexture();
        const Texture& GetOutputTexture() const;

        Texture& GetDiffuseOutputTexture();
        const Texture& GetDiffuseOutputTexture() const;

        void NewFrame();

        double GetTotalMemoryUsageInMb() const;
        double GetPersistentMemoryUsageInMb() const;
        double GetAliasableMemoryUsageInMb() const;

    private:
        bool Recreate(
            DescriptorHeap& descriptorHeap,
            uint32_t width,
            uint32_t height);

        Texture m_outputTexture;
        Texture m_diffuseOutputTexture;

        nrd::Integration m_Integration;

        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_graphicsQueue = nullptr;
        uint32_t m_queuedFrameCount = 0;

        uint32_t m_width = 0;
        uint32_t m_height = 0;

        Signal m_signal = Signal::Combined;
};