#ifndef ENGINE_PATH_TRACE_CAUSTIC_CLASSIFICATION
#define ENGINE_PATH_TRACE_CAUSTIC_CLASSIFICATION

// R.9I.3A Stage 1: pure complete-path classification. Intentionally not included by any
// production shader. No light tracing, camera subtraction, resource ownership or settings.
//
// Input contract (both traversals):
// - All records belong to one immutable publication; 64-bit identities are lossless word pairs.
// - Hit material values come from ResolveHitMaterial + texture/ARM resolution at the actual UV,
//   before or after final saturation. materialIndex is the resolved slot, never a default guess.
// - FlatVerified is a geometry/publication certificate: nondegenerate face normals equal shading
//   normals throughout the supported primitive, no smoothing or normal mapping. It is NOT a
//   near-equality dot test. Unknown evidence must be zero. Current hit payloads do not supply it.
// - Cosines are dot(unit face normal, unit direction away from this vertex), with normals faced
//   to the common reflection hemisphere BEFORE horizon correction. endpoint means light at the
//   reflector and pinhole at the receiver; other means the other surface. Supply the same
//   geometric path facts on reversal, not negated incoming-ray cosines or corrected normals.
// - No RNG choice, proposal identity, visibility result or splat count is an input.
// This classifies eligibility only; visibility/PDF/measurement and numerical equivalence remain
// separate obligations. CameraOwned is mandatory for unsupported or unproven input.

#ifdef __cplusplus
#include <bit>
#include <cstdint>
#define PT_CAUSTIC_UINT std::uint32_t
#define PT_CAUSTIC_FUNCTION inline
#else
#define PT_CAUSTIC_UINT uint
#define PT_CAUSTIC_FUNCTION
#endif

static const PT_CAUSTIC_UINT CausticCameraOwned = 0u;
static const PT_CAUSTIC_UINT CausticGlossyEligible = 1u;
// Delta adds previously unsupported transport; it does not authorize removing a camera NEE term.
static const PT_CAUSTIC_UINT CausticDeltaEligible = 2u;
static const PT_CAUSTIC_UINT CausticInvalidIdentity = 0xFFFFFFFFu;
static const PT_CAUSTIC_UINT CausticReferencePrimary = 0u;
static const PT_CAUSTIC_UINT CausticPointLight = 0u;
static const PT_CAUSTIC_UINT CausticFullReceiverBSDF = 1u;
static const PT_CAUSTIC_UINT CausticWholeInstance = 1u;
static const PT_CAUSTIC_UINT CausticPrimitiveRange = 2u;
static const PT_CAUSTIC_UINT CausticLightFirst = 0u;
static const PT_CAUSTIC_UINT CausticCameraFirst = 1u;
static const PT_CAUSTIC_UINT CausticMaterialResolved = 1u;
static const PT_CAUSTIC_UINT CausticFlatVerified = 2u;
static const PT_CAUSTIC_UINT CausticNormalMapped = 4u;
static const PT_CAUSTIC_UINT CausticSmoothed = 8u;
static const float CausticDeltaRoughness = 0.001f;
static const float CausticMinimumGGXRoughness = 0.045f;
static const float CausticMinimumCosine = 0.02f;

struct CausticIdentity
{
    PT_CAUSTIC_UINT low;
    PT_CAUSTIC_UINT high;
};

// These are publisher identities, not hashes of partially observed state. Never truncate an
// existing renderer revision. Texture identity must change for content/residency/LOD changes;
// geometry covers deformation/transforms, and instance covers membership/slot remapping.
struct CausticPublication
{
    CausticIdentity sceneLifetime;
    CausticIdentity instance;
    CausticIdentity geometry;
    CausticIdentity material;
    CausticIdentity texture;
    CausticIdentity light;
    CausticIdentity registration;
};

