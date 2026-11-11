#pragma once

// AIR.4F F4.4.1: the scene's mist appearance, authored in the Air window and stored in the scene (SceneDocument::mistRendering). The
// Engine builds each frame's AirMistRenderRequest from it; the mist simulation, Mist Emitters, the Editor's view choices (Show mist,
// quality preset) and the renderer's implementation switches are not part of it. A scene without the block uses these defaults,
// which are the values every scene had before the block existed. See Documentation/Air/AirArchitecture.md, AIR.4F F4.4.1.

#include <algorithm>
#include <cmath>

struct MistRenderSettings
{
    // Extinction multiplier (EditorMistPass::ExtinctionPerDensity per unit of mist density at 1).
    static constexpr float MaximumOpacity = 4.0f;
    // F4.1 Worley detail.
    static constexpr float MinimumDetailScale = 0.5f;
    static constexpr float MaximumDetailScale = 32.0f;
    // F4.4 curl warp.
    static constexpr float MaximumCurlStrength = 2.0f;
    static constexpr float MinimumCurlScale = 1.0f;
    static constexpr float MaximumCurlScale = 32.0f;
    // F4.4.2 wisps.
    static constexpr float MinimumWispScale = 2.0f;
    static constexpr float MaximumWispScale = 32.0f;
    // F4.3 lighting: the Henyey-Greenstein anisotropy's magnitude.
    static constexpr float MaximumAnisotropy = 0.9f;

    float opacity = 1.0f;

    bool detail = true;
    float detailStrength = 0.5f;
    float detailScale = 4.0f;

    bool curl = false;
    float curlStrength = 0.4f;
    float curlScale = 6.0f;
    float curlFlow = 1.0f;

    bool wisps = false;
    float wispStrength = 0.6f;
    float wispScale = 8.0f;

    // F4.4.2e density-dependent detail: finer, sharper wisps in thin mist and softer, broader detail in dense mist (0 to 1). Off by
    // default until its appearance is validated.
    bool densityDetail = false;
    float densityDetailStrength = 1.0f;

    bool lighting = true;
    bool sunShadows = true;
    float scattering = 1.0f;
    float anisotropy = 0.3f;
    float ambient = 0.0f;

    bool operator==(const MistRenderSettings&) const = default;
};

// Every value finite and inside its control's range; a non-finite value takes its default.
inline MistRenderSettings SanitizeMistRenderSettings(MistRenderSettings settings) noexcept
{
    const MistRenderSettings defaults;
    const auto bound = [](float& value, float fallback, float minimum, float maximum)
    {
        value = std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    };
    bound(settings.opacity, defaults.opacity, 0.0f, MistRenderSettings::MaximumOpacity);
    bound(settings.detailStrength, defaults.detailStrength, 0.0f, 1.0f);
    bound(settings.detailScale, defaults.detailScale, MistRenderSettings::MinimumDetailScale, MistRenderSettings::MaximumDetailScale);
    bound(settings.curlStrength, defaults.curlStrength, 0.0f, MistRenderSettings::MaximumCurlStrength);
    bound(settings.curlScale, defaults.curlScale, MistRenderSettings::MinimumCurlScale, MistRenderSettings::MaximumCurlScale);
    bound(settings.curlFlow, defaults.curlFlow, 0.0f, 1.0f);
    bound(settings.wispStrength, defaults.wispStrength, 0.0f, 1.0f);
    bound(settings.wispScale, defaults.wispScale, MistRenderSettings::MinimumWispScale, MistRenderSettings::MaximumWispScale);
    bound(settings.densityDetailStrength, defaults.densityDetailStrength, 0.0f, 1.0f);
    bound(settings.scattering, defaults.scattering, 0.0f, 1.0f);
    bound(settings.anisotropy, defaults.anisotropy, -MistRenderSettings::MaximumAnisotropy, MistRenderSettings::MaximumAnisotropy);
    bound(settings.ambient, defaults.ambient, 0.0f, 1.0f);
    return settings;
}

// Whether the settings are already sanitized (an edit is accepted only then).
inline bool ValidateMistRenderSettings(const MistRenderSettings& settings) noexcept
{
    return SanitizeMistRenderSettings(settings) == settings;
}
