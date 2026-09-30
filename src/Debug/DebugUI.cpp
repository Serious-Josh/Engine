#include "DebugUI.h"

#include "Renderer/DescriptorHeap.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

static void AllocateImGuiDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
{
    DescriptorHeap* descriptorHeap = static_cast<DescriptorHeap*>(info->UserData);

    unsigned int index = descriptorHeap->Allocate();

    if(index == UINT_MAX){
        *cpuHandle = {};
        *gpuHandle = {};
        return;
    }

    *cpuHandle = descriptorHeap->GetCPUHandle(index);
    *gpuHandle = descriptorHeap->GetGPUHandle(index);
}

static void FreeImGuiDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    DescriptorHeap* descriptorHeap = static_cast<DescriptorHeap*>(info->UserData);

    unsigned int index = descriptorHeap->GetIndex(cpuHandle);

    if(index != UINT_MAX){
        descriptorHeap->Free(index);
    }
}

bool DebugUI::Initialize(HWND windowHandle, ID3D12Device* device, ID3D12CommandQueue* commandQueue, DescriptorHeap& descriptorHeap)
{
    m_descriptorHeap = &descriptorHeap;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::StyleColorsDark();

    if(!ImGui_ImplWin32_Init(windowHandle)){
        ImGui::DestroyContext();
        m_descriptorHeap = nullptr;
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo{};

    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = 2;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    initInfo.SrvDescriptorHeap = descriptorHeap.GetHeap();
    initInfo.UserData = &descriptorHeap;

    initInfo.SrvDescriptorAllocFn = AllocateImGuiDescriptor;
    initInfo.SrvDescriptorFreeFn = FreeImGuiDescriptor;

    if(!ImGui_ImplDX12_Init(&initInfo)){
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_descriptorHeap = nullptr;
        return false;
    }

    return true;
}

void DebugUI::Shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext();

    m_descriptorHeap = nullptr;
}

void DebugUI::BeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGui::Begin(
        "Performance",
        nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs
    );

    ImGui::Text("%.1f FPS", io.Framerate);

    if(io.Framerate > 0.0f){
        ImGui::Text("%.2f ms", 1000.0f / io.Framerate);
    }

    ImGui::End();
}

void DebugUI::EndFrame(ID3D12GraphicsCommandList* commandList)
{
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}