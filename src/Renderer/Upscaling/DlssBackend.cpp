#include "DlssBackend.h"

#include "Core/Log.h"
#include "Renderer/ResourceStateTransaction.h"

#include <Windows.h>
#include <ShlObj.h>
#include <d3d12.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <system_error>
#include <utility>

#if defined(ENGINE_HAS_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#if defined(ENGINE_HAS_DLSS_RR)
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#endif
#endif

namespace
{
    // RR.1A: the production Ray Reconstruction runtime NGX loads from beside the executable (RR guide 2.1, 4.2).
    constexpr const char* RayReconstructionRuntimeName = "nvngx_dlssd.dll";

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

    // NGX loads its feature DLLs from the executable's directory by default (DLSS Programming Guide 4.2).
    bool RuntimeBesideExecutable(const std::string& fileName)
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);

        if(length == 0 || length == MAX_PATH)
        {
            return false;
        }

        std::error_code error;
        return std::filesystem::exists(std::filesystem::path(path).parent_path() / fileName, error);
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
        return "Available";
    }

    return "Unknown";
}

const char* DlssRayReconstructionAvailabilityName(DlssRayReconstructionAvailability availability) noexcept
{
    switch(availability)
    {
    case DlssRayReconstructionAvailability::NotCompiled:
        return "Not compiled";
    case DlssRayReconstructionAvailability::NotPermitted:
        return "Not permitted (Editor only)";
    case DlssRayReconstructionAvailability::NotInitialized:
        return "Not initialized";
    case DlssRayReconstructionAvailability::RuntimeMissing:
        return "Runtime missing";
    case DlssRayReconstructionAvailability::NgxInitializationFailed:
        return "NGX initialization failed";
    case DlssRayReconstructionAvailability::CapabilityQueryFailed:
        return "Capability query failed";
    case DlssRayReconstructionAvailability::DriverOutdated:
        return "Driver update required";
    case DlssRayReconstructionAvailability::Unsupported:
        return "Unsupported";
    case DlssRayReconstructionAvailability::FeatureInitFailed:
        return "Feature initialization failed";
    case DlssRayReconstructionAvailability::Available:
        return "Available";
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

const char* DlssRayReconstructionQualityName(DlssRayReconstructionQuality quality) noexcept
{
    switch(quality)
    {
    case DlssRayReconstructionQuality::Dlaa:
        return "DLAA";
    case DlssRayReconstructionQuality::Quality:
        return "Quality";
    case DlssRayReconstructionQuality::Balanced:
        return "Balanced";
    case DlssRayReconstructionQuality::Performance:
        return "Performance";
    }

    return "";
}

DlssBackend::~DlssBackend()
{
    Shutdown();
}

// RR.1A lifecycle bookkeeping (shared by the stub, whose slots simply never hold a handle).

void DlssBackend::ResolvePendingEvaluation(const FeatureSlot& slot) const
{
    if(slot.pendingRecording == 0)
    {
        return;
    }

    // Still recording on its list: pending.
    if(ResourceStateTransaction::RecordingFor(slot.pendingList) == slot.pendingRecording)
    {
        return;
    }

    // Discarded (R.1), or closed without ConfirmRecordingSubmitted (a failed or unhooked submission): it never counts.
    // NGX may have advanced its internal temporal state while recording it, which cannot be rolled back, so the next
    // evaluation resets. An unconfirmed one may also have reached the GPU, so its completion is unknown.
    if(!ResourceStateTransaction::IsDiscarded(slot.pendingRecording))
    {
        slot.lastUseUnknown = true;
    }

    slot.pendingRecording = 0;
    slot.pendingList = nullptr;
    slot.historyInvalid = true;
    m_recoveryRequired = true;
}

bool DlssBackend::RecoveryRequired() const
{
    ResolvePendingEvaluation(m_superResolution);
    ResolvePendingEvaluation(m_rayReconstruction);
    return m_recoveryRequired;
}

void DlssBackend::MarkRecovered()
{
    m_recoveryRequired = false;
}

bool DlssBackend::EvaluationNeedsReset(DlssFeatureKind kind) const
{
    const FeatureSlot& slot = Slot(kind);
    ResolvePendingEvaluation(slot);
    return slot.historyInvalid || slot.status.evaluations == 0;
}

void DlssBackend::ConfirmFeatureCreation(DlssFeatureKind kind, std::uint64_t fenceValue)
{
    FeatureSlot& slot = Slot(kind);

    if(!slot.creationPending)
    {
        return;
    }

    slot.creationPending = false;
    slot.lastUseFence = (std::max)(slot.lastUseFence, fenceValue);
    // A lifecycle submission whose fence signal failed may still have executed: completion is unknown.
    slot.lastUseUnknown = slot.lastUseUnknown || fenceValue == 0;
}

void DlssBackend::ConfirmRecordingSubmitted(std::uint64_t recording, std::uint64_t fenceValue)
{
    if(recording == 0)
    {
        return;
    }

    for(FeatureSlot* slot : {&m_superResolution, &m_rayReconstruction})
    {
        if(slot->pendingRecording != recording)
        {
            continue;
        }

        ++slot->status.evaluations;
        slot->lastUseFence = (std::max)(slot->lastUseFence, fenceValue);
        slot->lastUseUnknown = slot->lastUseUnknown || fenceValue == 0;
        slot->pendingRecording = 0;
        slot->pendingList = nullptr;
    }
}

bool DlssBackend::CanRelease(const FeatureSlot& slot, std::uint64_t completedFenceValue, bool gpuIdle,
    std::string& reason) const
{
    if(!slot.handle)
    {
        return true;
    }

    ResolvePendingEvaluation(slot);

    if(slot.creationPending)
    {
        reason = "its creation is recorded on a lifecycle list that was neither submitted nor abandoned";
        return false;
    }

    if(slot.pendingRecording != 0)
    {
        reason = "an evaluation is recorded in the open frame recording";
        return false;
    }

    if(gpuIdle)
    {
        return true;
    }

    if(slot.lastUseUnknown)
    {
        reason = "a submission's completion is unknown (a GPU drain is required)";
        return false;
    }

    if(completedFenceValue < slot.lastUseFence)
    {
        reason = "the GPU has not completed fence " + std::to_string(slot.lastUseFence) + " (completed " +
            std::to_string(completedFenceValue) + ")";
        return false;
    }

    return true;
}

bool DlssBackend::CanReleaseFeature(DlssFeatureKind kind, std::uint64_t completedFenceValue, bool gpuIdle) const
{
    std::string reason;
    return CanRelease(Slot(kind), completedFenceValue, gpuIdle, reason);
}

bool DlssBackend::ReleaseFeature(DlssFeatureKind kind, std::uint64_t completedFenceValue, bool gpuIdle, bool freeMemory)
{
    FeatureSlot& slot = Slot(kind);
    std::string reason;

    if(!CanRelease(slot, completedFenceValue, gpuIdle, reason))
    {
        Log::Error(std::string("DLSS: refused to release the ") +
            (kind == DlssFeatureKind::SuperResolution ? "Super Resolution" : "Ray Reconstruction") + " feature: " + reason +
            ".");
        return false;
    }

    ReleaseSlot(slot, freeMemory);
    return true;
}

bool DlssBackend::HasFeature(DlssFeatureKind kind) const noexcept
{
    return Slot(kind).handle != nullptr;
}

unsigned int DlssBackend::LiveFeatureCount() const noexcept
{
    return (m_superResolution.handle ? 1u : 0u) + (m_rayReconstruction.handle ? 1u : 0u);
}

bool DlssBackend::FeatureCreationNeeded(unsigned int renderWidth, unsigned int renderHeight, unsigned int outputWidth,
    unsigned int outputHeight, std::optional<DlssQualityPreset> preset) const
{
    const DlssFeatureStatus& feature = m_superResolution.status;
    const int requestedPreset = preset ? static_cast<int>(*preset) : -1;
    const bool sameExtents = feature.renderWidth == renderWidth && feature.renderHeight == renderHeight &&
        feature.outputWidth == outputWidth && feature.outputHeight == outputHeight && feature.requestedPreset == requestedPreset;
    // Active, or a failure for these extents that is not retried.
    return !(sameExtents && (feature.active || feature.createResult != 0));
}

bool DlssBackend::RayReconstructionCreationNeeded(unsigned int renderWidth, unsigned int renderHeight,
    unsigned int outputWidth, unsigned int outputHeight, DlssRayReconstructionQuality quality) const
{
    const DlssFeatureStatus& feature = m_rayReconstruction.status;
    const bool sameExtents = feature.renderWidth == renderWidth && feature.renderHeight == renderHeight &&
        feature.outputWidth == outputWidth && feature.outputHeight == outputHeight &&
        feature.requestedPreset == static_cast<int>(quality);
    return !(sameExtents && (feature.active || feature.createResult != 0));
}

#if !defined(ENGINE_HAS_DLSS)

void DlssBackend::Initialize(ID3D12Device*)
{
    m_status = {};
    m_status.availability = DlssAvailability::NotCompiled;
    m_status.detail = "DLSS backend was not built (SDK missing in AUTO, or build policy OFF).";
    m_status.rayReconstruction.availability = DlssRayReconstructionAvailability::NotCompiled;
    m_status.rayReconstruction.detail = "DLSS Ray Reconstruction was not built (it needs the DLSS backend).";
    Log::Info("DLSS Super Resolution: Not compiled. " + m_status.detail);
}

void DlssBackend::BeginInitialize(ID3D12Device* device)
{
    Initialize(device);
}

bool DlssBackend::CompleteInitialize(bool)
{
    return true;
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
    m_superResolution.status.detail = "DLSS backend not compiled (SDK missing in AUTO, or build policy OFF).";
    return false;
}

bool DlssBackend::Evaluate(ID3D12GraphicsCommandList*, const DlssEvaluation&)
{
    return false;
}

void DlssBackend::AbandonFeatureCreation(DlssFeatureKind kind, const std::string& detail)
{
    Slot(kind).creationPending = false;
    Slot(kind).status.detail = detail;
}

void DlssBackend::ReleaseSlot(FeatureSlot& slot, bool)
{
    slot = FeatureSlot{};
}

bool DlssBackend::QueryRayReconstructionOptimalRenderExtent(DlssRayReconstructionQuality, unsigned int, unsigned int,
    unsigned int& renderWidth, unsigned int& renderHeight)
{
    renderWidth = 0;
    renderHeight = 0;
    return false;
}

bool DlssBackend::EnsureRayReconstructionFeature(ID3D12GraphicsCommandList*, unsigned int, unsigned int, unsigned int,
    unsigned int, DlssRayReconstructionQuality)
{
    m_rayReconstruction.status.detail = "DLSS Ray Reconstruction not compiled.";
    return false;
}

bool DlssBackend::QueryRayReconstructionMemory(std::uint64_t& bytes)
{
    bytes = 0;
    return false;
}

bool DlssBackend::EvaluateRayReconstruction(ID3D12GraphicsCommandList*, const DlssRayReconstructionEvaluation&)
{
    return false;
}

#else

DlssBackend::InitializationRequest DlssBackend::MakeInitializationRequest() const
{
    InitializationRequest request;
    request.projectId = projectIdForTesting ? *projectIdForTesting : std::string(ENGINE_DLSS_PROJECT_ID);
    request.rayReconstructionPermitted = m_rayReconstructionPermitted;
    request.rayReconstructionRuntimeName = rayReconstructionRuntimeNameForTesting ? *rayReconstructionRuntimeNameForTesting :
        std::string(RayReconstructionRuntimeName);
    return request;
}

void DlssBackend::Adopt(InitializationOutcome outcome, ID3D12Device* device)
{
    m_status = std::move(outcome.status);
    m_initialized = outcome.initialized;
    m_device = device;
    Log::Info(std::string("DLSS Super Resolution: ") + DlssAvailabilityName(m_status.availability) + ". " + m_status.detail +
        " (" + std::to_string(m_status.initializeMilliseconds) + " ms)");
    Log::Info(std::string("DLSS Ray Reconstruction: ") +
        DlssRayReconstructionAvailabilityName(m_status.rayReconstruction.availability) + ". " +
        m_status.rayReconstruction.detail);
}

void DlssBackend::Initialize(ID3D12Device* device)
{
    if(m_pending.valid())
    {
        CompleteInitialize(true);
        return;
    }

    if(m_initialized)
    {
        return;
    }

    Adopt(RunInitialize(device, MakeInitializationRequest()), device);
}

void DlssBackend::BeginInitialize(ID3D12Device* device)
{
    if(m_initialized || m_pending.valid())
    {
        return;
    }

    m_pendingDevice = device;
    // std::async with launch::async: a thread of its own; the future's destructor would also wait for it.
    m_pending = std::async(std::launch::async, [device, request = MakeInitializationRequest()]()
    {
        return RunInitialize(device, request);
    });
}

bool DlssBackend::CompleteInitialize(bool wait)
{
    if(!m_pending.valid())
    {
        return true;
    }

    if(!wait && m_pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }

    Adopt(m_pending.get(), m_pendingDevice);
    m_pendingDevice = nullptr;
    return true;
}

DlssBackend::InitializationOutcome DlssBackend::RunInitialize(ID3D12Device* device, const InitializationRequest& request)
{
    InitializationOutcome outcome;
    const auto start = std::chrono::steady_clock::now();

    DlssStatus& status = outcome.status;
    DlssRayReconstructionStatus& rayReconstruction = status.rayReconstruction;

    // RR.1A Stage 0: what Ray Reconstruction can be before NGX is touched. Its runtime is checked here, beside Super
    // Resolution's; its capability is read from the same parameters below.
#if defined(ENGINE_HAS_DLSS_RR)
    if(!request.rayReconstructionPermitted)
    {
        rayReconstruction.availability = DlssRayReconstructionAvailability::NotPermitted;
        rayReconstruction.detail = "Ray Reconstruction is Editor-only: its runtime is not redistributed with packaged games.";
    }
    else
    {
        rayReconstruction.runtimePresent = RuntimeBesideExecutable(request.rayReconstructionRuntimeName);
    }
#else
    rayReconstruction.availability = DlssRayReconstructionAvailability::NotCompiled;
    rayReconstruction.detail = "DLSS Ray Reconstruction was not built (ENGINE_ENABLE_DLSS_RR, or its SDK files are missing).";
#endif

    const auto finish = [&](DlssAvailability availability, std::string detail)
    {
        status.availability = availability;
        status.detail = std::move(detail);

        // Ray Reconstruction not decided yet: NGX was not initialized (or its capability never read).
        if(rayReconstruction.availability == DlssRayReconstructionAvailability::NotInitialized)
        {
            rayReconstruction.availability = outcome.initialized ? DlssRayReconstructionAvailability::CapabilityQueryFailed :
                DlssRayReconstructionAvailability::NgxInitializationFailed;
            rayReconstruction.detail = "NGX was not ready for Ray Reconstruction: " + status.detail;
        }

        status.initializeMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return outcome;
    };

    // Stage 1: the runtime NGX would load. Without it NGX is not initialized at all.
    if(!device)
    {
        return finish(DlssAvailability::InitializationFailed, "No D3D12 device.");
    }

    if(!RuntimeBesideExecutable("nvngx_dlss.dll"))
    {
        return finish(DlssAvailability::RuntimeMissing, "nvngx_dlss.dll is not next to the executable.");
    }

    // Stage 2: the NGX instance for this device. The engine version is Engine's CMake project version.

    const std::filesystem::path dataDirectory = NgxDataDirectory();
    const NVSDK_NGX_Result initResult = NVSDK_NGX_D3D12_Init_with_ProjectID(request.projectId.c_str(),
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_DLSS_ENGINE_VERSION, dataDirectory.c_str(), device);
    status.initResult = static_cast<std::uint32_t>(initResult);

    if(NVSDK_NGX_FAILED(initResult))
    {
        return finish(DlssAvailability::InitializationFailed, "NVSDK_NGX_D3D12_Init_with_ProjectID returned " + Hex(initResult) +
            "; see the NGX log in " + dataDirectory.string() + ".");
    }

    outcome.initialized = true;

    // Stage 3: DLSS Super Resolution capability (DLSS Programming Guide 5.2.6).
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result capabilityResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    status.capabilityResult = static_cast<std::uint32_t>(capabilityResult);

    if(NVSDK_NGX_FAILED(capabilityResult) || !parameters)
    {
        return finish(DlssAvailability::InitializationFailed, "NVSDK_NGX_D3D12_GetCapabilityParameters returned " +
            Hex(capabilityResult) + " (an older driver may lack it).");
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

#if defined(ENGINE_HAS_DLSS_RR)
    // RR.1A Stage 3b: Ray Reconstruction capability, the SuperSamplingDenoising.* parameters (nvsdk_ngx_defs_dlssd.h).
    if(rayReconstruction.availability == DlssRayReconstructionAvailability::NotInitialized)
    {
        int rrNeedsUpdatedDriver = 0;
        unsigned int rrMinDriverMajor = 0;
        unsigned int rrMinDriverMinor = 0;
        int rrAvailable = 0;
        int rrFeatureInitResult = 0;
        const bool rrDriverKnown = NVSDK_NGX_SUCCEED(
            parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rrNeedsUpdatedDriver));
        parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &rrMinDriverMajor);
        parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &rrMinDriverMinor);
        const bool rrAvailableKnown =
            NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable));
        const bool rrInitResultKnown = NVSDK_NGX_SUCCEED(
            parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &rrFeatureInitResult));
        rayReconstruction.needsUpdatedDriver = rrDriverKnown && rrNeedsUpdatedDriver != 0;
        rayReconstruction.minDriverVersionMajor = rrMinDriverMajor;
        rayReconstruction.minDriverVersionMinor = rrMinDriverMinor;
        rayReconstruction.available = rrAvailableKnown && rrAvailable != 0;
        rayReconstruction.featureInitResult = rrFeatureInitResult;
        const auto decide = [&](DlssRayReconstructionAvailability availability, std::string detail)
        {
            rayReconstruction.availability = availability;
            rayReconstruction.detail = std::move(detail);
        };

        // The runtime is checked first: NGX reports a feature whose DLL it cannot find as unavailable, which would hide
        // the actionable reason.
        if(!rayReconstruction.runtimePresent)
        {
            decide(DlssRayReconstructionAvailability::RuntimeMissing, request.rayReconstructionRuntimeName +
                " is not next to the executable.");
        }
        else if(rayReconstruction.needsUpdatedDriver)
        {
            decide(DlssRayReconstructionAvailability::DriverOutdated, "Requires driver " + std::to_string(rrMinDriverMajor) +
                "." + std::to_string(rrMinDriverMinor) + " or newer.");
        }
        else if(!rrAvailableKnown)
        {
            decide(DlssRayReconstructionAvailability::CapabilityQueryFailed,
                "SuperSamplingDenoising.Available capability query failed.");
        }
        else if(!rayReconstruction.available)
        {
            decide(DlssRayReconstructionAvailability::Unsupported, "SuperSamplingDenoising.Available is 0 on this GPU/driver.");
        }
        else if(rrInitResultKnown && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(rrFeatureInitResult)))
        {
            decide(DlssRayReconstructionAvailability::FeatureInitFailed, "SuperSamplingDenoising.FeatureInitResult is " +
                Hex(static_cast<std::uint32_t>(rrFeatureInitResult)) + ".");
        }
        else
        {
            decide(DlssRayReconstructionAvailability::Available, "NGX reports Ray Reconstruction available (driver minimum " +
                std::to_string(rrMinDriverMajor) + "." + std::to_string(rrMinDriverMinor) + ").");
        }
    }
