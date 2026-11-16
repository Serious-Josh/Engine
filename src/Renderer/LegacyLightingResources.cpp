#include "LegacyLightingResources.h"

#include "DescriptorHeap.h"
#include "GraphicsDevice.h"
#include "Core/Log.h"

bool LegacyLightingResources::Resize(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width,
    unsigned int height)
{
    // Unsized until every pass succeeds, so a partial failure is never mistaken for a sized set.
    MarkSized(0, 0);
    ID3D12Device* const device = graphicsDevice.GetDevice();
    const unsigned int halfWidth = (width + 1u) / 2u;
    const unsigned int halfHeight = (height + 1u) / 2u;

    if(!rayTracedShadowPass.Resize(device, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced shadow pass.");
        return false;
    }

    if(!rayTracedReflectionPass.Resize(device, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection pass.");
        return false;
    }

    if(!rayTracedGIPass.Resize(device, descriptorHeap, halfWidth, halfHeight))
    {
        Log::Error("Failed to resize ray traced GI pass.");
        return false;
    }

    if(!rayTracedAOPass.Resize(device, descriptorHeap, halfWidth, halfHeight))
    {
        Log::Error("Failed to resize ray traced AO pass.");
        return false;
    }

    if(!aoTemporalPass.Resize(graphicsDevice, descriptorHeap, halfWidth, halfHeight))
    {
        Log::Error("Failed to resize AO temporal pass.");
        return false;
    }

    if(!aoDenoisePass.Resize(graphicsDevice, descriptorHeap, halfWidth, halfHeight))
    {
        Log::Error("Failed to resize AO denoise pass.");
        return false;
    }

    if(!aoResolvePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize AO resolve pass.");
        return false;
    }

    if(!rayTracedReflectionTemporalPass.Resize(device, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection temporal pass.");
        return false;
    }

    if(!reflectionContributionLimitPass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize reflection contribution limiter.");
        return false;
    }

    if(!nrdReflectionPreparePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection preparation pass.");
        return false;
    }

    if(!nrdGIPreparePass.Resize(graphicsDevice, descriptorHeap, halfWidth, halfHeight))
    {
        Log::Error("Failed to resize NRD GI preparation pass.");
        return false;
    }

    if(!nrdReflectionResolvePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection resolve pass.");
        return false;
    }

    if(!nrdDiffuseResolvePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD diffuse resolve pass.");
        return false;
    }

    if(!giSpatialDenoisePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize gi spatial denoise pass.");
        return false;
    }

    if(!rayTracedReflectionDenoisePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection denoise pass.");
        return false;
    }

    if(!shadowDenoisePass.Resize(graphicsDevice, descriptorHeap, width, height))
    {
        Log::Error("Failed to resize shadow denoise pass.");
        return false;
    }

    MarkSized(width, height);
    return true;
}

void LegacyLightingResources::ReleaseTextures()
{
    rayTracedShadowPass.ReleaseTextures();
    rayTracedReflectionPass.ReleaseTextures();
    rayTracedGIPass.ReleaseTextures();
    rayTracedAOPass.ReleaseTextures();
    aoTemporalPass.ReleaseTextures();
    aoDenoisePass.ReleaseTextures();
    aoResolvePass.ReleaseTextures();
    rayTracedReflectionTemporalPass.ReleaseTextures();
    reflectionContributionLimitPass.ReleaseTextures();
    nrdReflectionPreparePass.ReleaseTextures();
    nrdGIPreparePass.ReleaseTextures();
    nrdReflectionResolvePass.ReleaseTextures();
    nrdDiffuseResolvePass.ReleaseTextures();
    giSpatialDenoisePass.ReleaseTextures();
    rayTracedReflectionDenoisePass.ReleaseTextures();
    shadowDenoisePass.ReleaseTextures();
    MarkSized(0, 0);
}