struct CausticClassificationConfig
{
    CausticPublication publication;
    PT_CAUSTIC_UINT active;
    PT_CAUSTIC_UINT primary; // PathTracePrimary values, not the shader's split-pass dispatch codes.
    PT_CAUSTIC_UINT maxPathDepth;
    PT_CAUSTIC_UINT unsupportedFeatures; // Nonzero for cache, reconstruction, upscaling, etc.
    PT_CAUSTIC_UINT selectedPointLight;
    PT_CAUSTIC_UINT registeredInstance;
    PT_CAUSTIC_UINT registrationMode; // 0 = empty; whole instance or contiguous primitive range.
    PT_CAUSTIC_UINT firstPrimitive;
    PT_CAUSTIC_UINT primitiveCount;
    PT_CAUSTIC_UINT receiverPolicy;
    float reflectorRoughnessThreshold; // Strict < after saturation.
    float receiverRoughnessThreshold; // Inclusive >= after saturation.
};

struct CausticClassificationHit
{
    PT_CAUSTIC_UINT instanceIndex;
    PT_CAUSTIC_UINT primitiveIndex;
    PT_CAUSTIC_UINT materialIndex;
    PT_CAUSTIC_UINT evidence;
    PT_CAUSTIC_UINT materialModel;
    float metallic;
    float roughness;
    float cosineToEndpoint;
    float cosineToOtherSurface;
};

struct CausticCompletePath
{
    CausticPublication publication;
    PT_CAUSTIC_UINT traversal;
    PT_CAUSTIC_UINT lightIndex;
    PT_CAUSTIC_UINT lightType;
    CausticClassificationHit firstSurface;
    CausticClassificationHit secondSurface;
};

PT_CAUSTIC_FUNCTION bool CausticSameIdentity(CausticIdentity left, CausticIdentity right)
{
    return left.low == right.low && left.high == right.high;
}

PT_CAUSTIC_FUNCTION bool CausticSamePublication(CausticPublication left, CausticPublication right)
{
    return CausticSameIdentity(left.sceneLifetime, right.sceneLifetime) &&
        CausticSameIdentity(left.instance, right.instance) &&
        CausticSameIdentity(left.geometry, right.geometry) &&
        CausticSameIdentity(left.material, right.material) &&
        CausticSameIdentity(left.texture, right.texture) &&
        CausticSameIdentity(left.light, right.light) &&
        CausticSameIdentity(left.registration, right.registration);
}

PT_CAUSTIC_FUNCTION bool CausticValidIdentity(CausticIdentity identity)
{
    return identity.low != 0u || identity.high != 0u;
}

PT_CAUSTIC_FUNCTION bool CausticValidPublication(CausticPublication publication)
{
    return CausticValidIdentity(publication.sceneLifetime) && CausticValidIdentity(publication.instance) &&
        CausticValidIdentity(publication.geometry) && CausticValidIdentity(publication.material) &&
        CausticValidIdentity(publication.texture) && CausticValidIdentity(publication.light) &&
        CausticValidIdentity(publication.registration);
}

PT_CAUSTIC_FUNCTION bool CausticFinite(float value)
{
#ifdef __cplusplus
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000u) != 0x7F800000u;
#else
    return (asuint(value) & 0x7F800000u) != 0x7F800000u;
#endif
}