#endif

    NVSDK_NGX_D3D12_DestroyParameters(parameters);

    status.needsUpdatedDriver = driverKnown && needsUpdatedDriver != 0;
    status.minDriverVersionMajor = minDriverMajor;
    status.minDriverVersionMinor = minDriverMinor;
    status.superSamplingAvailable = availableKnown && available != 0;
    status.featureInitResult = featureInitResult;

    if(status.needsUpdatedDriver)
    {
        return finish(DlssAvailability::DriverOutdated, "Requires driver " + std::to_string(minDriverMajor) + "." +
            std::to_string(minDriverMinor) + " or newer.");
    }

    if(!availableKnown)
    {
        return finish(DlssAvailability::InitializationFailed, "SuperSampling.Available capability query failed.");
    }

    if(!status.superSamplingAvailable)
    {
        return finish(DlssAvailability::Unsupported, "SuperSampling.Available is 0 on this GPU/driver.");
    }

    // A failed FeatureInitResult means NGX denies the feature for this application (Programming Guide 5.2.6).
    if(initResultKnown && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(featureInitResult)))
    {
        return finish(DlssAvailability::Unsupported, "SuperSampling.FeatureInitResult is " +
            Hex(static_cast<std::uint32_t>(featureInitResult)) + " (feature denied).");
    }

    return finish(DlssAvailability::Available, "NGX initialized; Super Resolution capability available.");
}

