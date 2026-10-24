#include "DlssBackend.h"

#include "Core/Log.h"

#include <Windows.h>
#include <ShlObj.h>
#include <d3d12.h>

#include <chrono>
#include <cstdio>
#include <iterator>
#include <system_error>

#if defined(ENGINE_HAS_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#endif

namespace
{
#if defined(ENGINE_HAS_DLSS)
    // NGX logs and temporary files: Known Folder LocalAppData/Engine/NGX (empty when it cannot be created).
    std::filesystem::path NgxDataDirectory()
    {
        PWSTR localDirectory = nullptr;

        if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localDirectory)))
        {
            return {};
        }

        const std::filesystem::path directory = std::filesystem::path(localDirectory) / "Engine" / "NGX";
        CoTaskMemFree(localDirectory);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? std::filesystem::path{} : directory;
    }

    // NGX loads nvngx_dlss.dll from the executable's directory by default (DLSS Programming Guide 4.2).
    bool RuntimeBesideExecutable()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);

        if(length == 0 || length == MAX_PATH)
        {
            return false;
        }

        std::error_code error;
        return std::filesystem::exists(std::filesystem::path(path).parent_path() / "nvngx_dlss.dll", error);
    }

    std::string Hex(std::uint32_t value)
    {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
        return buffer;
    }
#endif
}

const char* DlssAvailabilityName(DlssAvailability availability) noexcept
{
    switch(availability)
    {
    case DlssAvailability::NotCompiled:
        return "Not compiled";
    case DlssAvailability::NotInitialized:
        return "Not initialized";
    case DlssAvailability::RuntimeMissing:
        return "Runtime missing";
    case DlssAvailability::InitializationFailed:
        return "Initialization failed";
    case DlssAvailability::DriverOutdated:
        return "Driver update required";
    case DlssAvailability::Unsupported:
        return "Unsupported";
    case DlssAvailability::Available:
        return "Available - evaluation not implemented";
    }

    return "Unknown";
}

const char* DlssQualityPresetName(DlssQualityPreset preset) noexcept
{
    switch(preset)
    {
    case DlssQualityPreset::Quality:
        return "Quality";
    case DlssQualityPreset::Balanced:
        return "Balanced";
    case DlssQualityPreset::Performance:
        return "Performance";
    }

    return "";
}

DlssBackend::~DlssBackend()
{
    Shutdown();
}

#if !defined(ENGINE_HAS_DLSS)

void DlssBackend::Initialize(ID3D12Device*)
{
    m_status = {};
    m_status.availability = DlssAvailability::NotCompiled;
    m_status.detail = "Built without ENGINE_ENABLE_DLSS.";
}

void DlssBackend::Shutdown()
{
}

bool DlssBackend::QueryOptimalRenderExtent(DlssQualityPreset, unsigned int, unsigned int, unsigned int& renderWidth,
    unsigned int& renderHeight)
{
    renderWidth = 0;
    renderHeight = 0;
    return false;
}

bool DlssBackend::EnsureFeature(ID3D12GraphicsCommandList*, unsigned int, unsigned int, unsigned int, unsigned int,
    std::optional<DlssQualityPreset>)
{
    m_feature.detail = "Built without ENGINE_ENABLE_DLSS.";
    return false;
}

bool DlssBackend::Evaluate(ID3D12GraphicsCommandList*, const DlssEvaluation&)
{
    return false;
}

void DlssBackend::ReleaseFeature()
{
}

#else

