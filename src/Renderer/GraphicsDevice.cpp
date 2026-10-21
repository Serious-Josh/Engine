#include "GraphicsDevice.h"
#include "Core/Log.h"

#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace
{
    // The executable exports D3D12SDKVersion/D3D12SDKPath (AgilitySDKExports.cpp); cmake/AgilitySDK.cmake
    // deploys the matching runtime into D3D12/ beside it. A missing or rejected runtime is a deployment
    // error: device creation is never retried on the OS runtime.
    const std::string agilityRuntimeName = std::format("DirectX 12 Agility SDK {} (D3D12SDKVersion {})",
        ENGINE_AGILITY_SDK_PACKAGE_VERSION, ENGINE_AGILITY_SDK_VERSION);

    std::string UTF8Path(const std::filesystem::path& path)
    {
        const auto utf8Path = path.u8string();
        return {utf8Path.begin(), utf8Path.end()};
    }

    std::filesystem::path ModulePath(HMODULE module)
    {
        std::vector<wchar_t> buffer(MAX_PATH);

        while (true)
        {
            const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));

            if (length == 0)
            {
                return {};
            }

            if (length < buffer.size())
            {
                return std::filesystem::path(buffer.data(), buffer.data() + length);
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    std::filesystem::path AgilityRuntimeDirectory()
    {
        return ModulePath(nullptr).parent_path() / "D3D12";
    }

    std::string FileVersion(const std::filesystem::path& path)
    {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);

        if (size == 0)
        {
            return "unknown version";
        }

        std::vector<std::byte> data(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoLength = 0;

        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoLength) || !info)
        {
            return "unknown version";
        }

        return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    }

    // Checked before any D3D12 call, so the error names the missing file instead of a later HRESULT.
    bool ValidateAgilityRuntimeDeployment()
    {
        const std::filesystem::path runtimePath = AgilityRuntimeDirectory() / "D3D12Core.dll";
        std::error_code error;

        if (!std::filesystem::is_regular_file(runtimePath, error))
        {
            Log::Error("Missing D3D12 runtime component D3D12Core.dll. " + agilityRuntimeName +
                       " is required at: " + UTF8Path(runtimePath) +
                       ". Rebuild or redeploy so the D3D12 folder ships beside the executable.");
            return false;
        }

        return true;
    }

#ifdef _DEBUG
    // Without the matching SDK layers the debug layer is unavailable; the device itself still uses
    // the Agility runtime, so this keeps the existing non-fatal debug-layer failure handling.
    void ValidateAgilityDebugLayerDeployment()
    {
        const std::filesystem::path layersPath = AgilityRuntimeDirectory() / "d3d12SDKLayers.dll";
        std::error_code error;

        if (!std::filesystem::is_regular_file(layersPath, error))
        {
            Log::Warning("Missing D3D12 debug-layer component d3d12SDKLayers.dll from " + agilityRuntimeName +
                         " at: " + UTF8Path(layersPath) + ". The D3D12 debug layer will be unavailable.");
        }
    }
#endif

    // Windows may load a newer compatible D3D12Core.dll than the deployed one (documented loader
    // behavior), so the loaded module is reported rather than required to be the app-local copy.
    void LogLoadedAgilityRuntime()
    {
        const HMODULE runtimeModule = GetModuleHandleW(L"D3D12Core.dll");

        if (!runtimeModule)
        {
            Log::Warning("D3D12 device created, but D3D12Core.dll is not loaded; cannot report the D3D12 runtime.");
            return;
        }

        const std::filesystem::path loadedPath = ModulePath(runtimeModule);
        const std::filesystem::path deployedPath = AgilityRuntimeDirectory() / "D3D12Core.dll";
        std::error_code error;
        const bool deployedRuntimeLoaded = std::filesystem::equivalent(loadedPath, deployedPath, error);

        if (deployedRuntimeLoaded)
        {
            Log::Info("D3D12 runtime: deployed " + agilityRuntimeName + " at " + UTF8Path(loadedPath) + " (" +
                      FileVersion(loadedPath) + ").");
        }
        else
        {
            Log::Info("D3D12 runtime: Windows loaded " + UTF8Path(loadedPath) + " (" + FileVersion(loadedPath) +
                      ") in place of the deployed " + agilityRuntimeName + " at " + UTF8Path(deployedPath) + ".");
        }
    }
}