bool DlssBackend::EnsureParameters(FeatureSlot& slot, const char* featureName)
{
    if(slot.parameters)
    {
        return true;
    }

    // A capability map (Programming Guide 5.2.6), because the optimal-settings query reads a callback that only it
    // carries; it also serves creation and evaluation. RR.1A: one map per feature, so one feature's query and creation
    // parameters never reach the other's calls.
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result allocated = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);

    if(NVSDK_NGX_FAILED(allocated) || !parameters)
    {
        slot.status.createResult = static_cast<std::uint32_t>(allocated);
        slot.status.detail = "NVSDK_NGX_D3D12_GetCapabilityParameters returned " + Hex(allocated) + ".";
        Log::Error(std::string(featureName) + ": " + slot.status.detail);
        return false;
    }

    slot.parameters = parameters;
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
        !EnsureParameters(m_superResolution, "DLSS Super Resolution"))
    {
        return false;
    }

    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    unsigned int minWidth = 0;
    unsigned int minHeight = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result queried = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        static_cast<NVSDK_NGX_Parameter*>(m_superResolution.parameters), outputWidth, outputHeight,
        PresetQualityValue(preset), &renderWidth, &renderHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness);

    if(NVSDK_NGX_FAILED(queried) || renderWidth == 0 || renderHeight == 0)
    {
        m_superResolution.status.detail = "Optimal-settings query returned " + Hex(queried) + " (or an empty extent).";
        renderWidth = 0;
        renderHeight = 0;
        return false;
    }

    return true;
}

