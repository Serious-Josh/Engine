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
// through the current TLAS, which equals the previous one because history survives only identical geometry.

struct RAB_Material
{
    uint model;
    float roughness;
    float metallic;
};

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material material;
    material.model = surface.surface.material.model;
    material.roughness = surface.surface.material.roughness;
    material.metallic = surface.surface.material.metallic;
    return material;
}

// Approved Gate 4 similarity: the same material model, roughness and metallic each within 0.5. Under ray-traced
// bias correction this only selects neighbors; it never biases the estimate.
bool RAB_AreMaterialsSimilar(RAB_Material first, RAB_Material second)
{
    return first.model == second.model && abs(first.roughness - second.roughness) <= 0.5f &&
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

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
    return clamp(pixelPosition, int2(0, 0), RtxdiBridgeViewSize() - 1);
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return RtxdiBridgeTranslateLightIndex(int(lightIndex), currentToPrevious);
}

// Visibility of a light sample from a previous surface. The sample was formed at previousSurface, so the ray
// starts there; currentSurface is the SDK's fallback for the temporal neighbor and is not needed.
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
    RAB_LightSample lightSample)
{
    return previousSurface.valid && lightSample.sample.valid && RtxdiBridgeVisible(previousSurface, lightSample);
}

#include "Rtxdi/DI/SpatioTemporalResampling.hlsli"

#endif