#ifdef _DEBUG
void GraphicsDevice::ReportDeviceRemovedData(
    ID3D12Device* device,
    const char* operationContext)
{
    if (!device)
    {
        return;
    }

    const HRESULT reason = device->GetDeviceRemovedReason();

    if (SUCCEEDED(reason))
    {
        return;
    }

    Log::Error("DRED device removal during " + std::string(operationContext ? operationContext : "unknown operation") +

               "; HRESULT: " + std::to_string(reason));
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    const HRESULT queryResult = device->QueryInterface(IID_PPV_ARGS(&dred));

    if (FAILED(queryResult))
    {
        Log::Warning("ID3D12DeviceRemovedExtendedData1 unavailable. HRESULT: " + std::to_string(queryResult));
        return;
    }

    // DRED owns all returned metadata. Consume it only while the interface is alive.

    const auto name = [](const char* narrow, const wchar_t* wide)
    {
        std::string result;

        for (unsigned int i = 0; i < 128; ++i)
        {

            if (narrow)
            {

                if (narrow[i] == '\0')
                {
                    break;
                }

                result += narrow[i];
            }
            else if (wide)
            {

                if (wide[i] == L'\0')
                {
                    break;
                }

                const unsigned int character = static_cast<unsigned int>(wide[i]);

                result += character < 128 ? std::string(1, static_cast<char>(character))
                                          : std::format("\\u{:04x}", character);
            }
            else
            {
                break;
            }
        }

        return result.empty() ? std::string("<unnamed>") : result;
    };

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    const HRESULT breadcrumbResult = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    Log::Info("DRED GetAutoBreadcrumbsOutput1 HRESULT: " + std::to_string(breadcrumbResult));

    if (SUCCEEDED(breadcrumbResult))
    {
        const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;

        if (!node)
        {
            Log::Info("DRED breadcrumb list is empty.");
        }

        for (unsigned int nodeIndex = 0; node && nodeIndex < 64; ++nodeIndex, node = node->pNext)
        {
            Log::Info(std::format("DRED queue {} ({:p}), list {} ({:p}); breadcrumb count: {}",
                                  name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                                  static_cast<const void*>(node->pCommandQueue),
                                  name(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
                                  static_cast<const void*>(node->pCommandList), node->BreadcrumbCount));

            if (!node->pLastBreadcrumbValue)
            {
                Log::Info("DRED completed operation counter unavailable.");
                continue;
            }

            const UINT completed = *node->pLastBreadcrumbValue;
            Log::Info("DRED completed operation counter: " + std::to_string(completed));

            if (completed > node->BreadcrumbCount)
            {
                Log::Warning("DRED completed operation counter exceeds breadcrumb count; skipping history.");
                continue;
            }

            if (!node->pCommandHistory || node->BreadcrumbCount == 0)
            {
                Log::Info("DRED operation history unavailable or empty.");

                continue;
            }

            // Only the last 65536 recorded operations remain in the history ring.
            const UINT oldest = node->BreadcrumbCount > 65536 ? node->BreadcrumbCount - 65536 : 0;

            const UINT recent = completed > 8 ? completed - 8 : 0;
            const UINT start = recent > oldest ? recent : oldest;
            const UINT remaining = node->BreadcrumbCount - completed;
            const UINT end = completed + (remaining < 8 ? remaining : 8);

            if (start >= end)
            {
                Log::Info("DRED history near completion is no longer retained.");
            }

            for (UINT operation = start; operation < end; ++operation)
            {
                Log::Info(std::format("DRED operation {}: type {} ({})", operation,
                                      static_cast<unsigned int>(node->pCommandHistory[operation % 65536]),
                                      operation < completed ? "completed" : "not confirmed completed"));
            }
        }

        if (node)
        {
            Log::Info("DRED breadcrumb list truncated at 64 nodes.");
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};

    const HRESULT pageFaultResult = dred->GetPageFaultAllocationOutput1(&pageFault);

    Log::Info("DRED GetPageFaultAllocationOutput1 HRESULT: " + std::to_string(pageFaultResult));

    if (SUCCEEDED(pageFaultResult))
    {
        Log::Info(std::format("DRED page-fault GPU virtual address: 0x{:x}", pageFault.PageFaultVA));

        const auto allocations = [&](const char* label, const D3D12_DRED_ALLOCATION_NODE1* node)
        {
            if (!node)
            {
                Log::Info(std::string("DRED ") + label + " allocation list is empty.");
            }

            for (unsigned int index = 0; node && index < 64; ++index, node = node->pNext)
            {
                Log::Info(std::format("DRED {} allocation: type {}, name {}", label,
                                      static_cast<unsigned int>(node->AllocationType),
                                      name(node->ObjectNameA, node->ObjectNameW)));
            }

            if (node)
            {
                Log::Info(std::string("DRED ") + label + " allocation list truncated at 64 nodes.");
            }
        };

        allocations("existing", pageFault.pHeadExistingAllocationNode);
        allocations("recently freed", pageFault.pHeadRecentFreedAllocationNode);
    }

    dred->Release();
}
#endif

bool GraphicsDevice::Initialize()
{
    if (!ValidateAgilityRuntimeDeployment())
    {
        return false;
    }

#ifdef _DEBUG
    ValidateAgilityDebugLayerDeployment();

    constexpr bool enableGpuBasedValidation = false;
    ID3D12Debug* debugController = nullptr;

    const HRESULT debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));

    if (SUCCEEDED(debugResult))
    {
        debugController->EnableDebugLayer();
        Log::Info("D3D12GetDebugInterface succeeded; D3D12 CPU debug layer enabled.");

        if constexpr (enableGpuBasedValidation)
        {
            ID3D12Debug1* gpuValidationController = nullptr;
            const HRESULT gpuValidationResult = debugController->QueryInterface(IID_PPV_ARGS(&gpuValidationController));

            if (SUCCEEDED(gpuValidationResult))
            {
                gpuValidationController->SetEnableGPUBasedValidation(TRUE);

                Log::Info("D3D12 GPU-based validation requested before device creation; runtime validation results are reported separately by the debug layer.");
                gpuValidationController->Release();
            }
            else
            {
                Log::Warning("ID3D12Debug1 unavailable; continuing without requesting GPU-based validation. HRESULT: " +
                             std::to_string(gpuValidationResult));
            }
        }

        debugController->Release();
    }
    else
    {
        Log::Warning("D3D12GetDebugInterface failed; continuing without the D3D12 CPU debug layer. HRESULT: " +
                     std::to_string(debugResult));
    }

    ID3D12DeviceRemovedExtendedDataSettings* dredSettings = nullptr;

    const HRESULT dredResult = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));

    if (SUCCEEDED(dredResult))
    {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

        Log::Info("D3D12 DRED automatic breadcrumbs and page-fault tracking requested before device creation.");
        dredSettings->Release();
    }
    else
    {
        Log::Warning("D3D12 DRED settings unavailable; continuing without requesting DRED tracking. HRESULT: " +
                     std::to_string(dredResult));
    }
