// SHARC.4D: the D6 radiance cache's resolve (RadianceCacheD6::RecordResolve), one thread per bucket, after the path
// tracer's cache library inserted the frame's samples. Nothing is resolved unless the GPU header names the binding's
// epoch and identity: a stale or uncleared generation is left untouched (its next clear zeroes it).

#include "RadianceCacheD6.hlsli"

cbuffer RadianceCacheD6ResolveConstants : register(b0)
{
    RadianceCacheD6Binding cache;
};

[numthreads(64, 1, 1)]
void CSResolve(uint3 dispatchThread : SV_DispatchThreadID)
{
    uint bucket = dispatchThread.x;
    bool active = bucket < cache.bucketCount && RadianceCacheD6BindingAccepted(cache);
    RadianceCacheD6ResolveCounts counts = (RadianceCacheD6ResolveCounts)0;
    bool overflowed = false;

    if(active)
    {
        RWByteAddressBuffer bucketFlags = ResourceDescriptorHeap[cache.bucketFlagsIndex];
        overflowed = (bucketFlags.Load(bucket * 4u) & 1u) != 0u;
        counts = RadianceCacheD6ResolveBucket(cache, bucket);
    }

    // Every lane takes part in the wave statistics.
    RadianceCacheD6CountResolve(cache, counts, overflowed);
}
