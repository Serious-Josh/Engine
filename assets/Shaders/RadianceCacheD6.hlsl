// SHARC.4C: the D6 radiance cache's generation clear (RadianceCacheD6::RecordGeneration). CSClearWords zeroes one raw
// buffer; CSWriteHeader then writes the generation header, recorded after a UAV barrier on every cleared buffer.

#include "RadianceCacheD6.hlsli"

// Must match RadianceCacheD6::ClearConstants.
cbuffer RadianceCacheD6ClearConstants : register(b0)
{
    uint targetIndex;
    uint wordCount;
    uint2 epoch;

    uint configurationIdentity;
    uint state;
    uint2 padding;
};

// Each thread zeroes RADIANCE_CACHE_D6_CLEAR_WORDS_PER_THREAD consecutive words (four 16-byte stores).
static const uint RADIANCE_CACHE_D6_CLEAR_WORDS_PER_THREAD = 16u;

[numthreads(64, 1, 1)]
void CSClearWords(uint3 dispatchThread : SV_DispatchThreadID)
{
    RWByteAddressBuffer target = ResourceDescriptorHeap[targetIndex];
    uint first = dispatchThread.x * RADIANCE_CACHE_D6_CLEAR_WORDS_PER_THREAD;

    for(uint word = first; word < min(first + RADIANCE_CACHE_D6_CLEAR_WORDS_PER_THREAD, wordCount); word += 4u)
    {
        if(word + 4u <= wordCount)
        {
            target.Store4(word * 4u, uint4(0u, 0u, 0u, 0u));
        }
        else
        {
            for(uint tail = word; tail < wordCount; ++tail)
            {
                target.Store(tail * 4u, 0u);
            }
        }
    }
}

[numthreads(1, 1, 1)]
void CSWriteHeader()
{
    RWByteAddressBuffer header = ResourceDescriptorHeap[targetIndex];
    header.Store4(0u, uint4(epoch, configurationIdentity, state));
}