bool DlssBackend::EnsureFeature(ID3D12GraphicsCommandList* lifecycleList, unsigned int renderWidth,
    unsigned int renderHeight, unsigned int outputWidth, unsigned int outputHeight,
    std::optional<DlssQualityPreset> preset)
{
    // Diagnostics: a simulated unavailable DLSS leaves an existing feature in place (no release, no recreation).
    if(availabilityOverrideForTesting && *availabilityOverrideForTesting != DlssAvailability::Available)
    {
        return false;
    }

    FeatureSlot& slot = m_superResolution;
    DlssFeatureStatus& feature = slot.status;

    // RR.1A: never into an instance awaiting its restart (nothing is memoized: the restart clears the slot anyway).
    if(RecoveryRequired())
    {
        return false;
    }

    if(!FeatureCreationNeeded(renderWidth, renderHeight, outputWidth, outputHeight, preset))
    {
        // Active, or a failure for these extents that is not retried.
        return feature.active;
    }

    // RR.1A: a feature for other extents is the caller's to release first, where its GPU use is known to be complete.
    if(slot.handle)
    {
        Log::Error("DLSS Super Resolution: a feature for other extents is still live; it must be released before creation.");
        return false;
    }

    feature = {};
    feature.renderWidth = renderWidth;
    feature.renderHeight = renderHeight;
    feature.outputWidth = outputWidth;
    feature.outputHeight = outputHeight;
    feature.requestedPreset = preset ? static_cast<int>(*preset) : -1;

    if(m_status.availability != DlssAvailability::Available || !lifecycleList)
    {
        feature.createResult = 0xFFFFFFFFu;
        feature.detail = std::string("DLSS is not available: ") + DlssAvailabilityName(m_status.availability) + ".";
        return false;
    }

    // Stage 1: the parameter map the feature lives with.
    if(!EnsureParameters(slot, "DLSS Super Resolution"))
    {
        return false;
    }

    NVSDK_NGX_Parameter* const parameters = static_cast<NVSDK_NGX_Parameter*>(slot.parameters);

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

    feature.supportedRanges = ranges;

    if(selected < 0)
    {
        feature.createResult = 0xFFFFFFFEu;
        feature.detail = std::string("No DLSS quality mode") + (preset ? std::string(" (") +
            DlssQualityPresetName(*preset) + ")" : std::string()) + " supports rendering " + std::to_string(renderWidth) +
            "x" +
            std::to_string(renderHeight) + " for " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
            ".";
        Log::Error("DLSS Super Resolution: " + feature.detail + " Supported: " + ranges);
        return false;
    }

    // Stage 3: the feature (Programming Guide 5.3). Linear HDR colour; render-extent, unjittered motion vectors;
    // standard depth; exposure from the 1x1 texture. RR.1A: recorded on the lifecycle list, which the caller submits.
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = renderWidth;
    create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = outputWidth;
    create.Feature.InTargetHeight = outputHeight;
    create.Feature.InPerfQualityValue = qualities[selected];
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result created = NGX_D3D12_CREATE_DLSS_EXT(lifecycleList, 1, 1, &handle, parameters, &create);
    feature.createResult = static_cast<std::uint32_t>(created);
    feature.perfQuality = static_cast<int>(qualities[selected]);
    feature.perfQualityName = qualityNames[selected];

    if(NVSDK_NGX_FAILED(created) || !handle)
    {
        feature.detail = "NGX_D3D12_CREATE_DLSS_EXT returned " + Hex(created) + ".";
        Log::Error("DLSS Super Resolution: " + feature.detail + " Supported: " + ranges);
        return false;
    }

    slot.handle = handle;
    slot.creationPending = true;

    if(std::exchange(failNextCreationForTesting, false))
    {
        // The handle exists; the caller abandons the lifecycle list and AbandonFeatureCreation releases it.
        feature.detail = "diagnostic: creation reported failed after NGX created the feature.";
        return false;
    }

    feature.active = true;
    feature.detail = std::string("Active (NGX quality mode ") + qualityNames[selected] + ").";
    Log::Info("DLSS Super Resolution feature created: " + std::to_string(renderWidth) + "x" +
        std::to_string(renderHeight) + " -> " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) +
        ", quality mode " + qualityNames[selected] + ". Supported: " + ranges);
    return true;
}

