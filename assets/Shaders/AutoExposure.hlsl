// Photographic and automatic exposure (Physical Lighting & Exposure, Checkpoint 2; CameraExposure.h). Reads the
// render-extent HDR scene colour only: lighting, accumulation, SHARC and denoiser inputs never see exposure.
//
// CSClear:     zeroes the histogram and the state (once, after creation).
// CSHistogram: 256-bin histogram of per-pixel physical EV100, log2(L_phys * 100 / K), over [meteringMin, meteringMax].
//              L_phys = Rec.709 luminance of the GPU value / S, S = (0.18 * 100 / K) * 2^-EV_ref, so pre-scaled values
//              are never taken as nits. Pixels with a non-finite channel or negative luminance are counted as
//              invalid and excluded; black (zero) and darker-than-range pixels fall in bin 0, brighter in bin 255.
// CSAdapt:     averages the bin-centre EV of the valid pixels between the low and high percentiles (the metered
//              EV100), subtracts compensation for the target, adapts exponentially (1 - exp(-rate * dt)) at the
//              brightening rate when the target is lower (brighter image) and the darkening rate otherwise, and
//              stores the effective EV100 and the multiplier 2^(EV_ref - EV_eff). Manual mode stores the CPU's
//              effective EV100. An empty histogram holds the previous exposure (EV_ref before any frame). Reset
//              (first frame, camera cut, scene load) snaps to the target; lock holds the adapted value. Then it
//              clears the histogram for the next frame.
//
// State (raw, 8 words): 0 effective EV100, 1 multiplier, 2 target EV100, 3 metered EV100, 4 valid pixels,
// 5 invalid pixels, 6 initialized, 7 unused. Histogram (raw): words 0..255 bins, 256 invalid pixels.

struct AutoExposureConstants
{
    uint sceneColorIndex;
    uint histogramIndex;
    uint stateIndex;
    uint width;

    uint height;
    uint mode;     // 0 Manual, 1 Auto (ExposureMode).
    uint flags;    // 1 reset, 2 locked.
    float referenceEV100;

    float manualEffectiveEV100;
    float compensation;
    float meteringMinEV100;
    float meteringMaxEV100;

    float lowPercentile;
    float highPercentile;
    float brighteningSpeed;
    float darkeningSpeed;

    float deltaSeconds;
    float3 padding;
};

ConstantBuffer<AutoExposureConstants> constants : register(b0);

static const uint BIN_COUNT = 256u;
static const uint INVALID_WORD = 256u;
static const uint FLAG_RESET = 1u;
static const uint FLAG_LOCKED = 2u;
// log2(100 / K) with K = 12.5, and log2(0.18 * 100 / K) = log2(1.44): the metering and pre-scale constants.
static const float LOG2_METER = 3.0f;
static const float LOG2_PRESCALE = 0.526068811667588f;

groupshared uint sharedBins[BIN_COUNT];

[numthreads(1, 1, 1)]
void CSClear()
{
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[constants.histogramIndex];
    RWByteAddressBuffer state = ResourceDescriptorHeap[constants.stateIndex];

    for(uint word = 0; word <= INVALID_WORD; ++word)
    {
        histogram.Store(word * 4u, 0u);
    }

    for(uint stateWord = 0; stateWord < 8u; ++stateWord)
    {
        state.Store(stateWord * 4u, 0u);
    }
}