#endif

    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));

    if (FAILED(result))
    {
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;

    for (UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);

        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter->Release();
            adapter = nullptr;
            continue;
        }

        result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

        // The runtime itself was rejected; every other adapter would load the same runtime files.
        if (result == D3D12_ERROR_INVALID_REDIST)
        {
            Log::Error(std::format("D3D12CreateDevice rejected the D3D12 runtime deployment (D3D12_ERROR_INVALID_REDIST, "
                                   "HRESULT 0x{:x}). {} in {} is missing, invalid or mismatched with the executable "
                                   "or its debug layer. Rebuild or redeploy the D3D12 folder.",
                                   static_cast<unsigned long>(result), agilityRuntimeName,
                                   UTF8Path(AgilityRuntimeDirectory())));
            adapter->Release();
            adapter = nullptr;
            break;
        }

        if (SUCCEEDED(result))
        {
#ifdef _DEBUG
            ID3D12InfoQueue* infoQueue = nullptr;
            const HRESULT infoQueueResult = m_device->QueryInterface(IID_PPV_ARGS(&infoQueue));

            if (SUCCEEDED(infoQueueResult))
            {
                const HRESULT corruptionResult = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);

                if (FAILED(corruptionResult))
                {
                    Log::Warning("Failed to enable D3D12 InfoQueue CORRUPTION breaks. HRESULT: " +
                                 std::to_string(corruptionResult));
                }

                const HRESULT errorResult = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);

                if (FAILED(errorResult))
                {
                    Log::Warning("Failed to enable D3D12 InfoQueue ERROR breaks. HRESULT: " +
                                 std::to_string(errorResult));
                }

                if (SUCCEEDED(corruptionResult) && SUCCEEDED(errorResult))
                {
                    Log::Info("D3D12 InfoQueue configured to break on CORRUPTION and ERROR.");
                }

                infoQueue->Release();
            }
            else
            {
                Log::Warning("D3D12 InfoQueue unavailable; continuing without break configuration. HRESULT: " +
                             std::to_string(infoQueueResult));
            }
