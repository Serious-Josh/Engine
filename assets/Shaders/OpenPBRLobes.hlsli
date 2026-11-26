#ifndef ENGINE_OPENPBR_LOBES
#define ENGINE_OPENPBR_LOBES

// Building blocks of the OpenPBR Surface lobes (OpenPBR 1.1.2; Documentation/Rendering/OpenPBR_Material_Gap_Analysis.md)
// shared by the path tracer's BSDF (PathTraceBSDF.hlsli). Every function here is self-contained: it takes resolved
// parameters and directions and touches no resource, so the BSDF decides when (and whether) each lobe runs.
//
// - F82-tint metal Fresnel (Kutz et al. 2021, the OpenPBR metal model).
// - OpenPBR roughness/anisotropy to GGX alphas, and the anisotropic GGX D, height-correlated Smith G and bounded VNDF
//   sampling (Eto and Tokuyoshi 2023), which reduce to Engine's isotropic forms for equal alphas.
// - The Zeltner et al. 2022 multiple-scattering sheen (OpenPBR fuzz) as an LTC lobe, with the analytic fits published
//   in MaterialX (Apache-2.0, libraries/pbrlib/genglsl/lib/mx_microfacet_sheen.glsl) of the ltc-sheen tables
//   (Apache-2.0).
// - Thin-film interference (Belcour and Barla 2017, the OpenPBR thin-film model) for a film over a dielectric or an
//   F82-tint metal, with the paper's Gaussian fits of the CIE sensitivity in the Fourier domain.
// - Octahedral packing for the unit vectors a surface carries only for the coat and anisotropy.
//
// All directions are unit vectors pointing away from the surface.

#include "Common.hlsli"

static const float OPENPBR_PI = 3.14159265358979323846f;
// The F82-tint grazing reference cosine, cos(82 degrees) ~ 1/7 (OpenPBR metal section).
static const float OPENPBR_F82_COSINE = 1.0f / 7.0f;
// Zeltner sheen fits are defined for roughness in [0.01, 1] (the ltc-sheen table range).
static const float OPENPBR_FUZZ_MINIMUM_ROUGHNESS = 0.01f;

// -----------------------------------------------------------------------------
// Octahedral unit-vector packing (two biased 16-bit fields in one uint; 0 is never a valid encoding)
// -----------------------------------------------------------------------------

// The value a surface field holds when the vector is unavailable (the builder had no smooth normal or tangent).
static const uint OPENPBR_PACKED_VECTOR_NONE = 0u;

float2 OpenPBROctahedralWrap(float2 value)
{
    float2 signs = float2(value.x >= 0.0f ? 1.0f : -1.0f, value.y >= 0.0f ? 1.0f : -1.0f);
    return (1.0f - abs(value.yx)) * signs;
}

// Each component is stored as round(e * 32767) + 32768, in [1, 65535], so the packed value is never 0.
uint OpenPBRPackUnitVector(float3 direction)
{
    float3 scaled = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1.0e-20f);
    float2 encoded = scaled.z >= 0.0f ? scaled.xy : OpenPBROctahedralWrap(scaled.xy);
    uint2 quantized = uint2(int2(round(clamp(encoded, -1.0f, 1.0f) * 32767.0f)) + 32768);
    return quantized.x | (quantized.y << 16u);
}

float3 OpenPBRUnpackUnitVector(uint packed)
{
    int2 quantized = int2(int(packed & 0xFFFFu), int(packed >> 16u)) - 32768;
    float2 encoded = float2(quantized) / 32767.0f;
    float3 direction = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));

    if(direction.z < 0.0f)
    {
        direction.xy = OpenPBROctahedralWrap(direction.xy);
    }

    return normalize(direction);
}

// -----------------------------------------------------------------------------
// Fresnel
// -----------------------------------------------------------------------------

float3 OpenPBRSchlick(float cosine, float3 F0)
{
    float factor = pow(1.0f - saturate(cosine), 5.0f);
    return F0 + (1.0f - F0) * factor;
}