void DlssBackend::AbandonFeatureCreation(DlssFeatureKind kind, const std::string& detail)
{
    FeatureSlot& slot = Slot(kind);

    if(!slot.creationPending)
    {
        return;
    }

    // The lifecycle list holding the creation was never executed, so the handle has no GPU use: release it now. The
    // failure stays memoized for these extents (createResult), as a failed creation's.
    const DlssFeatureStatus failed = slot.status;
    slot.creationPending = false;
    ReleaseSlot(slot, true);
    slot.status = failed;
    slot.status.active = false;
    slot.status.createResult = slot.status.createResult != 0 && NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(
        slot.status.createResult)) ? slot.status.createResult : 0xFFFFFFFDu;
    slot.status.detail = detail;
    Log::Error(std::string(kind == DlssFeatureKind::SuperResolution ? "DLSS Super Resolution" : "DLSS Ray Reconstruction") +
        ": creation abandoned: " + detail);
}

bool DlssBackend::Evaluate(ID3D12GraphicsCommandList* commandList, const DlssEvaluation& evaluation)
{
    FeatureSlot& slot = m_superResolution;

    if(!slot.status.active || !slot.handle || !commandList || slot.creationPending)
    {
        return false;
    }

    // RR.1A: an instance awaiting its restart is never evaluated (its internal resource-state tracking is not the GPU's).
    if(RecoveryRequired())
    {
        slot.status.detail = "NGX recovery pending after a discarded evaluation.";
        return false;
    }

    // RR.1A: the history restarts with the caller's reset, a fresh feature, or a discarded/unconfirmed earlier evaluation.
    const bool reset = evaluation.reset || EvaluationNeedsReset(DlssFeatureKind::SuperResolution);
    lastEvaluationResetForTesting = reset;

    NVSDK_NGX_D3D12_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = evaluation.color;
    parameters.Feature.pInOutput = evaluation.output;
    parameters.pInDepth = evaluation.depth;
    parameters.pInMotionVectors = evaluation.motionVectors;
    parameters.pInExposureTexture = evaluation.exposure;
    parameters.InJitterOffsetX = evaluation.jitterOffsetX;
    parameters.InJitterOffsetY = evaluation.jitterOffsetY;
    parameters.InRenderSubrectDimensions = {evaluation.renderWidth, evaluation.renderHeight};
    parameters.InReset = reset ? 1 : 0;
    parameters.InMVScaleX = 1.0f;
    parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = 1.0f;
    parameters.InExposureScale = 1.0f;

    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(commandList,
        static_cast<NVSDK_NGX_Handle*>(slot.handle), static_cast<NVSDK_NGX_Parameter*>(slot.parameters), &parameters);
    slot.status.evaluateResult = static_cast<std::uint32_t>(result);

    if(NVSDK_NGX_FAILED(result))
    {
        // The feature stays; the caller presents its fallback for this frame. Whatever NGX recorded or changed is not a
        // continuation of the history.
        slot.historyInvalid = true;
        const std::string detail = "NGX_D3D12_EVALUATE_DLSS_EXT returned " + Hex(result) + ".";
        if(slot.status.detail != detail) Log::Error("DLSS Super Resolution: " + detail);
        slot.status.detail = detail;
        return false;
    }

    slot.status.detail = std::string("Active (NGX quality mode ") + slot.status.perfQualityName + ").";
    RecordPendingEvaluation(slot, commandList);
    return true;
}

