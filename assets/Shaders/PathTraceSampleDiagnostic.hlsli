// R.9I.1: per-raygen-lane passive bookkeeping. Other entry points (including sparse
// SHARC training and split DI) leave the gate false and never touch these buffers.
// R.9I.1D: automatic capture opens the gate for every pixel; the selected-pixel store keeps its own gate.
#include "PathTraceSampleRecord.h"
static bool g_sampleDiagnostic = false;
static bool g_sampleManual = false;
static bool g_sampleAutoWrote = false;
static uint2 g_samplePixel;
static PathTraceSampleRecord g_sampleRecord;
static uint g_sampleVertex = 0u;
static uint g_sampleMaterial = 0xFFFFFFFFu;
static uint g_sampleLight = 0xFFFFFFFFu;
static uint g_sampleLightType = 0xFFFFFFFFu;
static float3 g_sampleThroughput;
static float g_samplePreviousPdf;

float4 PathTraceDiagnosticRGB(float3 rgb)
{
    return float4(rgb, PathTraceLuminance(rgb));
}

void PathTraceDiagnosticDirect(float3 term)
{
    if(!g_sampleDiagnostic) return;
    if(g_sampleVertex == 0u) g_sampleRecord.primary.rgb += term;
    else
    {
        g_sampleRecord.indirect.rgb += term;
        if(g_sampleVertex == 1u) g_sampleRecord.secondary.rgb += term;
        else g_sampleRecord.deeper.rgb += term;
    }
}

void PathTraceDiagnosticMiss(uint vertex, float3 term)
{
    if(!g_sampleDiagnostic) return;
    // A miss immediately after x0 is its direct environment/sun estimate,
    // not a surface-to-surface indirect bounce (vertex 0 is camera background).
    if(vertex <= 1u) g_sampleRecord.primary.rgb += term;
    else
    {
        g_sampleRecord.indirect.rgb += term;
        g_sampleRecord.miss.rgb += term;
    }
}

void PathTraceDiagnosticCache(float3 term)
{
    if(!g_sampleDiagnostic) return;
    g_sampleRecord.config.w |= 1u;
    g_sampleRecord.cache.rgb += term;
    g_sampleRecord.indirect.rgb += term;
}

void PathTraceDiagnosticLight(PathTraceSurface surface, PathTraceLightSample lightSample,
    float3 value, float3 diffuseValue, float cosine, float weight)
{
    if(!g_sampleDiagnostic || g_sampleVertex == 0u) return;
    float3 contribution = g_sampleThroughput * (value * weight);
    float luminance = PathTraceLuminance(contribution);
    if(luminance > g_sampleRecord.dominant.w)
    {
        g_sampleRecord.dominant = float4(contribution, luminance);
        g_sampleRecord.diffuse = PathTraceDiagnosticRGB(g_sampleThroughput *
            (diffuseValue * cosine * lightSample.radiance * weight));
        g_sampleRecord.specular = PathTraceDiagnosticRGB(contribution - g_sampleRecord.diffuse.rgb);
        g_sampleRecord.factors = float4(value, cosine);
        g_sampleRecord.incidence = float4(lightSample.radiance, lightSample.distance);
        g_sampleRecord.throughput = float4(g_sampleThroughput, g_samplePreviousPdf);
        g_sampleRecord.hit.yzw = float3(surface.material.roughness, lightSample.pdf, weight);
        g_sampleRecord.info = uint4(g_sampleVertex, g_sampleLight, g_sampleLightType, g_sampleMaterial);
    }
}

void PathTraceDiagnosticWrite(RWByteAddressBuffer records, uint base, PathTraceSampleRecord record)
{
    records.Store4(base, record.meta);
    records.Store4(base + 16u, record.config);
#define PT_DIAG_STORE(offset, field) records.Store4(base + offset, asuint(record.field))
    PT_DIAG_STORE(32u, sample);
    PT_DIAG_STORE(48u, indirect);
    PT_DIAG_STORE(64u, mean);
    PT_DIAG_STORE(80u, primary);
    PT_DIAG_STORE(96u, secondary);
    PT_DIAG_STORE(112u, deeper);
    PT_DIAG_STORE(128u, miss);
    PT_DIAG_STORE(144u, cache);
    PT_DIAG_STORE(160u, dominant);
    PT_DIAG_STORE(176u, diffuse);
    PT_DIAG_STORE(192u, specular);
    PT_DIAG_STORE(208u, factors);
    PT_DIAG_STORE(224u, incidence);
    PT_DIAG_STORE(240u, throughput);
    PT_DIAG_STORE(256u, hit);
#undef PT_DIAG_STORE
    records.Store4(base + 272u, record.info);
}

// R.9I.1D bucket lock: one attempt, never a spin. The holder's stores are made device-visible before release,
// so a later holder's record cannot be interleaved with an earlier one.
bool PathTraceDiagnosticAutoLock(RWByteAddressBuffer buckets, uint base)
{
    uint previous;
    buckets.InterlockedCompareExchange(base + 4u, 0u, 1u, previous);
    return previous == 0u;
}

void PathTraceDiagnosticAutoUnlock(RWByteAddressBuffer buckets, uint base)
{
    DeviceMemoryBarrier();
    uint previous;
    buckets.InterlockedExchange(base + 4u, 0u, previous);
}

uint PathTraceDiagnosticAutoBase(uint2 pixel)
{
    // Hashed (integer mix) so a cluster of adjacent outliers spreads over many buckets instead of sharing one.
    uint hash = (pixel.x | (pixel.y << 16u)) * 0x9E3779B1u;
    hash ^= hash >> 15u;
    hash *= 0x85EBCA77u;
    hash ^= hash >> 13u;
    return (hash % PathTraceAutoBucketCount) * PathTraceAutoBucketStride;
}