void DlssBackend::Initialize(ID3D12Device* device)
{
    if(m_initialized)
    {
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    m_status = {};
    m_device = device;

    const auto finish = [&](DlssAvailability availability, std::string detail)
    {
        m_status.availability = availability;
        m_status.detail = std::move(detail);
        m_status.initializeMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log::Info(std::string("DLSS Super Resolution: ") + DlssAvailabilityName(availability) + ". " + m_status.detail);
    };

    // Stage 1: the runtime NGX would load. Without it NGX is not initialized at all.
    if(!device)
    {
        finish(DlssAvailability::InitializationFailed, "No D3D12 device.");
        return;
    }

    if(!RuntimeBesideExecutable())
    {
        finish(DlssAvailability::RuntimeMissing, "nvngx_dlss.dll is not next to the executable.");
        return;
    }

    // Stage 2: the NGX instance for this device. The engine version is Engine's CMake project version.
    const std::string projectId = projectIdForTesting ? *projectIdForTesting : std::string(ENGINE_DLSS_PROJECT_ID);
    const std::filesystem::path dataDirectory = NgxDataDirectory();
    const NVSDK_NGX_Result initResult = NVSDK_NGX_D3D12_Init_with_ProjectID(projectId.c_str(),
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_DLSS_ENGINE_VERSION, dataDirectory.c_str(), device);
    m_status.initResult = static_cast<std::uint32_t>(initResult);

    if(NVSDK_NGX_FAILED(initResult))
    {
        finish(DlssAvailability::InitializationFailed, "NVSDK_NGX_D3D12_Init_with_ProjectID returned " + Hex(initResult) +
            "; see the NGX log in " + dataDirectory.string() + ".");
        return;
    }

    m_initialized = true;

    // Stage 3: DLSS Super Resolution capability (DLSS Programming Guide 5.2.6).
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result capabilityResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    m_status.capabilityResult = static_cast<std::uint32_t>(capabilityResult);

    if(NVSDK_NGX_FAILED(capabilityResult) || !parameters)
    {
        finish(DlssAvailability::InitializationFailed, "NVSDK_NGX_D3D12_GetCapabilityParameters returned " +
            Hex(capabilityResult) + " (an older driver may lack it).");
        return;
    }

    int needsUpdatedDriver = 0;
    unsigned int minDriverMajor = 0;
    unsigned int minDriverMinor = 0;
    int available = 0;
    int featureInitResult = 0;
    const bool driverKnown =
        NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver));
    parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverMajor);
    parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverMinor);
    const bool availableKnown =
        NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available));
    const bool initResultKnown =
        NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &featureInitResult));
    NVSDK_NGX_D3D12_DestroyParameters(parameters);

    m_status.needsUpdatedDriver = driverKnown && needsUpdatedDriver != 0;
    m_status.minDriverVersionMajor = minDriverMajor;
    m_status.minDriverVersionMinor = minDriverMinor;
    m_status.superSamplingAvailable = availableKnown && available != 0;
    m_status.featureInitResult = featureInitResult;

    if(m_status.needsUpdatedDriver)
    {
        finish(DlssAvailability::DriverOutdated, "Requires driver " + std::to_string(minDriverMajor) + "." +
            std::to_string(minDriverMinor) + " or newer.");
        return;
    }

    if(!m_status.superSamplingAvailable)
    {
        finish(DlssAvailability::Unsupported, "SuperSampling.Available is 0 on this GPU/driver.");
        return;
    }

    // A failed FeatureInitResult means NGX denies the feature for this application (Programming Guide 5.2.6).
    if(initResultKnown && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(featureInitResult)))
    {
        finish(DlssAvailability::Unsupported, "SuperSampling.FeatureInitResult is " +
            Hex(static_cast<std::uint32_t>(featureInitResult)) + " (feature denied).");
        return;
    }

    finish(DlssAvailability::Available, "NGX initialized; no DLSS feature created.");
}

bool DlssBackend::EnsureParameters()
{
    if(m_parameters)
    {
        return true;
    }

    // A capability map (Programming Guide 5.2.6), because the optimal-settings query reads a callback that only it
    // carries; it also serves creation and evaluation.
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result allocated = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);

    if(NVSDK_NGX_FAILED(allocated) || !parameters)
    {
        m_feature.createResult = static_cast<std::uint32_t>(allocated);
        m_feature.detail = "NVSDK_NGX_D3D12_GetCapabilityParameters returned " + Hex(allocated) + ".";
        Log::Error("DLSS Super Resolution: " + m_feature.detail);
        return false;
    }

    m_parameters = parameters;
    return true;
}