void DlssBackend::RecordPendingEvaluation(FeatureSlot& slot, ID3D12GraphicsCommandList* commandList)
{
    // Counted when this recording is confirmed submitted; until then pending.
    slot.pendingRecording = ResourceStateTransaction::RecordingFor(commandList);
    slot.pendingList = commandList;
    slot.historyInvalid = false;

    if(slot.pendingRecording == 0)
    {
        // Not the frame recording: no R.1 transaction tells whether it is submitted, so treat it as unconfirmed.
        slot.lastUseUnknown = true;
        slot.historyInvalid = true;
        m_recoveryRequired = true;
        slot.pendingList = nullptr;
    }
}

bool DlssBackend::EvaluateRayReconstruction(ID3D12GraphicsCommandList* commandList,
    const DlssRayReconstructionEvaluation& evaluation)
{
    FeatureSlot& slot = m_rayReconstruction;

    if(!slot.status.active || !slot.handle || !commandList || slot.creationPending)
    {
        return false;
    }

    // RR.1A: an instance awaiting its restart is never evaluated (its internal resource-state tracking is not the GPU's).
    if(RecoveryRequired())
    {
        slot.status.detail = "NGX recovery pending after a discarded evaluation.";
        return false;
    }

    const bool reset = evaluation.reset || EvaluationNeedsReset(DlssFeatureKind::RayReconstruction);
    lastRayReconstructionResetForTesting = reset;

    if(std::exchange(failNextRayReconstructionEvaluationForTesting, false))
    {
        // Nothing was recorded; the failure is reported as an NGX failure would be.
        slot.historyInvalid = true;
        slot.status.evaluateResult = 0xFFFFFFFFu;
        slot.status.detail = "diagnostic: Ray Reconstruction evaluation reported failed.";
        return false;
    }

#if defined(ENGINE_HAS_DLSS_RR)
    // RR guide 3.4: colour, depth, motion vectors, both albedos, normals with roughness packed in alpha (the creation's
    // Roughness_Mode_Packed: pInRoughness stays null), and the specular (reflection) motion vectors. Exposure: RR guide
    // 3.7 calls it unsupported; RR.1C's A/B decides what Engine passes (DLSS_Ray_Reconstruction_RR1C.md).
    NVSDK_NGX_D3D12_DLSSD_Eval_Params parameters{};
    parameters.pInColor = evaluation.color;
    parameters.pInOutput = evaluation.output;
    parameters.pInDepth = evaluation.depth;
    parameters.pInMotionVectors = evaluation.motionVectors;
    parameters.pInMotionVectorsReflections = evaluation.specularMotionVectors;
    parameters.pInDiffuseAlbedo = evaluation.diffuseAlbedo;
    parameters.pInSpecularAlbedo = evaluation.specularAlbedo;
    parameters.pInNormals = evaluation.normalRoughness;
    parameters.pInRoughness = nullptr;
    parameters.pInExposureTexture = evaluation.exposure;
    parameters.InPreExposure = evaluation.preExposure;
    parameters.InExposureScale = 1.0f;
    parameters.InJitterOffsetX = evaluation.jitterOffsetX;
    parameters.InJitterOffsetY = evaluation.jitterOffsetY;
    parameters.InRenderSubrectDimensions = {evaluation.renderWidth, evaluation.renderHeight};
    parameters.InReset = reset ? 1 : 0;
    parameters.InMVScaleX = 1.0f;
    parameters.InMVScaleY = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluation.frameTimeDeltaMilliseconds;

    const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSD_EXT(commandList,
        static_cast<NVSDK_NGX_Handle*>(slot.handle), static_cast<NVSDK_NGX_Parameter*>(slot.parameters), &parameters);
    slot.status.evaluateResult = static_cast<std::uint32_t>(result);

    if(NVSDK_NGX_FAILED(result))
    {
        slot.historyInvalid = true;
        const std::string detail = "NGX_D3D12_EVALUATE_DLSSD_EXT returned " + Hex(result) + ".";
        if(slot.status.detail != detail) Log::Error("DLSS Ray Reconstruction: " + detail);
        slot.status.detail = detail;
        return false;
    }

    slot.status.detail = std::string("Active (") + slot.status.perfQualityName + ").";
    RecordPendingEvaluation(slot, commandList);
    return true;
#else
    (void)evaluation;
    return false;
#endif
}