[numthreads(16, 16, 1)]
void CSHistogram(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    sharedBins[groupIndex] = 0u;
    GroupMemoryBarrierWithGroupSync();
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[constants.histogramIndex];

    if(id.x < constants.width && id.y < constants.height)
    {
        Texture2D<float4> sceneColor = ResourceDescriptorHeap[constants.sceneColorIndex];
        float3 color = sceneColor.Load(int3(id.xy, 0)).rgb;
        float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

        if(!all(isfinite(color)) || !isfinite(luminance) || luminance < 0.0f)
        {
            histogram.InterlockedAdd(INVALID_WORD * 4u, 1u);
        }
        else
        {
            uint bin = 0u;

            if(luminance > 0.0f)
            {
                float ev = log2(luminance) + constants.referenceEV100 - LOG2_PRESCALE + LOG2_METER;
                float position = (ev - constants.meteringMinEV100) / (constants.meteringMaxEV100 - constants.meteringMinEV100);
                bin = uint(clamp(position * float(BIN_COUNT), 0.0f, float(BIN_COUNT - 1u)));
            }

            InterlockedAdd(sharedBins[bin], 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if(sharedBins[groupIndex] != 0u)
    {
        histogram.InterlockedAdd(groupIndex * 4u, sharedBins[groupIndex]);
    }
}

[numthreads(256, 1, 1)]
void CSAdapt(uint groupIndex : SV_GroupIndex)
{
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[constants.histogramIndex];
    RWByteAddressBuffer state = ResourceDescriptorHeap[constants.stateIndex];
    sharedBins[groupIndex] = histogram.Load(groupIndex * 4u);
    GroupMemoryBarrierWithGroupSync();

    if(groupIndex == 0u)
    {
        uint total = 0u;

        for(uint bin = 0; bin < BIN_COUNT; ++bin)
        {
            total += sharedBins[bin];
        }

        const bool initialized = state.Load(6u * 4u) != 0u;
        const float previous = initialized ? asfloat(state.Load(0u)) : constants.referenceEV100;
        float metered = asfloat(state.Load(3u * 4u));
        const float range = constants.meteringMaxEV100 - constants.meteringMinEV100;

        if(total > 0u)
        {
            // Percentile band average in pixel counts, splitting the bins at the band edges.
            const float low = constants.lowPercentile * float(total);
            const float high = constants.highPercentile * float(total);
            float cumulative = 0.0f;
            float weighted = 0.0f;
            float weight = 0.0f;

            for(uint index = 0; index < BIN_COUNT; ++index)
            {
                const float count = float(sharedBins[index]);
                const float overlap = max(min(cumulative + count, high) - max(cumulative, low), 0.0f);
                weighted += overlap * (constants.meteringMinEV100 + (float(index) + 0.5f) * range / float(BIN_COUNT));
                weight += overlap;
                cumulative += count;
            }

            if(weight > 0.0f)
            {
                metered = weighted / weight;
            }
        }
        else if(!initialized)
        {
            metered = constants.referenceEV100;
        }

        float target = metered - constants.compensation;
        float effective = previous;

        if(constants.mode == 0u)
        {
            target = constants.manualEffectiveEV100;
            effective = target;
        }
        else if((constants.flags & FLAG_LOCKED) != 0u && initialized)
        {
            effective = previous;
        }
        else if(total == 0u)
        {
            effective = previous;
        }
        else if(!initialized || (constants.flags & FLAG_RESET) != 0u)
        {
            effective = target;
        }
        else
        {
            const float rate = target < previous ? constants.brighteningSpeed : constants.darkeningSpeed;
            effective = previous + (target - previous) * (1.0f - exp(-rate * max(constants.deltaSeconds, 0.0f)));
        }

        state.Store(0u, asuint(effective));
        state.Store(1u * 4u, asuint(exp2(constants.referenceEV100 - effective)));
        state.Store(2u * 4u, asuint(target));
        state.Store(3u * 4u, asuint(metered));
        state.Store(4u * 4u, total);
        state.Store(5u * 4u, histogram.Load(INVALID_WORD * 4u));
        state.Store(6u * 4u, 1u);
    }

    // Every bin was read above (the barrier), so the clear for the next frame cannot race the read.
    GroupMemoryBarrierWithGroupSync();
    histogram.Store(groupIndex * 4u, 0u);

    if(groupIndex == 0u)
    {
        histogram.Store(INVALID_WORD * 4u, 0u);
    }
}