namespace
{
    NVSDK_NGX_PerfQuality_Value PresetQualityValue(DlssQualityPreset preset) noexcept
    {
        switch(preset)
        {
        case DlssQualityPreset::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssQualityPreset::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssQualityPreset::Quality:
        default:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
    }
}

bool DlssBackend::QueryOptimalRenderExtent(DlssQualityPreset preset, unsigned int outputWidth,
    unsigned int outputHeight, unsigned int& renderWidth, unsigned int& renderHeight)
{
    renderWidth = 0;
    renderHeight = 0;

    if(m_status.availability != DlssAvailability::Available || outputWidth == 0 || outputHeight == 0 ||
        !EnsureParameters())
    {
        return false;
    }

    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    unsigned int minWidth = 0;
    unsigned int minHeight = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result queried = NGX_DLSS_GET_OPTIMAL_SETTINGS(static_cast<NVSDK_NGX_Parameter*>(m_parameters),
        outputWidth, outputHeight, PresetQualityValue(preset), &renderWidth, &renderHeight, &maxWidth, &maxHeight,
        &minWidth, &minHeight, &sharpness);

    if(NVSDK_NGX_FAILED(queried) || renderWidth == 0 || renderHeight == 0)
    {
        renderWidth = 0;
        renderHeight = 0;
        return false;
    }

    return true;
}

bool DlssBackend::EnsureFeature(ID3D12GraphicsCommandList* commandList, unsigned int renderWidth,
    unsigned int renderHeight, unsigned int outputWidth, unsigned int outputHeight,
    std::optional<DlssQualityPreset> preset)
{
    // Diagnostics: a simulated unavailable DLSS leaves an existing feature in place (no release, no recreation).
    if(availabilityOverrideForTesting && *availabilityOverrideForTesting != DlssAvailability::Available)
    {
        return false;
    }

    const int requestedPreset = preset ? static_cast<int>(*preset) : -1;
    const bool sameExtents = m_feature.renderWidth == renderWidth && m_feature.renderHeight == renderHeight &&
        m_feature.outputWidth == outputWidth && m_feature.outputHeight == outputHeight &&
        m_feature.requestedPreset == requestedPreset;

    if(sameExtents && (m_feature.active || m_feature.createResult != 0))
    {
        // Active, or a failure for these extents that is not retried.
        return m_feature.active;
    }

    ReleaseFeature();
    m_feature.renderWidth = renderWidth;
    m_feature.renderHeight = renderHeight;
    m_feature.outputWidth = outputWidth;
    m_feature.outputHeight = outputHeight;
    m_feature.requestedPreset = requestedPreset;

    if(m_status.availability != DlssAvailability::Available || !commandList)
    {
        m_feature.createResult = 0xFFFFFFFFu;
        m_feature.detail = std::string("DLSS is not available: ") + DlssAvailabilityName(m_status.availability) + ".";
        return false;
    }

    // Stage 1: the parameter map the feature lives with.
    if(!EnsureParameters())
    {
        return false;
    }

    NVSDK_NGX_Parameter* const parameters = static_cast<NVSDK_NGX_Parameter*>(m_parameters);

    // Stage 2: a quality mode whose dynamic render range holds this render extent (Programming Guide 5.2.8): the
    // requested preset's (UPSCALING.3D), otherwise the first that does (the extent is Engine's choice; DLSS only has to
    // support it).
    const NVSDK_NGX_PerfQuality_Value qualities[] = {NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        NVSDK_NGX_PerfQuality_Value_DLAA};
    const char* qualityNames[] = {"Quality", "UltraQuality", "Balanced", "Performance", "UltraPerformance", "DLAA"};
    std::string ranges;
    int selected = -1;

    for(int index = 0; index < static_cast<int>(std::size(qualities)); ++index)
    {
        unsigned int optimalWidth = 0;
        unsigned int optimalHeight = 0;
        unsigned int maxWidth = 0;
        unsigned int maxHeight = 0;
        unsigned int minWidth = 0;
        unsigned int minHeight = 0;
        float sharpness = 0.0f;
        const NVSDK_NGX_Result queried = NGX_DLSS_GET_OPTIMAL_SETTINGS(parameters, outputWidth, outputHeight,
            qualities[index], &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness);

        if(NVSDK_NGX_FAILED(queried) || optimalWidth == 0 || optimalHeight == 0)
        {
            ranges += std::string(qualityNames[index]) + ": unavailable; ";
            continue;
        }

        ranges += std::string(qualityNames[index]) + " " + std::to_string(optimalWidth) + "x" +
            std::to_string(optimalHeight) + " [" + std::to_string(minWidth) + "x" + std::to_string(minHeight) + ".." +
            std::to_string(maxWidth) + "x" + std::to_string(maxHeight) + "]; ";
        const bool contains = renderWidth >= minWidth && renderWidth <= maxWidth && renderHeight >= minHeight &&
            renderHeight <= maxHeight;

        const bool wanted = !preset || qualities[index] == PresetQualityValue(*preset);

        if(contains && wanted && selected < 0)
        {
            selected = index;
        }
    }

    m_feature.supportedRanges = ranges;

    if(selected < 0)
    {
        m_feature.createResult = 0xFFFFFFFEu;
        m_feature.detail = std::string("No DLSS quality mode") + (preset ? std::string(" (") +
            DlssQualityPresetName(*preset) + ")" : std::string()) + " supports rendering " + std::to_string(renderWidth) +
            "x" +
            std::to_string(renderHeight) + " for " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
            ".";
        Log::Error("DLSS Super Resolution: " + m_feature.detail + " Supported: " + ranges);
        return false;
    }

    // Stage 3: the feature (Programming Guide 5.3). Linear HDR colour; render-extent, unjittered motion vectors;
    // standard depth; exposure from the 1x1 texture.
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = renderWidth;
    create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = outputWidth;
    create.Feature.InTargetHeight = outputHeight;
    create.Feature.InPerfQualityValue = qualities[selected];
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result created = NGX_D3D12_CREATE_DLSS_EXT(commandList, 1, 1, &handle, parameters, &create);
    m_feature.createResult = static_cast<std::uint32_t>(created);
    m_feature.perfQuality = static_cast<int>(qualities[selected]);
    m_feature.perfQualityName = qualityNames[selected];

    if(NVSDK_NGX_FAILED(created) || !handle)
    {
        m_feature.detail = "NGX_D3D12_CREATE_DLSS_EXT returned " + Hex(created) + ".";
        Log::Error("DLSS Super Resolution: " + m_feature.detail + " Supported: " + ranges);
        return false;
    }

    m_featureHandle = handle;
    m_feature.active = true;
    m_feature.detail = std::string("Active (NGX quality mode ") + qualityNames[selected] + ").";
    Log::Info("DLSS Super Resolution feature created: " + std::to_string(renderWidth) + "x" +
        std::to_string(renderHeight) + " -> " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
        ", quality mode " + qualityNames[selected] + ". Supported: " + ranges);
    return true;
}

bool DlssBackend::Evaluate(ID3D12GraphicsCommandList* commandList, const DlssEvaluation& evaluation)
{
    if(!m_feature.active || !m_featureHandle || !commandList)
    {
        return false;
    }

    NVSDK_NGX_D3D12_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = evaluation.color;
    parameters.Feature.pInOutput = evaluation.output;
    parameters.pInDepth = evaluation.depth;
    parameters.pInMotionVectors = evaluation.motionVectors;
    parameters.pInExposureTexture = evaluation.exposure;
    parameters.InJitterOffsetX = evaluation.jitterOffsetX;
    parameters.InJitterOffsetY = evaluation.jitterOffsetY;
    parameters.InRenderSubrectDimensions = {evaluation.renderWidth, evaluation.renderHeight};
    parameters.InReset = evaluation.reset ? 1 : 0;
    parameters.InMVScaleX = 1.0f;
    parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = 1.0f;
    parameters.InExposureScale = 1.0f;

    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(commandList,
        static_cast<NVSDK_NGX_Handle*>(m_featureHandle), static_cast<NVSDK_NGX_Parameter*>(m_parameters), &parameters);
    m_feature.evaluateResult = static_cast<std::uint32_t>(result);

    if(NVSDK_NGX_FAILED(result))
    {
        // The feature stays; the caller presents its fallback for this frame.
        m_feature.detail = "NGX_D3D12_EVALUATE_DLSS_EXT returned " + Hex(result) + ".";
        return false;
    }

    ++m_feature.evaluations;
    return true;
}

void DlssBackend::ReleaseFeature()
{
    if(m_featureHandle)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_featureHandle));

        if(NVSDK_NGX_FAILED(result))
        {
            Log::Error("DLSS Super Resolution: NVSDK_NGX_D3D12_ReleaseFeature returned " + Hex(result) + ".");
        }
    }

    m_featureHandle = nullptr;
    m_feature = {};
}

void DlssBackend::Shutdown()
{
    ReleaseFeature();

    if(m_parameters)
    {
        NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(m_parameters));
        m_parameters = nullptr;
    }

    if(m_initialized)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(m_device);

        if(NVSDK_NGX_FAILED(result))
        {
            Log::Error("DLSS Super Resolution: NVSDK_NGX_D3D12_Shutdown1 returned " + Hex(result) + ".");
        }
    }

    m_initialized = false;
    m_device = nullptr;
    m_status = {};
}

#endif