// The F82-tint correction coefficient b: F82(mu) = Schlick(mu) - b mu (1 - mu)^6, chosen so that
// F82(1/7) = edgeTint * Schlick(1/7). An edge tint above 1 is clamped (the edge cannot be brighter than Schlick).
float3 OpenPBRMetalF82Coefficient(float3 F0, float3 edgeTint)
{
    const float mu = OPENPBR_F82_COSINE;
    float oneMinusMu = 1.0f - mu;
    float denominator = mu * oneMinusMu * oneMinusMu * oneMinusMu * oneMinusMu * oneMinusMu * oneMinusMu;
    float3 schlickAtEdge = OpenPBRSchlick(mu, F0);
    return schlickAtEdge * (1.0f - saturate(edgeTint)) / denominator;
}

// F_metal(mu) = clamp(specularWeight * F82(mu), 0, 1) (OpenPBR Eq. 66).
float3 OpenPBRMetalFresnel(float cosine, float3 F0, float3 f82Coefficient, float specularWeight)
{
    float mu = saturate(cosine);
    float oneMinusMu = 1.0f - mu;
    float oneMinusMuSquared = oneMinusMu * oneMinusMu;
    float sixthPower = oneMinusMuSquared * oneMinusMuSquared * oneMinusMuSquared;
    float3 f82 = OpenPBRSchlick(mu, F0) - f82Coefficient * mu * sixthPower;
    return saturate(specularWeight * f82);
}

// The hemispherical average 2 int F82 mu dmu = F0 + (1 - F0) / 21 - b / 126 (OpenPBR Eq. 67), with the weight.
float3 OpenPBRMetalAverageFresnel(float3 F0, float3 f82Coefficient, float specularWeight)
{
    return saturate(specularWeight * (F0 + (1.0f - F0) / 21.0f - f82Coefficient / 126.0f));
}

// Unpolarized dielectric Fresnel at cosine (incident side) for the IOR ratio eta = n_transmitted / n_incident; 1 under
// total internal reflection.
float OpenPBRDielectricFresnel(float cosine, float eta)
{
    float c = saturate(cosine);
    float gSquared = eta * eta - 1.0f + c * c;

    if(gSquared <= 0.0f)
    {
        return 1.0f;
    }

    float g = sqrt(gSquared);
    float a = (g - c) / (g + c);
    float b = ((g + c) * c - 1.0f) / ((g - c) * c + 1.0f);
    return 0.5f * a * a * (1.0f + b * b);
}

// -----------------------------------------------------------------------------
// GGX with OpenPBR anisotropy
// -----------------------------------------------------------------------------

// OpenPBR's (roughness, anisotropy) to (alpha_t, alpha_b) (OpenPBR Eq. 20): alpha_t^2 + alpha_b^2 = 2 alpha^2.
// minimumRoughness bounds the perceptual roughness first, so an isotropic surface gets Engine's clamped alpha exactly.
float2 OpenPBRAnisotropicAlphas(float roughness, float anisotropy, float minimumRoughness)
{
    float perceptual = max(roughness, minimumRoughness);
    float alpha = perceptual * perceptual;
    float a = saturate(anisotropy);

    if(a <= 0.0f)
    {
        return float2(alpha, alpha);
    }

    float oneMinusA = 1.0f - a;
    float alphaTangent = alpha * sqrt(2.0f / (1.0f + oneMinusA * oneMinusA));
    // The bitangent alpha keeps the minimum alpha so the lobe never becomes a delta along one axis.
    float minimumAlpha = minimumRoughness * minimumRoughness;
    return float2(min(alphaTangent, 1.0f), max(oneMinusA * alphaTangent, minimumAlpha));
}

// A direction in the surface's (tangent, bitangent, normal) frame.
float3 OpenPBRToLocal(float3 direction, float3 tangent, float3 bitangent, float3 normal)
{
    return float3(dot(direction, tangent), dot(direction, bitangent), dot(direction, normal));
}

