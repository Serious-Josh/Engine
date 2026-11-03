#pragma once

#include "Project/ProjectSettings.h"
#include "Renderer/RenderingConfiguration.h"

#include <cstddef>
#include <cstdint>
#include <optional>

// PS.2C: temporary Debug panel overrides of the saved rendering settings, for this Editor session only.
// They are never written to ProjectSettings.json; the effective settings are the saved settings with every
// present override applied, resolved by ResolveRenderingConfiguration like the saved settings alone.
//
// Each list entry is (override name, RenderingSettings member). The lists drive the struct, the field
// identifiers, application, clearing and counting, so a field is declared once.
#define ENGINE_RENDERING_SETTING_OVERRIDES(X) \
    X(lightingMode, lightingMode) \
    X(toneMapper, toneMapper) \
    X(reflectionDenoiser, legacy.reflectionDenoiser) \
    X(contributionLimiter, legacy.contributionLimiter) \
    X(rtgi, legacy.rtgi) \
    X(rtgiContactAOStrength, legacy.rtgiContactAOStrength) \
    X(rtgiContactAORadius, legacy.rtgiContactAORadius) \
    X(samplesPerFrame, pathTracing.samplesPerFrame) \
    X(maxPathDepth, pathTracing.maxPathDepth) \
    X(rouletteFromDepth, pathTracing.rouletteFromDepth) \
    X(sampleTarget, pathTracing.sampleTarget) \
    X(psrMode, pathTracing.psrMode) \
    X(psrDebugView, pathTracing.psrDebugView) \
    X(dlss, pathTracing.dlss) \
    X(dlssQuality, pathTracing.dlssQuality) \
    X(cameraJitter, pathTracing.cameraJitter) \
    X(denoiser, pathTracing.denoiser) \
    X(denoiserView, pathTracing.denoiserView) \
    X(denoiserSignal, pathTracing.denoiserSignal) \
    X(viewExposureStops, pathTracing.viewExposureStops) \
    X(differenceAmplification, pathTracing.differenceAmplification) \
    X(uncertaintyFloorLog10, pathTracing.uncertaintyFloorLog10) \
    X(sharc, pathTracing.sharc) \
    X(dedicatedCaustics, pathTracing.dedicatedCaustics) \
    X(causticOnlyView, pathTracing.causticOnlyView) \
    X(pixelCentredCaustics, pathTracing.pixelCentredCaustics)

// Debug-only Reference Path Tracer development overrides (name, type). No project setting reaches Reference;
// without these it renders with its fixed internal quality and no dedicated caustics.
#define ENGINE_REFERENCE_DEBUG_OVERRIDES(X) \
    X(referenceSamplesPerFrame, std::uint32_t) \
    X(referenceMaxPathDepth, std::uint32_t) \
    X(referenceRouletteFromDepth, std::uint32_t) \
    X(referenceSampleTarget, std::uint32_t) \
    X(referenceDedicatedCaustics, bool) \
    X(referenceCausticOnlyView, bool)

enum class RenderingOverrideField
{
#define ENGINE_OVERRIDE_FIELD_ENUM(name, ...) name,
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_OVERRIDE_FIELD_ENUM)
    ENGINE_REFERENCE_DEBUG_OVERRIDES(ENGINE_OVERRIDE_FIELD_ENUM)
#undef ENGINE_OVERRIDE_FIELD_ENUM
};

struct RenderingOverrides
{
#define ENGINE_SETTING_OVERRIDE_MEMBER(name, member) std::optional<decltype(RenderingSettings{}.member)> name;
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_SETTING_OVERRIDE_MEMBER)
#undef ENGINE_SETTING_OVERRIDE_MEMBER

#define ENGINE_REFERENCE_OVERRIDE_MEMBER(name, type) std::optional<type> name;
    ENGINE_REFERENCE_DEBUG_OVERRIDES(ENGINE_REFERENCE_OVERRIDE_MEMBER)
#undef ENGINE_REFERENCE_OVERRIDE_MEMBER

    bool operator==(const RenderingOverrides&) const = default;
};

// The saved settings with every present setting override applied (the Reference overrides do not apply here).
RenderingSettings ApplyRenderingOverrides(const RenderingSettings& saved, const RenderingOverrides& overrides);

// Writes the Reference overrides into diagnostics: a quality request starting from Reference's fixed quality,
// and dedicated caustics. Nothing is written when no Reference override is present.
void ApplyReferenceOverrides(const RenderingOverrides& overrides, RenderingDiagnostics& diagnostics);

bool IsOverridden(const RenderingOverrides& overrides, RenderingOverrideField field);
void ClearOverride(RenderingOverrides& overrides, RenderingOverrideField field);
std::size_t CountOverrides(const RenderingOverrides& overrides);

// After a successful save from previousSaved to newSaved: clears each setting override whose saved value the
// save changed (the save decided it) or that now equals the saved value (it no longer overrides anything).
// Overrides of unchanged settings stay, as do the Debug-only Reference overrides.
void ClearConflictingOverrides(RenderingOverrides& overrides, const RenderingSettings& previousSaved,
    const RenderingSettings& newSaved);
