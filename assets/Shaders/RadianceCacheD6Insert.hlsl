// SHARC.4D-Q: deferred D6 insertion (RadianceCacheD6::RecordDeferredInsertion), one thread per training record, after
// the learning dispatch that stored them and before resolve. It runs what the inline harvest runs at the end of each
// path (PathTraceRadianceCacheHarvestEnd), unchanged: the generation header check, the D6 sample values and the atomic
// insertion with its wave statistics. Only records stamped with the binding's sequence count (RadianceCacheD6.hlsli).

#include "RadianceCacheD6.hlsli"

cbuffer RadianceCacheD6InsertConstants : register(b0)
{
    RadianceCacheD6Binding cache;
};

// Groups are dispatched in rows of RADIANCE_CACHE_D6_INSERT_GROUPS_PER_ROW (RadianceCacheD6::InsertGroupsPerRow), so a
// buffer of more than 65535 groups of records is reachable.
static const uint RADIANCE_CACHE_D6_INSERT_GROUPS_PER_ROW = 1024u;

uint RadianceCacheD6InsertRecord(uint3 group, uint thread)
{
    return (group.y * RADIANCE_CACHE_D6_INSERT_GROUPS_PER_ROW + group.x) * 64u + thread;
}

// A new record buffer (or one whose stamps may be stale beyond the sequence's range): no record is current.
[numthreads(64, 1, 1)]
void CSClearRecordStamps(uint3 group : SV_GroupID, uint3 groupThread : SV_GroupThreadID)
{
    uint record = RadianceCacheD6InsertRecord(group, groupThread.x);

    if(record < cache.recordCapacity)
    {
        RWByteAddressBuffer records = ResourceDescriptorHeap[cache.recordsIndex];
        records.Store(RadianceCacheD6StampAddress(cache, record), 0u);
    }
}

[numthreads(64, 1, 1)]
void CSInsertRecords(uint3 group : SV_GroupID, uint3 groupThread : SV_GroupThreadID)
{
    uint record = RadianceCacheD6InsertRecord(group, groupThread.x);
    RWByteAddressBuffer records = ResourceDescriptorHeap[cache.recordsIndex];
    bool harvested = false;
    bool sampledContinuation = false;

    if(record < cache.recordCapacity)
    {
        uint stamp = records.Load(RadianceCacheD6StampAddress(cache, record));
        harvested = (stamp >> 1u) == cache.recordSequence;
        sampledContinuation = harvested && (stamp & 1u) != 0u;
    }

    uint2 key = uint2(0u, 0u);
    float3 normal = float3(0.0f, 0.0f, 0.0f);
    float3 direction = float3(0.0f, 0.0f, 0.0f);
    float pdf = 0.0f;
    float3 suffixRadiance = float3(0.0f, 0.0f, 0.0f);

    if(harvested)
    {
        uint base = RadianceCacheD6RecordAddress(record);
        uint4 first = records.Load4(base);
        uint4 second = records.Load4(base + 16u);
        uint4 third = records.Load4(base + 32u);
        key = first.xy;
        normal = asfloat(uint3(first.zw, second.x));
        direction = asfloat(second.yzw);
        pdf = asfloat(third.x);
        suffixRadiance = asfloat(third.yzw);
    }

    bool accepted = RadianceCacheD6BindingAccepted(cache);
    bool active = harvested && accepted;
    bool sampled = active && sampledContinuation;
    float3 values[RADIANCE_CACHE_D6_MOMENTS];
    RadianceCacheD6SampleValues(cache, sampled, normal, direction, pdf, suffixRadiance, values);

    bool zero = !sampled || all(suffixRadiance == 0.0f);
    RadianceCacheD6CountWave(cache, RADIANCE_CACHE_D6_STATISTIC_HEADER_REJECTED, harvested && !accepted);
    RadianceCacheD6AddSample(cache, active, key, values, zero);
}
