#pragma once

#include "Renderer/LightingModeTransition.h"

#include <cstddef>
#include <string>

// Async DXR preparation, Gate 3: how the Editor presents the Renderer's lighting-mode transition snapshot.
// Pure value mapping; the Renderer alone owns the requested/active modes and the stage.

// True while the Renderer is preparing the requested mode. The lighting-mode selector is disabled and the
// progress indicator shown only then. The render-thread stages normally complete within one recording, but
// count as preparation if a snapshot ever observes them.
inline bool IsLightingModePreparing(LightingModeTransitionStage stage)
{
    switch(stage)
    {
    case LightingModeTransitionStage::Queued:
    case LightingModeTransitionStage::Compiling:
    case LightingModeTransitionStage::CreatingPipeline:
    case LightingModeTransitionStage::InitializingResources:
    case LightingModeTransitionStage::ReadyToActivate:
        return true;
    case LightingModeTransitionStage::Idle:
    case LightingModeTransitionStage::Failed:
        return false;
    }

    return false;
}

inline const char* GetLightingModeDisplayName(RendererLightingMode mode)
{
    switch(mode)
    {
    case RendererLightingMode::Legacy:
        return "Legacy";
    case RendererLightingMode::PathTracedReference:
        return "Path Traced Reference";
    case RendererLightingMode::PathTracedRasterPrimary:
        return "Raster Primary";
    case RendererLightingMode::PathTracedRasterPrimaryReSTIRDI:
        return "ReSTIR DI";
    }

    return "Unknown";
}

inline const char* GetLightingModeStageLabel(LightingModeTransitionStage stage)
{
    switch(stage)
    {
    case LightingModeTransitionStage::Idle:
        return "";
    case LightingModeTransitionStage::Queued:
        return "Queued";
    case LightingModeTransitionStage::Compiling:
        return "Compiling shaders";
    case LightingModeTransitionStage::CreatingPipeline:
        return "Creating pipeline";
    case LightingModeTransitionStage::InitializingResources:
        return "Initializing resources";
    case LightingModeTransitionStage::ReadyToActivate:
        return "Activating";
    case LightingModeTransitionStage::Failed:
        return "Failed";
    }

    return "";
}

// The first lines of a diagnostic, for compact display; the full text stays in the log.
inline std::string SummarizeLightingModeDiagnostic(const std::string& diagnostic, std::size_t maximumLines)
{
    std::string summary;
    std::size_t lineStart = 0;
    std::size_t lines = 0;

    while(lineStart < diagnostic.size() && lines < maximumLines)
    {
        std::size_t lineEnd = diagnostic.find('\n', lineStart);

        if(lineEnd == std::string::npos)
        {
            lineEnd = diagnostic.size();
        }

        if(lineEnd > lineStart)
        {
            if(!summary.empty())
            {
                summary.push_back('\n');
            }

            summary.append(diagnostic, lineStart, lineEnd - lineStart);
            ++lines;
        }

        lineStart = lineEnd + 1;
    }

    if(lineStart < diagnostic.size())
    {
        summary.append("\n(see the log for the full error)");
    }

    return summary;
}

// Identifies one failure, so a dismissed notification is not reopened while the same failure persists.
inline std::string GetLightingModeFailureKey(const LightingModeTransitionStatus& status)
{
    return std::string(GetLightingModeDisplayName(status.requested)) + "\n" + status.diagnostic;
}