// Detection only: the luminance comparison selects which complete record to keep, never a radiance value.
void PathTraceDiagnosticAutoStore()
{
    float luminance = g_sampleRecord.indirect.w;

    // NaN and nonpositive luminance never qualify; key 0 marks an empty bucket.
    if(sampleAutoIndex == 0xFFFFFFFFu || !(luminance >= sampleAutoThreshold) || !(luminance > 0.0f))
    {
        return;
    }

    RWByteAddressBuffer buckets = ResourceDescriptorHeap[sampleAutoIndex];
    uint base = PathTraceDiagnosticAutoBase(g_samplePixel);
    uint key = asuint(luminance);
    uint previous;
    buckets.InterlockedAdd(base + 12u, 1u, previous);
    buckets.InterlockedMax(base, key, previous);

    // Weaker or tied: the earlier maximum keeps the bucket.
    if(previous >= key)
    {
        return;
    }

    if(!PathTraceDiagnosticAutoLock(buckets, base))
    {
        buckets.InterlockedAdd(base + 16u, 1u, previous);
        return;
    }

    // A stronger event that took the lock first keeps its record.
    uint current;
    buckets.InterlockedOr(base, 0u, current);

    if(current == key)
    {
        PathTraceSampleRecord record = g_sampleRecord;
        record.config.w |= 4u;
        // Unavailable until the batch mean is stored.
        record.mean = float4(0.0f, 0.0f, 0.0f, -1.0f);
        PathTraceDiagnosticWrite(buckets, base + PathTraceAutoHeaderBytes, record);
        buckets.InterlockedExchange(base + 8u, g_samplePixel.x | (g_samplePixel.y << 16u), previous);
        g_sampleAutoWrote = true;
    }

    PathTraceDiagnosticAutoUnlock(buckets, base);
}

void PathTraceDiagnosticStore(uint sampleOffset, float3 sampleRadiance, bool rejected)
{
    if(!g_sampleDiagnostic)
    {
        return;
    }

    g_sampleRecord.meta = uint4(sampleDiagnosticFrame, accumulatedSamples + sampleOffset,
        accumulatedSamples, samplesPerFrame);
    g_sampleRecord.config.xyz = uint3(maxPathDepth, sampleDiagnosticTarget, randomSeed);
    g_sampleRecord.config.w |= rejected ? 2u : 0u;
    g_sampleRecord.sample = PathTraceDiagnosticRGB(sampleRadiance);
    g_sampleRecord.indirect = PathTraceDiagnosticRGB(g_sampleRecord.indirect.rgb);
    g_sampleRecord.primary = PathTraceDiagnosticRGB(g_sampleRecord.primary.rgb);
    g_sampleRecord.secondary = PathTraceDiagnosticRGB(g_sampleRecord.secondary.rgb);
    g_sampleRecord.deeper = PathTraceDiagnosticRGB(g_sampleRecord.deeper.rgb);
    g_sampleRecord.miss = PathTraceDiagnosticRGB(g_sampleRecord.miss.rgb);
    g_sampleRecord.cache = PathTraceDiagnosticRGB(g_sampleRecord.cache.rgb);

    // Automatic capture always keeps full provenance, so it runs before the selected-pixel threshold strips it.
    PathTraceDiagnosticAutoStore();

    if(!g_sampleManual || sampleOffset >= 64u)
    {
        return;
    }

    // Only this threshold gates expanded provenance, never estimator values or decisions.
    if(g_sampleRecord.indirect.w >= sampleDiagnosticThreshold)
    {
        g_sampleRecord.config.w |= 4u;
    }
    else
    {
        g_sampleRecord.dominant = g_sampleRecord.diffuse = g_sampleRecord.specular = 0.0f;
        g_sampleRecord.factors = g_sampleRecord.incidence = g_sampleRecord.throughput = 0.0f;
        g_sampleRecord.hit.yzw = 0.0f;
        g_sampleRecord.info = 0xFFFFFFFFu;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[sampleDiagnosticIndex];
    PathTraceDiagnosticWrite(records, sampleOffset * 288u, g_sampleRecord);
}

void PathTraceDiagnosticMean(float3 mean)
{
    if(g_sampleManual)
    {
        RWByteAddressBuffer records = ResourceDescriptorHeap[sampleDiagnosticIndex];

        for(uint offset = 0u; offset < min(samplesPerFrame, 64u); ++offset)
        {
            records.Store4(offset * 288u + 64u, asuint(PathTraceDiagnosticRGB(mean)));
        }
    }

    if(!g_sampleAutoWrote)
    {
        return;
    }

    RWByteAddressBuffer buckets = ResourceDescriptorHeap[sampleAutoIndex];
    uint base = PathTraceDiagnosticAutoBase(g_samplePixel);
    uint previous;

    if(!PathTraceDiagnosticAutoLock(buckets, base))
    {
        buckets.InterlockedAdd(base + 20u, 1u, previous);
        return;
    }

    // Only this lane writes this pixel, and the buffer is cleared per dispatch, so a matching owner
    // means the bucket still holds one of this batch's samples.
    uint owner;
    buckets.InterlockedOr(base + 8u, 0u, owner);

    if(owner == (g_samplePixel.x | (g_samplePixel.y << 16u)))
    {
        buckets.Store4(base + PathTraceAutoHeaderBytes + 64u, asuint(PathTraceDiagnosticRGB(mean)));
    }

    PathTraceDiagnosticAutoUnlock(buckets, base);
}