void DlssBackend::ReleaseSlot(FeatureSlot& slot, bool freeMemory)
{
    if(slot.handle)
    {
        if(freeMemory && slot.parameters)
        {
            // Programming Guide 3.14: release the feature's memory instead of caching it for an immediate re-creation.
            static_cast<NVSDK_NGX_Parameter*>(slot.parameters)->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        }

        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(slot.handle));

        if(NVSDK_NGX_FAILED(result))
        {
            Log::Error("DLSS: NVSDK_NGX_D3D12_ReleaseFeature returned " + Hex(result) + ".");
        }

        if(freeMemory && slot.parameters)
        {
            static_cast<NVSDK_NGX_Parameter*>(slot.parameters)->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 0);
        }
    }

    void* const parameters = slot.parameters;
    slot = FeatureSlot{};
    slot.parameters = parameters;
}

namespace
{
#if defined(ENGINE_HAS_DLSS_RR)
    NVSDK_NGX_PerfQuality_Value RayReconstructionQualityValue(DlssRayReconstructionQuality quality) noexcept
    {
        switch(quality)
        {
        case DlssRayReconstructionQuality::Dlaa:
            return NVSDK_NGX_PerfQuality_Value_DLAA;
        case DlssRayReconstructionQuality::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssRayReconstructionQuality::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssRayReconstructionQuality::Quality:
        default:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
    }
#endif
}

bool DlssBackend::QueryRayReconstructionOptimalRenderExtent(DlssRayReconstructionQuality quality,
    unsigned int outputWidth, unsigned int outputHeight, unsigned int& renderWidth, unsigned int& renderHeight)
{
    renderWidth = 0;
    renderHeight = 0;
#if defined(ENGINE_HAS_DLSS_RR)
    FeatureSlot& slot = m_rayReconstruction;

    if(m_status.rayReconstruction.availability != DlssRayReconstructionAvailability::Available || outputWidth == 0 ||
        outputHeight == 0 || !EnsureParameters(slot, "DLSS Ray Reconstruction"))
    {
        return false;
    }

    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    unsigned int minWidth = 0;
    unsigned int minHeight = 0;
    // RR guide 5.5: the sharpness output must be ignored.
    float sharpness = 0.0f;
    const NVSDK_NGX_Result queried = NGX_DLSSD_GET_OPTIMAL_SETTINGS(static_cast<NVSDK_NGX_Parameter*>(slot.parameters),
        outputWidth, outputHeight, RayReconstructionQualityValue(quality), &renderWidth, &renderHeight, &maxWidth, &maxHeight,
        &minWidth, &minHeight, &sharpness);

    if(NVSDK_NGX_FAILED(queried) || renderWidth == 0 || renderHeight == 0)
    {
        slot.status.detail = "Ray Reconstruction optimal-settings query returned " + Hex(queried) + " (or an empty extent).";
        renderWidth = 0;
        renderHeight = 0;
        return false;
    }

    return true;
#else
    (void)quality;
    (void)outputWidth;
    (void)outputHeight;
    return false;
#endif
}

bool DlssBackend::EnsureRayReconstructionFeature(ID3D12GraphicsCommandList* lifecycleList, unsigned int renderWidth,
    unsigned int renderHeight, unsigned int outputWidth, unsigned int outputHeight, DlssRayReconstructionQuality quality)
{
    FeatureSlot& slot = m_rayReconstruction;
    DlssFeatureStatus& feature = slot.status;

    if(RecoveryRequired())
    {
        return false;
    }

    if(!RayReconstructionCreationNeeded(renderWidth, renderHeight, outputWidth, outputHeight, quality))
    {
        return feature.active;
    }

    if(slot.handle)
    {
        Log::Error("DLSS Ray Reconstruction: a feature for other extents is still live; it must be released before creation.");
        return false;
    }

    feature = {};
    feature.renderWidth = renderWidth;
    feature.renderHeight = renderHeight;
    feature.outputWidth = outputWidth;
    feature.outputHeight = outputHeight;
    feature.requestedPreset = static_cast<int>(quality);

#if defined(ENGINE_HAS_DLSS_RR)
    if(m_status.rayReconstruction.availability != DlssRayReconstructionAvailability::Available || !lifecycleList)
    {
        feature.createResult = 0xFFFFFFFFu;
        feature.detail = std::string("Ray Reconstruction is not available: ") +
            DlssRayReconstructionAvailabilityName(m_status.rayReconstruction.availability) + ".";
        return false;
    }

    if(!EnsureParameters(slot, "DLSS Ray Reconstruction"))
    {
        return false;
    }

    NVSDK_NGX_Parameter* const parameters = static_cast<NVSDK_NGX_Parameter*>(slot.parameters);
    const NVSDK_NGX_PerfQuality_Value perfQuality = RayReconstructionQualityValue(quality);

    // The requested mode's dynamic render range must hold the render extent (RR guide 5.5); DLAA renders at the output.
    unsigned int optimalWidth = 0;
    unsigned int optimalHeight = 0;
    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    unsigned int minWidth = 0;
    unsigned int minHeight = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result queried = NGX_DLSSD_GET_OPTIMAL_SETTINGS(parameters, outputWidth, outputHeight, perfQuality,
        &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness);
    feature.supportedRanges = std::string(DlssRayReconstructionQualityName(quality)) + " " + std::to_string(optimalWidth) +
        "x" + std::to_string(optimalHeight) + " [" + std::to_string(minWidth) + "x" + std::to_string(minHeight) + ".." +
        std::to_string(maxWidth) + "x" + std::to_string(maxHeight) + "]";
    const bool contains = quality == DlssRayReconstructionQuality::Dlaa ?
        renderWidth == outputWidth && renderHeight == outputHeight :
        renderWidth >= minWidth && renderWidth <= maxWidth && renderHeight >= minHeight && renderHeight <= maxHeight;

    if(NVSDK_NGX_FAILED(queried) || optimalWidth == 0 || !contains)
    {
        feature.createResult = 0xFFFFFFFEu;
        feature.detail = std::string("Ray Reconstruction ") + DlssRayReconstructionQualityName(quality) +
            " does not support rendering " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) + " for " +
            std::to_string(outputWidth) + "x" + std::to_string(outputHeight) + " (query " + Hex(queried) + "; " +
            feature.supportedRanges + ").";
        Log::Error("DLSS Ray Reconstruction: " + feature.detail);
        return false;
    }

