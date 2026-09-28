#pragma once

#include "Buffer.h"
#include "Mesh.h"
#include "Pipeline.h"
#include "GBuffer.h"
#include "Renderer/CommandContext.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RenderObject.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/SwapChain.h"

#include <d3d12.h>

struct ID3D12CommandQueue;
struct IDXGISwapChain3;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;
struct ID3D12Fence;
struct ID3D12RootSignature;
struct ID3D12PipelineState;

class DemoScene;

class Renderer
{
    public:
        bool Initialize(void* windowHandle, unsigned int width, unsigned int height);
        void Render(const RenderObject& object);
        void Shutdown();

        void Resize(unsigned int width, unsigned int height);

        void BeginGeometryPass(ID3D12GraphicsCommandList* commandList);
        void EndGeometryPass(ID3D12GraphicsCommandList* commandList);

        void ExecuteCompositePass(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV);

        bool InitializeDemoScene(DemoScene& scene);
    private:
        bool InitializeDevice();
        bool InitializeSwapChain(void* windowHandle);
        void Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object);

        GraphicsDevice m_graphicsDevice;
        SwapChain m_swapChain;
        CommandContext m_commandContext;

        ResourceUploader m_resourceUploader;
        DescriptorHeap m_resourceDescriptorHeap;
        DescriptorHeap m_rtvDescriptorHeap;
        DescriptorHeap m_dsvDescriptorHeap;
        Pipeline m_compositePipeline;

        unsigned int m_width = 0;
        unsigned int m_height = 0;

        GBuffer m_gBuffer;
};