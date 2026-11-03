#include "Engine/RenderingOverrides.h"

RenderingSettings ApplyRenderingOverrides(const RenderingSettings& saved, const RenderingOverrides& overrides)
{
    RenderingSettings effective = saved;

#define ENGINE_APPLY_SETTING_OVERRIDE(name, member) \
    if(overrides.name) \
    { \
        effective.member = *overrides.name; \
    }
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_APPLY_SETTING_OVERRIDE)
#undef ENGINE_APPLY_SETTING_OVERRIDE

    return effective;
}

void ApplyReferenceOverrides(const RenderingOverrides& overrides, RenderingDiagnostics& diagnostics)
{
    const bool qualityOverridden = overrides.referenceSamplesPerFrame || overrides.referenceMaxPathDepth ||
        overrides.referenceRouletteFromDepth || overrides.referenceSampleTarget;

    if(qualityOverridden)
    {
        const PathTraceReferenceSettings fixed = GetReferencePathTracerQuality();
        PathTraceReferenceSettings quality = fixed;
        quality.samplesPerFrame = overrides.referenceSamplesPerFrame.value_or(fixed.samplesPerFrame);
        quality.maxPathDepth = overrides.referenceMaxPathDepth.value_or(fixed.maxPathDepth);
        quality.russianRouletteDepth = overrides.referenceRouletteFromDepth.value_or(fixed.russianRouletteDepth);
        quality.maxAccumulatedSamples = overrides.referenceSampleTarget.value_or(fixed.maxAccumulatedSamples);
        diagnostics.referenceQuality = quality;
    }

    diagnostics.referenceDedicatedCaustics = overrides.referenceDedicatedCaustics.value_or(false);
    diagnostics.referenceCausticOnlyView = overrides.referenceCausticOnlyView.value_or(false);
}

bool IsOverridden(const RenderingOverrides& overrides, RenderingOverrideField field)
{
    switch(field)
    {
#define ENGINE_OVERRIDE_PRESENT(name, ...) \
    case RenderingOverrideField::name: \
        return overrides.name.has_value();
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_OVERRIDE_PRESENT)
    ENGINE_REFERENCE_DEBUG_OVERRIDES(ENGINE_OVERRIDE_PRESENT)
#undef ENGINE_OVERRIDE_PRESENT
    }

    return false;
}

void ClearOverride(RenderingOverrides& overrides, RenderingOverrideField field)
{
    switch(field)
    {
#define ENGINE_OVERRIDE_CLEAR(name, ...) \
    case RenderingOverrideField::name: \
        overrides.name.reset(); \
        break;
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_OVERRIDE_CLEAR)
    ENGINE_REFERENCE_DEBUG_OVERRIDES(ENGINE_OVERRIDE_CLEAR)
#undef ENGINE_OVERRIDE_CLEAR
    }
}

std::size_t CountOverrides(const RenderingOverrides& overrides)
{
    std::size_t count = 0;

#define ENGINE_OVERRIDE_COUNT(name, ...) count += overrides.name.has_value() ? 1u : 0u;
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_OVERRIDE_COUNT)
    ENGINE_REFERENCE_DEBUG_OVERRIDES(ENGINE_OVERRIDE_COUNT)
#undef ENGINE_OVERRIDE_COUNT

    return count;
}

void ClearConflictingOverrides(RenderingOverrides& overrides, const RenderingSettings& previousSaved,
    const RenderingSettings& newSaved)
{
#define ENGINE_CLEAR_CONFLICTING_OVERRIDE(name, member) \
    if(overrides.name && \
        (!(previousSaved.member == newSaved.member) || *overrides.name == newSaved.member)) \
    { \
        overrides.name.reset(); \
    }
    ENGINE_RENDERING_SETTING_OVERRIDES(ENGINE_CLEAR_CONFLICTING_OVERRIDE)
#undef ENGINE_CLEAR_CONFLICTING_OVERRIDE
}
