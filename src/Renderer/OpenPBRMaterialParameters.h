#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

// CPU-side helpers for Engine's OpenPBR Surface materials (OpenPBR 1.1.2; Documentation/Rendering/
// OpenPBR_Material_Gap_Analysis.md): the derived GPU terms that depend only on untextured parameters, the conversion
// of a complex conductor IOR (n, k) to OpenPBR's F82-tint metal colours, and a table of real-metal presets.
namespace OpenPBRMaterialParameters
{
    // OpenPBR defaults for the parameters Engine added with the OpenPBR material (schema 2).
    inline constexpr float DefaultCoatIor = 1.6f;
    inline constexpr float DefaultFuzzRoughness = 0.5f;
    inline constexpr float DefaultThinFilmThickness = 0.5f;
    inline constexpr float DefaultThinFilmIor = 1.4f;

    // Bounds that keep the derived terms finite. OpenPBR allows IOR in (0, inf); 1/1000 and 1000 bound every real
    // material, and the shaders treat the result as a ratio.
    inline constexpr float MinimumIor = 1.0e-3f;
    inline constexpr float MaximumIor = 1.0e3f;

    inline float SchlickF0FromIorRatio(float ratio)
    {
        const float difference = ratio - 1.0f;
        const float sum = ratio + 1.0f;
        return (difference * difference) / (std::max)(sum * sum, 1.0e-12f);
    }

    // The base dielectric's Schlick F0 seen from outside, with OpenPBR's specular_weight modulation and the coat's
    // effect on the surrounding IOR. The IOR ratio blends between the ambient (n = 1) and the coat by coat weight
    // (OpenPBR Eq. 72), inverting the coat-to-base ratio where the coat is denser than the base, the TIR fix of
    // OpenPBR Eq. 73. specular_weight scales the normal-incidence reflectivity (OpenPBR Eq. 30), clamped to 1.
    inline float DielectricF0(float specularIor, float specularWeight, float coatWeight, float coatIor)
    {
        const float baseIor = std::clamp(specularIor, MinimumIor, MaximumIor);
        const float clampedCoatIor = std::clamp(coatIor, MinimumIor, MaximumIor);
        const float coatRatio = clampedCoatIor > baseIor ? clampedCoatIor / baseIor : baseIor / clampedCoatIor;
        const float coverage = std::clamp(coatWeight, 0.0f, 1.0f);
        const float ratio = baseIor + (coatRatio - baseIor) * coverage;
        const float weight = (std::max)(specularWeight, 0.0f);
        return (std::min)(weight * SchlickF0FromIorRatio(ratio), 1.0f);
    }

    inline float CoatF0(float coatIor)
    {
        return SchlickF0FromIorRatio(std::clamp(coatIor, MinimumIor, MaximumIor));
    }

    // Exact unpolarized dielectric Fresnel for the IOR ratio eta = n_t / n_i; 1 under total internal reflection.
    inline float DielectricFresnel(float cosine, float eta)
    {
        const float c = std::clamp(cosine, 0.0f, 1.0f);
        const float gSquared = eta * eta - 1.0f + c * c;

        if(gSquared <= 0.0f)
        {
            return 1.0f;
        }

        const float g = std::sqrt(gSquared);
        const float a = (g - c) / (g + c);
        const float b = ((g + c) * c - 1.0f) / ((g - c) * c + 1.0f);
        return 0.5f * a * a * (1.0f + b * b);
    }

    // The coat's internal diffuse reflection coefficient for a rough base, K_r = 1 - (1 - E_F) / eta^2, with E_F the
    // hemispherical (cosine-weighted) average of the coat Fresnel (OpenPBR coat darkening, Eq. 81).
    inline float CoatInternalReflection(float coatIor)
    {
        const float eta = std::clamp(coatIor, MinimumIor, MaximumIor);
        constexpr unsigned int sampleCount = 256;
        float integral = 0.0f;

        for(unsigned int sample = 0; sample < sampleCount; ++sample)
        {
            // Midpoint integration over mu = cos(theta).
            const float mu = (static_cast<float>(sample) + 0.5f) / static_cast<float>(sampleCount);
            integral += DielectricFresnel(mu, eta) * mu;
        }

        const float averageFresnel = 2.0f * integral / static_cast<float>(sampleCount);
        return std::clamp(1.0f - (1.0f - averageFresnel) / (eta * eta), 0.0f, 1.0f);
    }