float3 OpenPBRFromLocal(float3 local, float3 tangent, float3 bitangent, float3 normal)
{
    return local.x * tangent + local.y * bitangent + local.z * normal;
}

// Anisotropic GGX D for a local micronormal: 1 / (PI at ab (x^2/at^2 + y^2/ab^2 + z^2)^2). Written as
// at ab / (PI (ab^2 x^2 + at^2 y^2 + at^2 ab^2 z^2)^2 / (at ab)^... ) in the stable cross-product spirit: the
// isotropic case reduces to alpha^2 / (PI (|n x h|^2 + alpha^2 (n.h)^2)^2).
float OpenPBRDistributionAnisotropic(float3 localHalf, float2 alphas)
{
    float x = localHalf.x / alphas.x;
    float y = localHalf.y / alphas.y;
    float z = localHalf.z;
    float denominator = x * x + y * y + z * z;
    return 1.0f / max(OPENPBR_PI * alphas.x * alphas.y * denominator * denominator, 1.0e-30f);
}

// Height-correlated Smith's root term for one local direction: sqrt(z^2 + at^2 x^2 + ab^2 y^2). For equal alphas this
// is GeometrySmithGGXRoot's sqrt(mu^2 (1 - alpha^2) + alpha^2).
float OpenPBRSmithRootAnisotropic(float3 localDirection, float2 alphas)
{
    float x = alphas.x * localDirection.x;
    float y = alphas.y * localDirection.y;
    return sqrt(max(localDirection.z * localDirection.z + x * x + y * y, 0.0f));
}

// Height-correlated Smith G2 = 2 mu_l mu_v / (mu_v root(l) + mu_l root(v)), the form GeometrySmithGGXPrepared uses.
float OpenPBRSmithG2FromRoots(float normalDotLight, float normalDotView, float lightRoot, float viewRoot)
{
    return 2.0f * normalDotLight * normalDotView /
        max(normalDotView * lightRoot + normalDotLight * viewRoot, 1.0e-6f);
}

// The bounded-cap parameter k of bounded VNDF sampling (Eto and Tokuyoshi 2023, Eq. 5 and 6) for a local view above
// the surface: the spherical cap of Dupuy and Benyoub 2023 shrunk so fewer reflections leave below the horizon.
float OpenPBRBoundedVndfCap(float3 localView, float2 alphas)
{
    float a = saturate(min(alphas.x, alphas.y));
    float s = 1.0f + length(localView.xy);
    float aSquared = a * a;
    float sSquared = s * s;
    return (1.0f - aSquared) * sSquared / (sSquared + aSquared * localView.z * localView.z);
}

// Bounded VNDF sampling of a reflection direction for a local view (z > 0). Returns the local reflected direction.
float3 OpenPBRSampleBoundedVndf(float3 localView, float2 alphas, float2 random)
{
    float3 stretchedView = normalize(float3(localView.xy * alphas, localView.z));
    float phi = 2.0f * OPENPBR_PI * random.x;
    float k = OpenPBRBoundedVndfCap(localView, alphas);
    float lowerBound = k * stretchedView.z;
    float z = (1.0f - random.y) * (1.0f + lowerBound) - lowerBound;
    float sinTheta = sqrt(saturate(1.0f - z * z));
    float3 stretchedOutgoing = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);

    float3 stretchedHalf = stretchedView + stretchedOutgoing;
    float3 localHalf = normalize(float3(stretchedHalf.xy * alphas, max(stretchedHalf.z, 0.0f)));
    return 2.0f * dot(localView, localHalf) * localHalf - localView;
}