    // RR guide 5.3: DLUnified denoise mode. Linear HDR colour, render-extent motion vectors, hardware depth, roughness in
    // the normals' alpha (RR.0 §7.2). No preset hint: NGX's default for every mode (RR guide 3.13).
    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    create.InWidth = renderWidth;
    create.InHeight = renderHeight;
    create.InTargetWidth = outputWidth;
    create.InTargetHeight = outputHeight;
    create.InPerfQualityValue = perfQuality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    create.InEnableOutputSubrects = false;
    // M5.6 (user-approved default 2026-11-23): the RR model, hinted for every quality mode
    // (NVSDK_NGX_RayReconstruction_Hint_Render_Preset). Default E, the latest transformer model: it halved still-camera and
    // after-stop silhouette shimmer and reduced it about 8-10% in motion on ManyLight_1024 for about +0.5 ms
    // (M5.6 report section 10). ENGINE_RR_PRESET=F restores the RR2 model NGX used by default before (D: the earlier
    // transformer; "none": no hint, NGX's own default, which may change after OTA updates).
    static const unsigned int rrPreset = []
    {
        const char* value = std::getenv("ENGINE_RR_PRESET");
        const std::string text = value ? value : "";
        return text == "none" ? 0u : text == "D" ? 4u : text == "F" ? 6u : 5u;
    }();

    if(rrPreset != 0u)
    {
        NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, rrPreset);
        NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, rrPreset);
        NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, rrPreset);
        NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, rrPreset);
        NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
            rrPreset);
        Log::Info("DLSS Ray Reconstruction: render preset hint " + std::to_string(rrPreset) + " (ENGINE_RR_PRESET).");
    }

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result created = NGX_D3D12_CREATE_DLSSD_EXT(lifecycleList, 1, 1, &handle, parameters, &create);
    feature.createResult = static_cast<std::uint32_t>(created);
    feature.perfQuality = static_cast<int>(perfQuality);
    feature.perfQualityName = DlssRayReconstructionQualityName(quality);

    if(NVSDK_NGX_FAILED(created) || !handle)
    {
        feature.detail = "NGX_D3D12_CREATE_DLSSD_EXT returned " + Hex(created) + ".";
        Log::Error("DLSS Ray Reconstruction: " + feature.detail);
        return false;
    }

    slot.handle = handle;
    slot.creationPending = true;

    if(std::exchange(failNextCreationForTesting, false))
    {
        feature.detail = "diagnostic: creation reported failed after NGX created the feature.";
        return false;
    }

    feature.active = true;
    feature.detail = std::string("Created (") + DlssRayReconstructionQualityName(quality) + ").";
    Log::Info("DLSS Ray Reconstruction feature created: " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) +
        " -> " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) + ", " +
        DlssRayReconstructionQualityName(quality) + ".");
    return true;
#else
    (void)lifecycleList;
    feature.createResult = 0xFFFFFFFFu;
    feature.detail = "DLSS Ray Reconstruction not compiled.";
    return false;
#endif
}

bool DlssBackend::QueryRayReconstructionMemory(std::uint64_t& bytes)
{
    bytes = 0;
#if defined(ENGINE_HAS_DLSS_RR)
    if(m_status.rayReconstruction.availability != DlssRayReconstructionAvailability::Available ||
        !EnsureParameters(m_rayReconstruction, "DLSS Ray Reconstruction"))
    {
        return false;
    }

    unsigned long long allocated = 0;
    const NVSDK_NGX_Result queried =
        NGX_DLSSD_GET_STATS(static_cast<NVSDK_NGX_Parameter*>(m_rayReconstruction.parameters), &allocated);
    bytes = allocated;
    return NVSDK_NGX_SUCCEED(queried);
#else
    return false;
#endif
}

void DlssBackend::Shutdown()
{
    // M0c.5 Stage 5.5: an initialization still running is waited for and adopted, so the NGX instance it creates is released
    // below, before the device. The caller has drained the GPU, so both features release unconditionally.
    CompleteInitialize(true);

    if(m_initialized)
    {
        Log::Info("DLSS: NGX shutdown; releasing " + std::to_string(LiveFeatureCount()) + " live feature(s).");
    }

    for(FeatureSlot* slot : {&m_superResolution, &m_rayReconstruction})
    {
        ReleaseSlot(*slot, false);

        if(slot->parameters)
        {
            NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(slot->parameters));
            slot->parameters = nullptr;
        }
    }

    if(m_initialized)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(m_device);

        if(NVSDK_NGX_FAILED(result))
        {
            Log::Error("DLSS Super Resolution: NVSDK_NGX_D3D12_Shutdown1 returned " + Hex(result) + ".");
        }

        Log::Info("DLSS: NGX shut down; live features " + std::to_string(LiveFeatureCount()) + ".");
    }

    m_initialized = false;
    m_device = nullptr;
    m_status = {};
}

#endif