    // Conductor Fresnel for a complex IOR n + ik (one channel).
    inline float ConductorFresnel(float cosTheta, float n, float k)
    {
        cosTheta = std::clamp(cosTheta, 0.0f, 1.0f);

        const float cosThetaSquared = cosTheta * cosTheta;
        const float sinThetaSquared = 1.0f - cosThetaSquared;
        const float nSquared = n * n;
        const float kSquared = k * k;

        const float t0 = nSquared - kSquared - sinThetaSquared;
        const float a2PlusB2 = std::sqrt(t0 * t0 + 4.0f * nSquared * kSquared);
        const float t1 = a2PlusB2 + cosThetaSquared;
        const float a = std::sqrt((std::max)(0.5f * (a2PlusB2 + t0), 0.0f));

        const float t2 = 2.0f * cosTheta * a;
        const float Rs = (t1 - t2) / (t1 + t2);
        const float t3 = cosThetaSquared * a2PlusB2 + sinThetaSquared * sinThetaSquared;
        const float t4 = t2 * sinThetaSquared;
        const float Rp = Rs * (t3 - t4) / (t3 + t4);

        return 0.5f * (Rp + Rs);
    }

    struct MetalColors
    {
        std::array<float, 3> baseColor{};
        std::array<float, 3> edgeTint{};
    };

    // The F82-tint colours that reproduce a complex IOR (n, k) per channel: base_color is the normal-incidence
    // reflectance F(1), specular_color the ratio of the true reflectance at cos = 1/7 to the Schlick curve there,
    // clamped to [0, 1] since F82-tint cannot make an edge brighter than Schlick (OpenPBR metal section).
    inline MetalColors MetalColorsFromComplexIor(const std::array<float, 3>& n, const std::array<float, 3>& k)
    {
        constexpr float edgeCosine = 1.0f / 7.0f;
        MetalColors colors;

        for(size_t channel = 0; channel < 3; ++channel)
        {
            const float channelN = (std::max)(n[channel], 0.0f);
            const float channelK = (std::max)(k[channel], 0.0f);
            const float F0 = std::clamp(ConductorFresnel(1.0f, channelN, channelK), 0.0f, 1.0f);
            const float oneMinusMu = 1.0f - edgeCosine;
            const float schlickAtEdge = F0 + (1.0f - F0) * std::pow(oneMinusMu, 5.0f);
            const float actualAtEdge = ConductorFresnel(edgeCosine, channelN, channelK);

            colors.baseColor[channel] = F0;
            colors.edgeTint[channel] = schlickAtEdge > 0.0f ? std::clamp(actualAtEdge / schlickAtEdge, 0.0f, 1.0f) : 1.0f;
        }

        return colors;
    }

    // Fitted F82-tint colours for real metals in linear sRGB, from the OpenPBR technical companion paper (Portsmouth,
    // Kutz, Hill, arXiv 2512.23696, Appendix D), clamped to [0, 1].
    struct MetalPreset
    {
        std::string_view name;
        std::array<float, 3> baseColor;
        std::array<float, 3> edgeTint;
    };

    inline constexpr std::array<MetalPreset, 18> MetalPresets = {{
        {"Aluminium", {0.916f, 0.923f, 0.924f}, {0.910f, 0.936f, 0.959f}},
        {"Brass", {0.962f, 0.713f, 0.464f}, {0.971f, 0.994f, 1.000f}},
        {"Chromium", {0.654f, 0.685f, 0.701f}, {0.688f, 0.728f, 0.798f}},
        {"Cobalt", {0.699f, 0.704f, 0.671f}, {0.727f, 0.772f, 0.823f}},
        {"Copper", {0.932f, 0.623f, 0.522f}, {0.982f, 0.947f, 0.945f}},
        {"Gold", {1.000f, 0.773f, 0.307f}, {0.971f, 1.000f, 0.994f}},
        {"Iridium", {0.745f, 0.734f, 0.704f}, {0.759f, 0.781f, 0.810f}},
        {"Iron", {0.530f, 0.513f, 0.494f}, {0.765f, 0.767f, 0.802f}},
        {"Lead", {0.626f, 0.640f, 0.693f}, {0.758f, 0.773f, 0.799f}},
        {"Magnesium", {0.956f, 0.953f, 0.950f}, {0.954f, 0.964f, 0.977f}},
        {"Mercury", {0.781f, 0.780f, 0.778f}, {0.813f, 0.852f, 0.902f}},
        {"Nickel", {0.697f, 0.641f, 0.563f}, {0.815f, 0.834f, 0.871f}},
        {"Palladium", {0.734f, 0.704f, 0.662f}, {0.811f, 0.836f, 0.872f}},
        {"Platinum", {0.765f, 0.730f, 0.676f}, {0.793f, 0.815f, 0.840f}},
        {"Silver", {0.991f, 0.985f, 0.974f}, {0.994f, 0.995f, 0.998f}},
        {"Steel", {0.669f, 0.639f, 0.598f}, {0.789f, 0.823f, 0.870f}},
        {"Titanium", {0.441f, 0.400f, 0.361f}, {0.865f, 0.906f, 0.946f}},
        {"Zinc", {0.808f, 0.844f, 0.865f}, {0.762f, 0.833f, 0.896f}},
    }};
}