// The solid-angle density of OpenPBRSampleBoundedVndf for a local view (z > 0) and reflected direction: D / (2 (k v_z +
// root(v))) (Eto and Tokuyoshi 2023, Eq. 8 with the reflection Jacobian). 0 for a degenerate half vector.
float OpenPBRBoundedVndfPdf(float3 localView, float3 localLight, float2 alphas)
{
    float3 halfVector = localView + localLight;
    float halfLengthSquared = dot(halfVector, halfVector);

    if(halfLengthSquared <= 1.0e-12f)
    {
        return 0.0f;
    }

    float3 localHalf = halfVector * rsqrt(halfLengthSquared);

    if(dot(localView, localHalf) <= 0.0f || localHalf.z <= 0.0f)
    {
        return 0.0f;
    }

    float distribution = OpenPBRDistributionAnisotropic(localHalf, alphas);
    float k = OpenPBRBoundedVndfCap(localView, alphas);
    float viewRoot = OpenPBRSmithRootAnisotropic(localView, alphas);
    return distribution / max(2.0f * (k * localView.z + viewRoot), 1.0e-20f);
}

// -----------------------------------------------------------------------------
// Coat
// -----------------------------------------------------------------------------

// The cosine of a ray refracted into the coat (OpenPBR view-dependent absorption): sqrt(1 - (1 - mu^2) / eta^2).
float OpenPBRRefractedCosine(float cosine, float coatIor)
{
    float mu = saturate(cosine);
    float inverseEta = 1.0f / max(coatIor, 1.0e-3f);
    return sqrt(saturate(1.0f - (1.0f - mu * mu) * inverseEta * inverseEta));
}

// The coat's absorption along the light and view rays, T^(1/mu_i^t + 1/mu_o^t) with T^2 = coat_color at normal
// incidence (OpenPBR view-dependent absorption), from log(coat_color) / 2.
float3 OpenPBRCoatTransmittance(float3 halfLogCoatColor, float refractedLightCosine, float refractedViewCosine)
{
    float pathLength = 1.0f / max(refractedLightCosine, 1.0e-3f) + 1.0f / max(refractedViewCosine, 1.0e-3f);
    return exp(halfLogCoatColor * pathLength);
}

// -----------------------------------------------------------------------------
// Fuzz (Zeltner et al. 2022 sheen, OpenPBR fuzz)
// -----------------------------------------------------------------------------

float OpenPBRFuzzRoughness(float roughness)
{
    return clamp(roughness, OPENPBR_FUZZ_MINIMUM_ROUGHNESS, 1.0f);
}

// The fuzz layer's directional albedo E_fuzz(mu_o, roughness): MaterialX's Gaussian fit of the ltc-sheen table.
float OpenPBRFuzzAlbedo(float normalDotView, float roughness)
{
    float x = saturate(normalDotView);
    float y = OpenPBRFuzzRoughness(roughness);
    float s = y * (0.0206607f + 1.58491f * y) / (0.0379424f + y * (1.32227f + y));
    float m = y * (-0.193854f + y * (-1.14885f + y * (1.7932f - 0.95943f * y * y))) / (0.046391f + y);
    float o = y * (0.000654023f + (-0.0207818f + 0.119681f * y) * y) / (1.26264f + y * (-1.92021f + y));
    float normalized = (x - m) / s;
    return saturate(exp(-0.5f * normalized * normalized) / (s * sqrt(2.0f * OPENPBR_PI)) + o);
}

// The LTC inverse-matrix coefficients (A, B) of M^-1 = [[A 0 B] [0 A 0] [0 0 1]]: MaterialX's rational fits.
float2 OpenPBRFuzzInverseLtc(float normalDotView, float roughness)
{
    float x = saturate(normalDotView);
    float y = OpenPBRFuzzRoughness(roughness);
    float a = (2.58126f * x + 0.813703f * y) * y / (1.0f + 0.310327f * x * x + 2.60994f * x * y);
    float b = sqrt(1.0f - x) * (y - 1.0f) * y * y * y / (0.0000254053f + 1.71228f * x - 1.71506f * x * y +
        1.34174f * y * y);
    return float2(a, b);
}