PT_CAUSTIC_FUNCTION float CausticSaturate(float value)
{
    // Only called after finite validation, avoiding backend-dependent NaN min/max behavior.
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

PT_CAUSTIC_FUNCTION bool CausticInRegisteredDomain(
    CausticClassificationConfig config, CausticClassificationHit hit)
{
    if(hit.instanceIndex != config.registeredInstance)
    {
        return false;
    }

    if(config.registrationMode == CausticWholeInstance)
    {
        return true;
    }

    return config.registrationMode == CausticPrimitiveRange && hit.primitiveIndex >= config.firstPrimitive &&
        hit.primitiveIndex - config.firstPrimitive < config.primitiveCount;
}

PT_CAUSTIC_FUNCTION bool CausticSupportedHit(CausticClassificationHit hit)
{
    // Exact certificate, rejecting unknown bits rather than guessing future evidence semantics.
    return hit.instanceIndex != CausticInvalidIdentity && hit.primitiveIndex != CausticInvalidIdentity &&
        hit.materialIndex != CausticInvalidIdentity &&
        hit.evidence == (CausticMaterialResolved | CausticFlatVerified) && hit.materialModel <= 2u &&
        CausticFinite(hit.metallic) && CausticFinite(hit.roughness) &&
        CausticFinite(hit.cosineToEndpoint) && CausticFinite(hit.cosineToOtherSurface) &&
        hit.cosineToEndpoint >= CausticMinimumCosine && hit.cosineToEndpoint <= 1.0f &&
        hit.cosineToOtherSurface >= CausticMinimumCosine && hit.cosineToOtherSurface <= 1.0f;
}

// publication is the currently accepted immutable snapshot, independent of config/path copies.
// Zero-initialized config is inactive; all unrecognized configurations stay camera-owned.
PT_CAUSTIC_FUNCTION PT_CAUSTIC_UINT ClassifyCausticCompletePath(
    CausticClassificationConfig config, CausticPublication publication, CausticCompletePath path)
{
    if(config.active != 1u || config.primary != CausticReferencePrimary || config.maxPathDepth != 2u ||
        config.unsupportedFeatures != 0u || config.receiverPolicy != CausticFullReceiverBSDF ||
        config.selectedPointLight == CausticInvalidIdentity || config.registeredInstance == CausticInvalidIdentity ||
        !CausticValidPublication(publication) ||
        !CausticSamePublication(config.publication, publication) ||
        !CausticSamePublication(path.publication, publication))
    {
        return CausticCameraOwned;
    }

    if((config.registrationMode != CausticWholeInstance && config.registrationMode != CausticPrimitiveRange) ||
        (config.registrationMode == CausticPrimitiveRange &&
            (config.primitiveCount == 0u || config.firstPrimitive == CausticInvalidIdentity ||
                config.primitiveCount > CausticInvalidIdentity - config.firstPrimitive)) ||
        !CausticFinite(config.reflectorRoughnessThreshold) || !CausticFinite(config.receiverRoughnessThreshold) ||
        config.reflectorRoughnessThreshold <= 0.0f || config.reflectorRoughnessThreshold > 1.0f ||
        config.receiverRoughnessThreshold <= CausticDeltaRoughness || config.receiverRoughnessThreshold > 1.0f ||
        path.lightType != CausticPointLight || path.lightIndex != config.selectedPointLight ||
        (path.traversal != CausticLightFirst && path.traversal != CausticCameraFirst))
    {
        return CausticCameraOwned;
    }

    CausticClassificationHit reflector = path.firstSurface;
    CausticClassificationHit receiver = path.secondSurface;

    if(path.traversal == CausticCameraFirst)
    {
        reflector = path.secondSurface;
        receiver = path.firstSurface;
    }

    if(!CausticSupportedHit(reflector) || !CausticSupportedHit(receiver) ||
        !CausticInRegisteredDomain(config, reflector) || CausticInRegisteredDomain(config, receiver))
    {
        return CausticCameraOwned;
    }

    bool reflectiveMaterial = reflector.materialModel == 2u ||
        (reflector.materialModel == 0u && CausticSaturate(reflector.metallic) == 1.0f);
    float reflectorRoughness = CausticSaturate(reflector.roughness);
    float receiverRoughness = CausticSaturate(receiver.roughness);

    if(!reflectiveMaterial || reflectorRoughness >= config.reflectorRoughnessThreshold ||
        receiverRoughness < config.receiverRoughnessThreshold)
    {
        return CausticCameraOwned;
    }

    return reflectorRoughness <= CausticDeltaRoughness ? CausticDeltaEligible : CausticGlossyEligible;
}

#undef PT_CAUSTIC_FUNCTION
#undef PT_CAUSTIC_UINT
#endif
