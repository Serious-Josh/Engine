#include "GraphicsDevice.h"
#include "Core/Log.h"

#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <format>
#include <string>

#ifdef _DEBUG
void GraphicsDevice::ReportDeviceRemovedData(ID3D12Device* device, const char* operationContext)
{
    if(!device){
        return;
    }
    const HRESULT reason = device->GetDeviceRemovedReason();
    if(SUCCEEDED(reason)){
        return;
    }

    Log::Error("DRED device removal during " + std::string(operationContext ? operationContext : "unknown operation") + "; HRESULT: " + std::to_string(reason));
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    const HRESULT queryResult = device->QueryInterface(IID_PPV_ARGS(&dred));
    if(FAILED(queryResult)){
        Log::Warning("ID3D12DeviceRemovedExtendedData1 unavailable. HRESULT: " + std::to_string(queryResult));
        return;
    }

    // DRED owns all returned metadata. Consume it only while the interface is alive.
    const auto name = [](const char* narrow, const wchar_t* wide)
    {
        std::string result;
        for(unsigned int i = 0; i < 128; ++i){
            if(narrow){
                if(narrow[i] == '\0') break;
                result += narrow[i];
            }else if(wide){
                if(wide[i] == L'\0') break;
                const unsigned int character = static_cast<unsigned int>(wide[i]);
                result += character < 128 ? std::string(1, static_cast<char>(character)) : std::format("\\u{:04x}", character);
            }else{
                break;
            }
        }
        return result.empty() ? std::string("<unnamed>") : result;
    };

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    const HRESULT breadcrumbResult = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    Log::Info("DRED GetAutoBreadcrumbsOutput1 HRESULT: " + std::to_string(breadcrumbResult));
    if(SUCCEEDED(breadcrumbResult)){
        const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
        if(!node) Log::Info("DRED breadcrumb list is empty.");
        for(unsigned int nodeIndex = 0; node && nodeIndex < 64; ++nodeIndex, node = node->pNext){
            Log::Info(std::format("DRED queue {} ({:p}), list {} ({:p}); breadcrumb count: {}", name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW), static_cast<const void*>(node->pCommandQueue), name(node->pCommandListDebugNameA, node->pCommandListDebugNameW), static_cast<const void*>(node->pCommandList), node->BreadcrumbCount));
            if(!node->pLastBreadcrumbValue){
                Log::Info("DRED completed operation counter unavailable.");
                continue;
            }
            const UINT completed = *node->pLastBreadcrumbValue;
            Log::Info("DRED completed operation counter: " + std::to_string(completed));
            if(completed > node->BreadcrumbCount){
                Log::Warning("DRED completed operation counter exceeds breadcrumb count; skipping history.");
                continue;
            }
            if(!node->pCommandHistory || node->BreadcrumbCount == 0){
                Log::Info("DRED operation history unavailable or empty.");
                continue;
            }
            // Only the last 65536 recorded operations remain in the history ring.
            const UINT oldest = node->BreadcrumbCount > 65536 ? node->BreadcrumbCount - 65536 : 0;
            const UINT recent = completed > 8 ? completed - 8 : 0;
            const UINT start = recent > oldest ? recent : oldest;
            const UINT remaining = node->BreadcrumbCount - completed;
            const UINT end = completed + (remaining < 8 ? remaining : 8);
            if(start >= end) Log::Info("DRED history near completion is no longer retained.");
            for(UINT operation = start; operation < end; ++operation){
                Log::Info(std::format("DRED operation {}: type {} ({})", operation, static_cast<unsigned int>(node->pCommandHistory[operation % 65536]), operation < completed ? "completed" : "not confirmed completed"));
            }
        }
        if(node) Log::Info("DRED breadcrumb list truncated at 64 nodes.");
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
    const HRESULT pageFaultResult = dred->GetPageFaultAllocationOutput1(&pageFault);
    Log::Info("DRED GetPageFaultAllocationOutput1 HRESULT: " + std::to_string(pageFaultResult));
    if(SUCCEEDED(pageFaultResult)){
        Log::Info(std::format("DRED page-fault GPU virtual address: 0x{:x}", pageFault.PageFaultVA));
        const auto allocations = [&](const char* label, const D3D12_DRED_ALLOCATION_NODE1* node)
        {
            if(!node) Log::Info(std::string("DRED ") + label + " allocation list is empty.");
            for(unsigned int index = 0; node && index < 64; ++index, node = node->pNext){
                Log::Info(std::format("DRED {} allocation: type {}, name {}", label, static_cast<unsigned int>(node->AllocationType), name(node->ObjectNameA, node->ObjectNameW)));
            }
            if(node) Log::Info(std::string("DRED ") + label + " allocation list truncated at 64 nodes.");
        };
        allocations("existing", pageFault.pHeadExistingAllocationNode);
        allocations("recently freed", pageFault.pHeadRecentFreedAllocationNode);
    }
    dred->Release();
}
#endif