// The LTC lobe's frame: X in the plane of the view and the normal, Z the normal.
void OpenPBRFuzzFrame(float3 normal, float3 view, float normalDotView, out float3 axisX, out float3 axisY)
{
    float3 inPlane = view - normal * normalDotView;
    float lengthSquared = dot(inPlane, inPlane);

    if(lengthSquared > 1.0e-12f)
    {
        axisX = inPlane * rsqrt(lengthSquared);
    }
    else
    {
        // The view is along the normal: any frame (the lobe is then symmetric).
        float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        axisX = normalize(cross(helper, normal));
    }

    axisY = cross(normal, axisX);
}

// The LTC distribution D(omega_i) of the fuzz lobe from its view terms (the frame's X axis, Y = N x X, and the
// inverse-matrix coefficients), a normalized density over directions (OpenPBR Eq. 90: mu_i f_fuzz = fuzz_color E_fuzz D).
// Also the lobe's sampling density.
float OpenPBRFuzzDistributionFromTerms(float3 normal, float3 axisX, float2 inverseLtc, float3 light)
{
    float3 axisY = cross(normal, axisX);
    float3 local = float3(dot(light, axisX), dot(light, axisY), dot(light, normal));
    float3 original = float3(inverseLtc.x * local.x + inverseLtc.y * local.z, inverseLtc.x * local.y, local.z);
    float lengthSquared = dot(original, original);

    if(!(lengthSquared > 1.0e-20f) || original.z <= 0.0f)
    {
        return 0.0f;
    }

    float ratio = inverseLtc.x / lengthSquared;
    return original.z / OPENPBR_PI * ratio * ratio;
}

float OpenPBRFuzzDistribution(float3 normal, float3 view, float3 light, float normalDotView, float roughness)
{
    float3 axisX;
    float3 axisY;
    OpenPBRFuzzFrame(normal, view, normalDotView, axisX, axisY);
    return OpenPBRFuzzDistributionFromTerms(normal, axisX, OpenPBRFuzzInverseLtc(normalDotView, roughness), light);
}

// Samples the fuzz LTC lobe from its view terms: a cosine sample pushed through M = (M^-1)^-1. Returns the world
// direction.
float3 OpenPBRSampleFuzzFromTerms(float3 normal, float3 axisX, float2 inverseLtc, float2 random)
{
    float3 axisY = cross(normal, axisX);
    float radius = sqrt(random.x);
    float angle = 2.0f * OPENPBR_PI * random.y;
    float3 original = float3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0f - random.x, 0.0f)));
    float inverseA = 1.0f / max(inverseLtc.x, 1.0e-7f);
    float3 transformed = float3(original.x * inverseA - original.z * inverseLtc.y * inverseA, original.y * inverseA,
        original.z);
    transformed = normalize(transformed);
    return transformed.x * axisX + transformed.y * axisY + transformed.z * normal;
}

float3 OpenPBRSampleFuzz(float3 normal, float3 view, float normalDotView, float roughness, float2 random)
{
    float3 axisX;
    float3 axisY;
    OpenPBRFuzzFrame(normal, view, normalDotView, axisX, axisY);
    return OpenPBRSampleFuzzFromTerms(normal, axisX, OpenPBRFuzzInverseLtc(normalDotView, roughness), random);
}

// -----------------------------------------------------------------------------
// Thin film (Belcour and Barla 2017)
// -----------------------------------------------------------------------------

// Polarized (parallel, perpendicular) dielectric reflectances at a cosine for eta = n_t / n_i.
float2 OpenPBRDielectricFresnelPolarized(float cosine, float eta)
{
    float cosineSquared = saturate(cosine) * saturate(cosine);
    float sineSquared = 1.0f - cosineSquared;
    float t0 = max(eta * eta - sineSquared, 0.0f);
    float t1 = t0 + cosineSquared;
    float t2 = 2.0f * sqrt(t0) * saturate(cosine);
    float perpendicular = (t1 - t2) / max(t1 + t2, 1.0e-20f);
    float t3 = cosineSquared * t0 + sineSquared * sineSquared;
    float t4 = t2 * sineSquared;
    float parallel = perpendicular * (t3 - t4) / max(t3 + t4, 1.0e-20f);
    return float2(parallel, perpendicular);
}

