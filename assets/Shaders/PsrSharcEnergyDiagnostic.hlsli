// Test-only observer of actual uncached integrator contributions. No harvest state is read.
struct PsrSharcEnergyObservation
{
    bool seen;
    bool valid;
    bool sampled;
    bool eligible;
    float3 position;
    float3 normal;
    float metallic;
    float roughness;
    float3 mirrorThroughput;
    float3 bsdfWeight;
    float3 continuationThroughput;
    float3 suffixImage;
    float3 direct;
    float3 immediateImage;
    float3 direction;
    float pdf;
};
void PsrSharcEnergyStore(PsrSharcEnergyObservation observation, PathTraceRasterFirstVertex first, float3 radiance)
{
    uint record = DispatchRaysIndex().x * samplesPerFrame + g_pathTraceRadianceCachePaths - 1u;
#ifdef PATH_TRACE_PSR_SHARC_ENERGY_REFERENCE
    uint outputIndex = psrGBufferPadding.z;
#else
    uint outputIndex = psrGBufferPadding.x;
#endif
    RWByteAddressBuffer output = ResourceDescriptorHeap[outputIndex];
    uint address = record * 192u;
    uint2 key = 0u;
    uint level = 0u;
    bool eligible = observation.seen && observation.eligible &&
        RadianceCacheD6MakeClassKey(radianceCache, observation.position, observation.normal, false, key, level);
    float3 divisor = observation.continuationThroughput;
    float3 target = select(abs(divisor) > 1.0e-20f, observation.suffixImage / divisor, 0.0f);
    float3 immediate = select(abs(divisor) > 1.0e-20f, observation.immediateImage / divisor, 0.0f);
    uint flags = (observation.seen ? 1u : 0u) | (observation.valid ? 2u : 0u) |
        (observation.sampled ? 4u : 0u) | (eligible ? 8u : 0u) | (PathTracePsrReplaced(first.psrState) ? 16u : 0u);
    output.Store4(address, uint4(flags, asuint(observation.position)));
    output.Store4(address + 16u, uint4(asuint(observation.normal), asuint(observation.metallic)));
    output.Store4(address + 32u, uint4(asuint(observation.roughness), asuint(observation.mirrorThroughput)));
    output.Store3(address + 48u, asuint(observation.bsdfWeight));
    output.Store3(address + 60u, asuint(target));
    output.Store3(address + 72u, asuint(observation.direct));
    output.Store3(address + 84u, asuint(immediate));
    output.Store3(address + 96u, asuint(radiance));
    output.Store3(address + 108u, asuint(observation.continuationThroughput));
    output.Store4(address + 120u, uint4(asuint(observation.direction), asuint(observation.pdf)));
    output.Store4(address + 136u, uint4(observation.seen ? 1u : 0xFFFFFFFFu,
        first.psrPixel.y * resolution.x + first.psrPixel.x, key));
    uint2 virtualKey = 0u;
    RadianceCacheD6MakeBindingKey(radianceCache, PathTracePsrVirtualPosition(first.psrPixel),
        PsrReflectDirection(observation.normal, first.surface.normal), virtualKey);
    Texture2D<float4> stored = ResourceDescriptorHeap[gBufferSurfaceIndex];
    float material = stored.Load(int3(first.psrPixel, 0)).w;
    output.Store4(address + 152u, uint4(level, asuint(material), virtualKey));
    output.Store4(address + 168u, uint4(0u, asuint(first.surface.baseColor)));
    output.Store2(address + 184u, uint2(0u, 0u));
}
