#pragma once

#include "Passes/AODenoisePass.h"
#include "Passes/AOResolvePass.h"
#include "Passes/AOTemporalPass.h"
#include "Passes/GISpatialDenoisePass.h"
#include "Passes/NrdDiffuseResolvePass.h"
#include "Passes/NrdPreparePass.h"
#include "Passes/NrdReflectionResolvePass.h"
#include "Passes/RayTracedAOPass.h"
#include "Passes/RayTracedGIPass.h"
#include "Passes/RayTracedReflectionDenoisePass.h"
#include "Passes/RayTracedReflectionPass.h"
#include "Passes/RayTracedReflectionTemporalPass.h"
#include "Passes/RayTracedShadowPass.h"
#include "Passes/ReflectionContributionLimitPass.h"
#include "Passes/ShadowDenoisePass.h"

#include "ModeResourceResidency.h"

class DescriptorHeap;
class GraphicsDevice;

// M0c.1: the passes only the Legacy lighting mode records, owned together. No path-traced mode (Path Tracing or
// Reference) and no shared stage reads them; their only other uses are Initialize, ResetSceneHistory and Shutdown.
// Includes Legacy's NRD preparation and resolve textures (attributed to Denoisers / Upscaling by their own passes).
//
// Not included, deliberately:
// - GISignedCorrectionPass: active with RTGI (GI_MODE_PRODUCTION is the baseline-correction mode), but lazily created
//   before the lighting mode is resolved, possibly on a path-traced frame.
// - The Legacy REBLUR instances (m_nrdReflectionDenoiser, m_nrdGIDenoiser): owned by NRD retention, and their
//   recreation can wait for the GPU, which must not move into a recording.
//
// The passes follow the render extent only while Legacy renders. They are created at startup in Renderer's existing
// order. M0c.2: while Legacy is inactive their textures may retire (Renderer::UpdateLegacyLightingRetention); pipelines
// stay, and the next Legacy frame recreates the textures through Resize. Shutdown is unchanged.
struct LegacyLightingResources
{
    // M0c.2 residency of the passes' textures and descriptors (the policy Path Tracing also uses; Renderer owns it).
    using Retention = ModeResourceResidency;

    Retention retention;

    RayTracedShadowPass rayTracedShadowPass;
    RayTracedReflectionPass rayTracedReflectionPass;
    RayTracedReflectionTemporalPass rayTracedReflectionTemporalPass;
    RayTracedReflectionDenoisePass rayTracedReflectionDenoisePass;
    RayTracedGIPass rayTracedGIPass;
    RayTracedAOPass rayTracedAOPass;
    AOTemporalPass aoTemporalPass;
    AODenoisePass aoDenoisePass;
    AOResolvePass aoResolvePass;
    ReflectionContributionLimitPass reflectionContributionLimitPass;
    GISpatialDenoisePass giSpatialDenoisePass;
    ShadowDenoisePass shadowDenoisePass;
    NrdPreparePass nrdReflectionPreparePass;
    NrdPreparePass nrdGIPreparePass;
    NrdReflectionResolvePass nrdReflectionResolvePass;
    NrdDiffuseResolvePass nrdDiffuseResolvePass;

    // Records the render extent every pass was initialized or last resized for.
    void MarkSized(unsigned int width, unsigned int height) noexcept
    {
        m_width = width;
        m_height = height;
    }

    // False after a scene resize skipped the passes (stale) or after a failed resize.
    bool IsSizedFor(unsigned int width, unsigned int height) const noexcept
    {
        return m_width == width && m_height == height;
    }

    // Resizes every pass to the render extent (GI, AO and the GI preparation at half extent), in the order the scene
    // resize used. The caller guarantees that no submitted or recorded GPU work still references the passes' resources
    // or descriptors. A failure leaves the set unsized.
    bool Resize(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);

    // M0c.2: releases every pass's render-extent textures and their descriptors (pipelines stay) and leaves the set
    // unsized. Descriptor indices return to the heap immediately, so the caller guarantees that the GPU has completed every
    // submission that used them and that nothing recorded since references them.
    void ReleaseTextures();

private:
    unsigned int m_width = 0;
    unsigned int m_height = 0;
};