// The Fourier-domain Gaussian fits of the CIE 1931 XYZ sensitivity (Belcour and Barla 2017, supplemental): the
// spectral integral of cos(2 PI opd / lambda + shift) against the XYZ matching functions, opd in metres.
float3 OpenPBRThinFilmSensitivity(float opticalPathDifference, float3 shift)
{
    float phase = 2.0f * OPENPBR_PI * opticalPathDifference;
    const float3 value = float3(5.4856e-13f, 4.4201e-13f, 5.2481e-13f);
    const float3 position = float3(1.6810e+06f, 1.7953e+06f, 2.2084e+06f);
    const float3 variance = float3(4.3278e+09f, 9.3046e+09f, 6.6121e+09f);
    float3 xyz = value * sqrt(2.0f * OPENPBR_PI * variance) * cos(position * phase + shift) *
        exp(-variance * phase * phase);
    xyz.x += 9.7470e-14f * sqrt(2.0f * OPENPBR_PI * 4.5282e+09f) * cos(2.2399e+06f * phase + shift.x) *
        exp(-4.5282e+09f * phase * phase);
    return xyz / 1.0685e-7f;
}

// The RGB reflectance of a thin film (thickness in micrometres, IOR filmIor) between an outer medium of IOR outerIor
// and a base whose reflectance inside the film is baseReflectanceInside (unpolarized, at the refracted cosine) with
// interface phase shift basePhase (0 or PI per channel). cosine is the incident cosine at the microfacet. Two Airy
// orders, as Belcour and Barla's reference implementation. The film only redistributes energy; the result is clamped to
// [0, 1].
float3 OpenPBRThinFilmReflectance(float cosine, float outerIor, float filmIor, float thickness,
    float3 baseReflectanceInside, float3 basePhase, float refractedCosine)
{
    // XYZ to CIE 1931 RGB under the equal-energy illuminant, so a spectrally flat result stays neutral.
    const float3x3 xyzToRgb = float3x3(
        2.3706743f, -0.9000405f, -0.4706338f,
        -0.5138850f, 1.4253036f, 0.0885814f,
        0.0052982f, -0.0146949f, 1.0093968f);

    float etaFilm = max(filmIor, outerIor) / outerIor;
    float2 outerReflectance = OpenPBRDielectricFresnelPolarized(cosine, etaFilm);

    if(refractedCosine <= 0.0f)
    {
        outerReflectance = float2(1.0f, 1.0f);
    }

    float2 outerTransmittance = 1.0f - outerReflectance;

    // Reflection phase at the outer interface seen from inside the film: Brewster flips the parallel component.
    float brewsterCosine = cos(atan(etaFilm));
    float2 outerPhase = float2(cosine < brewsterCosine ? 0.0f : OPENPBR_PI, OPENPBR_PI);

    float opticalPathDifference = 2.0f * max(filmIor, outerIor) * refractedCosine * thickness * 1.0e-6f;
    float3 intensity = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for(uint polarization = 0u; polarization < 2u; ++polarization)
    {
        float reflectance12 = outerReflectance[polarization];
        float transmittance121 = outerTransmittance[polarization];
        float3 amplitude = sqrt(max(reflectance12 * baseReflectanceInside, 0.0f));

        // The m = 0 term (DC amplitude).
        float3 direct = transmittance121 * transmittance121 * baseReflectanceInside /
            max(1.0f - reflectance12 * baseReflectanceInside, 1.0e-6f);
        intensity += reflectance12 + direct;

        // The m = 1, 2 terms (pairs of Diracs).
        float3 coefficient = direct - transmittance121;

        [unroll]
        for(uint order = 1u; order <= 2u; ++order)
        {
            coefficient *= amplitude;
            float3 sensitivity = 2.0f * OpenPBRThinFilmSensitivity(float(order) * opticalPathDifference,
                float(order) * (basePhase + outerPhase[polarization]));
            intensity += coefficient * sensitivity;
        }
    }

    return saturate(mul(xyzToRgb, 0.5f * intensity));
}

#endif
