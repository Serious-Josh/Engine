#include "DebugUI.h"

#include "Renderer/DescriptorHeap.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "ImGuizmo.h"

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

    ImGuizmo::BeginFrame();
}

void DebugUI::DrawPerformanceWindow(const std::vector<std::string>& environmentNames, size_t currentEnvironmentIndex, const RendererStatistics& statistics)
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

    ImGui::Text(
        "Geometry draws: %llu",
        static_cast<unsigned long long>(
            statistics.geometryDrawCalls));

    ImGui::Text(
        "Triangles: %llu",
        static_cast<unsigned long long>(
            statistics.submittedTriangles));

    ImGui::Text(
        "Render objects: %llu",
        static_cast<unsigned long long>(
            statistics.renderObjectCount));

    ImGui::Text(
        "TLAS instances: %llu",
        static_cast<unsigned long long>(
            statistics.tlasInstanceCount));

    ImGui::Separator();

    const GpuTimestampProfiler::Results& gpu =
        statistics.gpuTimings;

    auto drawGpuTiming = [&](const char* name, GpuTimestampProfiler::Region region)
    {
        const unsigned int index =
            static_cast<unsigned int>(region);

        const uint32_t mask = 1u << index;

        if((gpu.validRegions & mask) != 0)
        {
            ImGui::Text("%s: %.3f ms", name, gpu.milliseconds[index]);
        }
        else
        {
            ImGui::Text("%s: -- (inactive / unavailable)", name);
        }
    };

    drawGpuTiming(
        "GPU frame",
        GpuTimestampProfiler::Region::TotalFrame);

    ImGui::TextUnformatted("Geometry / scene");
    drawGpuTiming("  TLAS update", GpuTimestampProfiler::Region::TlasUpdate);
    drawGpuTiming(
        "  Geometry",
        GpuTimestampProfiler::Region::Geometry);
    drawGpuTiming("  Selection mask", GpuTimestampProfiler::Region::SelectionMask);

    ImGui::TextUnformatted("Shadows");
    drawGpuTiming(
        "  RT shadows",
        GpuTimestampProfiler::Region::RayTracedShadows);

    drawGpuTiming(
        "  Shadow denoise",
        GpuTimestampProfiler::Region::ShadowDenoise);

    ImGui::TextUnformatted("Ambient occlusion");
    drawGpuTiming("  Ray-Traced AO", GpuTimestampProfiler::Region::RayTracedAO);
    drawGpuTiming("  AO Temporal", GpuTimestampProfiler::Region::AOTemporal);
    drawGpuTiming("  AO Denoise", GpuTimestampProfiler::Region::AODenoise);

    ImGui::TextUnformatted("Reflections");
    drawGpuTiming(
        "  RT reflections",
        GpuTimestampProfiler::Region::RayTracedReflections);

    drawGpuTiming(
        "  Reflection limiter",
        GpuTimestampProfiler::Region::ReflectionContributionLimiter);
    drawGpuTiming("  Native temporal", GpuTimestampProfiler::Region::ReflectionTemporal);
    drawGpuTiming("  Native spatial denoise", GpuTimestampProfiler::Region::ReflectionDenoise);

    drawGpuTiming(
        "  NRD prepare",
        GpuTimestampProfiler::Region::NrdPrepare);

    drawGpuTiming(
        "  NRD REBLUR",
        GpuTimestampProfiler::Region::NrdReblur);

    drawGpuTiming(
        "  NRD reflection resolve",
        GpuTimestampProfiler::Region::NrdResolve);

    ImGui::TextUnformatted("Global illumination");
    drawGpuTiming("  Ray-Traced GI", GpuTimestampProfiler::Region::RayTracedGI);
    drawGpuTiming("  NRD diffuse resolve", GpuTimestampProfiler::Region::NrdDiffuseResolve);

    ImGui::TextUnformatted("Deferred lighting / presentation");
    drawGpuTiming(
        "  Deferred lighting",
        GpuTimestampProfiler::Region::DeferredLighting);

    drawGpuTiming(
        "  Tone map",
        GpuTimestampProfiler::Region::ToneMap);
    drawGpuTiming("  DebugUI rendering", GpuTimestampProfiler::Region::DebugUI);

    ImGui::Separator();

    ImGui::Text("TLAS update %s", m_tlasUpdated ? "yes" : "no");
    ImGui::Text("TLAS updates: %llu", static_cast<unsigned long long>(m_totalTLASUpdates));

    ImGui::End();

    ImGui::Begin("Rendering debug");

    ImGui::TextUnformatted("Environment");

    const char* currentEnvironmentName =
        currentEnvironmentIndex < environmentNames.size()
            ? environmentNames[currentEnvironmentIndex].c_str()
            : "Unknown";

    if(ImGui::BeginCombo("HDRI", currentEnvironmentName))
    {
        for(size_t i = 0; i < environmentNames.size(); ++i)
        {
            const bool selected = i == currentEnvironmentIndex;

            if(ImGui::Selectable(environmentNames[i].c_str(), selected))
            {
                m_requestedEnvironmentIndex = static_cast<int>(i);
            }

            if(selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Separator();

    const char* views[] = {
        "None",
        "Specular tracking distance",
        "NRD ViewZ",
        "NRD Normal",
        "NRD Roughness",
        "NRD Motion",
        "NRD Spec Radiance",
        "NRD Hit Distance",
        "NrdResolvedReflection",
        "GBuffer Motion",
        "Selection Mask (BLACK SCREEN)",
        "GI Raw",
        "GI Hit Distance",
        "GI Denoised",
        "AO Raw",
        "AO Temporal",
        "AO Filtered"
    };
    const char* denoisers[] = {"NRD", "Native"};
    ImGui::Combo("Reflection denoiser", &m_reflectionDenoiserMode, denoisers, IM_ARRAYSIZE(denoisers));
    ImGui::Checkbox("NRD contribution limiter", &m_contributionLimiterEnabled);
    ImGui::Checkbox("Enable RTGI", &m_rtgiEnabled);
    ImGui::Combo("Diagnostic view", &m_toneMapDebugView, views, IM_ARRAYSIZE(views));
    ImGui::Separator();
    if(GetToneMapDebugView() == ToneMapDebugView::SpecularTrackingDistance){
        ImGui::TextUnformatted("Magenta: invalid | Blue: environment miss | Gray: geometry");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::GBufferMotion){
        ImGui::TextUnformatted("Gray: zero UV motion | R: signed X | G: signed Y (20x)");
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
    else if(GetToneMapDebugView() == ToneMapDebugView::GIRaw){
        ImGui::TextUnformatted(
            "Raw diffuse GI irradiance (noisy) | Black: invalid surface");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::GIHitDistance){
        ImGui::TextUnformatted(
            "Gray: hit distance | Blue: miss | Magenta: invalid");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::AORaw){
        ImGui::TextUnformatted(
            "White: unoccluded | Black: occluded | Background: white");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::AOTemporal){
        ImGui::TextUnformatted(
            "Temporally accumulated ray-traced ambient occlusion.");
    }
    else if(GetToneMapDebugView() == ToneMapDebugView::AOFiltered){
        ImGui::TextUnformatted(
            "Temporally accumulated AO after edge-aware spatial filtering.");
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