#endif
            break;
        }

        adapter->Release();

        adapter = nullptr;
    }

    if (adapter)
    {
        adapter->Release();
    }

    if (m_device == nullptr)
    {
        return false;
    }

    LogLoadedAgilityRuntime();

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};

    result = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));

    if (FAILED(result))
    {
        Log::Error(

            std::format("D3D12_OPTIONS5 feature query failed. HRESULT: 0x{:x}", static_cast<unsigned long>(result)));
    }
    else
    {
        m_rayTracingTier = options5.RaytracingTier;

        if (SupportsRayTracing())
        {
            result = m_device->QueryInterface(IID_PPV_ARGS(&m_rayTracingDevice));

            if (FAILED(result))
            {
                Log::Error(std::format("Failed to acquire ID3D12Device5. HRESULT: 0x{:x}",
                                       static_cast<unsigned long>(result)));

                return false;
            }
        }
    }

    switch (m_rayTracingTier)
    {
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
        Log::Info("DXR ray tracing tier: Unknown (" + std::to_string(static_cast<unsigned int>(m_rayTracingTier)) +
                  ")");
        break;
    }

    QueryShaderCapabilities();

    return true;
}

void GraphicsDevice::QueryShaderCapabilities()
{
    // The runtime reports the highest model it and the driver both support, capped at the requested one.
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_HIGHEST_SHADER_MODEL};
    HRESULT result = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));

    if (FAILED(result))
    {
        Log::Error(std::format("Shader model feature query failed. HRESULT: 0x{:x}",
                               static_cast<unsigned long>(result)));
    }
    else
    {
        m_highestShaderModel = shaderModel.HighestShaderModel;
        const unsigned int model = static_cast<unsigned int>(m_highestShaderModel);
        Log::Info(std::format("Highest supported shader model: {}.{}", model >> 4, model & 0xF));
    }

    // OPTIONS22 is unknown to runtimes older than the pinned Agility SDK; a failure only means "no".
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22{};
    result = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &options22, sizeof(options22));

    if (FAILED(result))
    {
        Log::Info(std::format("D3D12_OPTIONS22 feature query unavailable. HRESULT: 0x{:x}",
                              static_cast<unsigned long>(result)));
    }
    else
    {
        m_serActuallyReorders = options22.ShaderExecutionReorderingActuallyReorders != FALSE;
    }

    Log::Info(std::string("Shader execution reordering actually reorders: ") + (m_serActuallyReorders ? "yes" : "no"));
}

void GraphicsDevice::Shutdown()
{
    if (m_rayTracingDevice)
    {
        m_rayTracingDevice->Release();
        m_rayTracingDevice = nullptr;
    }

    if (m_device)
    {
        m_device->Release();
        m_device = nullptr;
    }

    if (m_factory)
    {
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
    return m_rayTracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

ID3D12Device5* GraphicsDevice::GetRayTracingDevice() const
{
    return m_rayTracingDevice;
}

D3D12_RAYTRACING_TIER GraphicsDevice::GetRayTracingTier() const
{
    return m_rayTracingTier;
}

D3D_SHADER_MODEL GraphicsDevice::GetHighestShaderModel() const
{
    return m_highestShaderModel;
}

bool GraphicsDevice::ShaderExecutionReorderingActuallyReorders() const
{
    return m_serActuallyReorders;
}
