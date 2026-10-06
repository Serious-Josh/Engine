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
}

void DebugUI::DrawPerformanceWindow()
{
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

    ImGui::Separator();

    ImGui::Text("TLAS update %s", m_tlasUpdated ? "yes" : "no");
    ImGui::Text("TLAS updates: %llu", static_cast<unsigned long long>(m_totalTLASUpdates));

    ImGui::End();

    ImGui::Begin("Rendering debug");
    const char* views[] = {"None", "Specular tracking distance", "NRD ViewZ", "NRD Normal",
        "NRD Roughness", "NRD Motion", "NRD Spec Radiance", "NRD Hit Distance",
        "NrdResolvedReflection"};
    const char* denoisers[] = {"NRD", "Native"};
    ImGui::Combo("Reflection denoiser", &m_reflectionDenoiserMode, denoisers, IM_ARRAYSIZE(denoisers));
    ImGui::Checkbox("NRD contribution limiter", &m_contributionLimiterEnabled);
    ImGui::Combo("Diagnostic view", &m_toneMapDebugView, views, IM_ARRAYSIZE(views));
    ImGui::Separator();
    if(GetToneMapDebugView() == ToneMapDebugView::SpecularTrackingDistance){
        ImGui::TextUnformatted("Magenta: invalid | Blue: environment miss | Gray: geometry");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::NrdViewZ){
        ImGui::TextUnformatted("Gray: Z / (Z + 10) | Magenta: beyond NRD range | Red: invalid Z");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::NrdMotion){
        ImGui::TextUnformatted("Black: exact zero XYZ | RGB: amplified absolute XYZ (100x)");
        ImGui::TextUnformatted("Any nonzero component has at least 25% intensity; camera motion should stay black.");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::NrdSpecularRadiance){
        ImGui::TextUnformatted("Decoded demodulated radiance with AgX display mapping");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::NrdRoughness ||
            GetToneMapDebugView() == ToneMapDebugView::NrdHitDistance){
        ImGui::TextUnformatted("Decoded value: 0 = black | 1 = white");
    }
    ImGui::End();
}

void DebugUI::EndFrame(ID3D12GraphicsCommandList* commandList)
{
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void DebugUI::SetTLASUpdated(bool updated)
{
    m_tlasUpdated = updated;

    if(updated){
        ++m_totalTLASUpdates;
    }
}