bool GraphicsDevice::Initialize()
{
#ifdef _DEBUG
    constexpr bool enableGpuBasedValidation = false;
    ID3D12Debug* debugController = nullptr;

    const HRESULT debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
    if(SUCCEEDED(debugResult))
    {
        debugController->EnableDebugLayer();
        Log::Info("D3D12GetDebugInterface succeeded; D3D12 CPU debug layer enabled.");

        if constexpr(enableGpuBasedValidation){
            ID3D12Debug1* gpuValidationController = nullptr;
            const HRESULT gpuValidationResult = debugController->QueryInterface(IID_PPV_ARGS(&gpuValidationController));
            if(SUCCEEDED(gpuValidationResult)){
                gpuValidationController->SetEnableGPUBasedValidation(TRUE);
                Log::Info("D3D12 GPU-based validation requested before device creation; runtime validation results are reported separately by the debug layer.");
                gpuValidationController->Release();
            }else{
                Log::Warning("ID3D12Debug1 unavailable; continuing without requesting GPU-based validation. HRESULT: " + std::to_string(gpuValidationResult));
            }
        }

        debugController->Release();
    }
    else
    {
        Log::Warning("D3D12GetDebugInterface failed; continuing without the D3D12 CPU debug layer. HRESULT: " + std::to_string(debugResult));
    }

    ID3D12DeviceRemovedExtendedDataSettings* dredSettings = nullptr;
    const HRESULT dredResult = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));
    if(SUCCEEDED(dredResult)){
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        Log::Info("D3D12 DRED automatic breadcrumbs and page-fault tracking requested before device creation.");
        dredSettings->Release();
    }else{
        Log::Warning("D3D12 DRED settings unavailable; continuing without requesting DRED tracking. HRESULT: " + std::to_string(dredResult));
    }
#endif

    HRESULT result = CreateDXGIFactory2(
        0,
        IID_PPV_ARGS(&m_factory)
    );

    if(FAILED(result))
    {
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;

    for(UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);

        if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter->Release();
            adapter = nullptr;
            continue;
        }

        result = D3D12CreateDevice(
            adapter,
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)
        );

        if(SUCCEEDED(result))
        {
#ifdef _DEBUG
            ID3D12InfoQueue* infoQueue = nullptr;
            const HRESULT infoQueueResult = m_device->QueryInterface(IID_PPV_ARGS(&infoQueue));
            if(SUCCEEDED(infoQueueResult))
            {
                const HRESULT corruptionResult = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                if(FAILED(corruptionResult)){
                    Log::Warning("Failed to enable D3D12 InfoQueue CORRUPTION breaks. HRESULT: " + std::to_string(corruptionResult));
                }

                const HRESULT errorResult = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                if(FAILED(errorResult)){
                    Log::Warning("Failed to enable D3D12 InfoQueue ERROR breaks. HRESULT: " + std::to_string(errorResult));
                }

                if(SUCCEEDED(corruptionResult) && SUCCEEDED(errorResult)){
                    Log::Info("D3D12 InfoQueue configured to break on CORRUPTION and ERROR.");
                }
                infoQueue->Release();
            }
            else
            {
                Log::Warning("D3D12 InfoQueue unavailable; continuing without break configuration. HRESULT: " + std::to_string(infoQueueResult));
            }
#endif
            break;
        }

        adapter->Release();
        adapter = nullptr;
    }

    if(adapter)
    {
        adapter->Release();
    }

    if(m_device == nullptr)
    {
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};

    result = m_device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS5,
        &options5,
        sizeof(options5)
    );

    if(FAILED(result))
    {
        Log::Error(std::format(
            "D3D12_OPTIONS5 feature query failed. HRESULT: 0x{:x}",
            static_cast<unsigned long>(result)));
    }
    else
    {
        m_rayTracingTier = options5.RaytracingTier;

        if(SupportsRayTracing()){
            result = m_device->QueryInterface(IID_PPV_ARGS(&m_rayTracingDevice));

            if(FAILED(result))
            {
                Log::Error(std::format(
                    "Failed to acquire ID3D12Device5. HRESULT: 0x{:x}",
                    static_cast<unsigned long>(result)));

                return false;
            }
        }
    }

    switch(m_rayTracingTier){
        case D3D12_RAYTRACING_TIER_NOT_SUPPORTED:
            Log::Info("DXR ray tracing tier: Not supported");
            break;

        case D3D12_RAYTRACING_TIER_1_0:
            Log::Info("DXR ray tracing tier: 1.0");
            break;

        case D3D12_RAYTRACING_TIER_1_1:
            Log::Info("DXR ray tracing tier: 1.1");
            break;

        case D3D12_RAYTRACING_TIER_1_2:
            Log::Info("DXR ray tracing tier: 1.2");
            break;

        default:
            Log::Info(
                "DXR ray tracing tier: Unknown (" +
                std::to_string(static_cast<unsigned int>(m_rayTracingTier)) +
                ")");
            break;
    }

    return true;
}

void GraphicsDevice::Shutdown()
{
    if(m_rayTracingDevice){
        m_rayTracingDevice->Release();
        m_rayTracingDevice = nullptr;
    }

    if(m_device){
        m_device->Release();
        m_device = nullptr;
    }

    if(m_factory){
        m_factory->Release();
        m_factory = nullptr;
    }
}

ID3D12Device* GraphicsDevice::GetDevice() const
{
    return m_device;
}

IDXGIFactory4* GraphicsDevice::GetFactory() const
{
    return m_factory;
}

bool GraphicsDevice::SupportsRayTracing() const
{
    return
        m_rayTracingTier !=
        D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

ID3D12Device5* GraphicsDevice::GetRayTracingDevice() const
{
    return m_rayTracingDevice;
}
