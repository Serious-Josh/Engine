#ifndef ENGINE_RTXDI_REUSE_BRIDGE
#define ENGINE_RTXDI_REUSE_BRIDGE

// RTXPT.3 R3.5/R3.6: the RTXDI application-bridge callbacks for fused spatiotemporal reuse
// (RTXDI_DISpatioTemporalResampling). Included after RtxdiApplicationBridge.hlsli, by the raster-primary ReSTIR
// DI library only. The temporal neighbor and the spatial neighbors both come from the previous frame's
// reservoirs and surfaces; there is no same-frame spatial pass.
//
// The includer provides, before including this file:
//   RAB_Surface RtxdiBridgeLoadSurface(int2 pixel, bool previousFrame);  (invalid outside the view or background)
//   bool RtxdiBridgeVisible(RAB_Surface surface, RAB_LightSample lightSample);
//   int RtxdiBridgeTranslateLightIndex(int lightIndex, bool currentToPrevious);  (-1: no reusable counterpart)
//   int2 RtxdiBridgeViewSize();
//   #define RTXDI_NEIGHBOR_OFFSETS_BUFFER   (a Buffer<float2>)
//
// Ray-traced bias correction (the approved configuration) weights each previous neighbor by its target times
// its visibility, which assumes stored reservoirs no longer hold occluded samples: the includer stores with
// RTXDI_StoreVisibilityInDIReservoir(..., discardIfInvisible = true). Previous-surface visibility is traced
// through the current TLAS. While no transform changed since the reused frame, that TLAS is the previous one and the
// test is exact. PT.2A keeps history through transform-only updates (any other geometry change still drops it); the
// previous TLAS is not retained, so after moved transforms the test sees the current occluders (see below).

struct RAB_Material
{
    // The material's optional-lobe set (GPUMaterial::lobeFlags): coated, fuzzy or filmed surfaces pair only with alike.
    uint lobeFlags;
    float roughness;
    float metallic;
};

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material material;
    material.lobeFlags = surface.surface.material.lobeFlags;
    material.roughness = surface.surface.material.roughness;
    material.metallic = surface.surface.material.metallic;
    return material;
}

// Approved Gate 4 similarity: the same optional lobes, roughness and metallic each within 0.5. Under ray-traced
// bias correction this only selects neighbors; it never biases the estimate.
bool RAB_AreMaterialsSimilar(RAB_Material first, RAB_Material second)
{
    return first.lobeFlags == second.lobeFlags && abs(first.roughness - second.roughness) <= 0.5f &&
        abs(first.metallic - second.metallic) <= 0.5f;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
    return RtxdiBridgeLoadSurface(pixelPosition, previousFrame);
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.valid;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
    return surface.linearDepth;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
    return surface.surface.normal;
}

// RTXDI.2A: an out-of-view neighbor position is returned unchanged, so RAB_GetGBufferSurface reports it invalid and
// the SDK skips that neighbor before reading its reservoir: the first resampling pass checks the surface first, and the
// bias-correction pass revisits only neighbors the first pass accepted (both map positions through this function, so
// it must be deterministic). Clamping into the view instead sent every out-of-view disocclusion-boost neighbor of a
// pixel entering the view to the same border pixels, whose shared reservoirs then streaked inward with camera motion
// (build/rtxdi-2a/REPORT.md).
int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
    return pixelPosition;
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return RtxdiBridgeTranslateLightIndex(int(lightIndex), currentToPrevious);
}

// Visibility of a light sample from a previous surface, for the bias-correction weight of that neighbor. The sample was
// formed at previousSurface, so the ray starts there; currentSurface is the SDK's fallback and is not needed.
// PT.2A, after moved transforms: the ray still starts at the previous surface but meets the current occluders, a
// transient bias in the MIS normalization near moving objects and their shadows. RTXDI's documented alternative, starting
// from currentSurface (the temporal neighbor's fallback) toward the same light point, measured clearly more biased (up to
// 6-7% of 16 x 16 tiles off by more than 5%, against 2% here; PT2A_ReSTIR_DI_History_Visibility.md). Shading is unaffected:
// the selected sample is always shadow-tested from the current surface through the current TLAS.
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
    RAB_LightSample lightSample)
{
    return previousSurface.valid && lightSample.sample.valid && RtxdiBridgeVisible(previousSurface, lightSample);
}

#include "Rtxdi/DI/SpatioTemporalResampling.hlsli"

#endif
