// R.9I.3 dedicated caustics: bounded adaptive RGB64 accumulation of one frame's deposit records
// and its fold into the caustic-owned history (never Reference accumulation).
//
// Unpacked adaptive RGB64 from R.9I.3C (src/Renderer/tests/AdaptiveCaustic/Unpacked.hlsl): per pixel
// and channel, e = floor(log2 max Z), delta = 2^(e+1-b), b = 63 - ceil(log2 N); records are quantized
// ties-to-even and summed exactly in 64-bit integers; the frame mean is sum * delta / N over ALL
// attempts. |quantizedSum - T| / T <= N 2^-b for every finite nonnegative record set. Invalid records,
// illegal scales or integer overflow latch a fault for the entire epoch. Invalidate runs after
// each validation boundary; no old or partially decoded history may be published.
//
// Control words (raw, 16 B): 0 sticky epoch fault, 1 folded frame count,
// 2 most recent fault flags (retained across explicit epoch resets), 3 attempted frames.
// resetHistory may clear the latch only at a CPU-authorized drained epoch boundary.

cbuffer CausticAccumulateConstants : register(b0)
{
    uint attempts;
    uint pixels;
    uint precisionBits;
    uint resetHistory;
    uint recordsIndex;
    uint sumsIndex;
    uint scalesIndex;
    uint historyIndex;
    uint controlIndex;
    uint3 padding;
};

void Fault(uint flag)
{
    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];
    control.InterlockedOr(0u, flag);
}

bool Valid(uint bits)
{
    uint magnitude = bits & 0x7FFFFFFFu;
    return magnitude < 0x7F800000u && ((bits >> 31u) == 0u || magnitude == 0u);
}

int EffectiveExponent(uint bits)
{
    uint exponent = bits >> 23u;
    uint fraction = bits & 0x7FFFFFu;
    return exponent != 0u ? int(exponent) - 127 : (fraction != 0u ? firstbithigh(fraction) - 149 : -149);
}

uint64_t Quantize(uint bits, int exponent)
{
    uint field = bits >> 23u;
    uint significand = bits & 0x7FFFFFu;
    int power = -149;

    if((bits & 0x7FFFFFFFu) == 0u)
    {
        return 0ull;
    }

    if(field != 0u)
    {
        significand |= 0x800000u;
        power = int(field) - 150;
    }

    int shift = power - (exponent + 1 - int(precisionBits));

    if(shift >= 0)
    {
        if(shift > 62 || uint64_t(significand) > (0x7FFFFFFFFFFFFFFFull >> uint(shift)))
        {
            Fault(8u);
            return 0ull;
        }

        return uint64_t(significand) << uint(shift);
    }

    uint right = uint(-shift);

    if(right > 24u)
    {
        return 0ull;
    }

    uint quotient = significand >> right;
    uint remainder = significand & ((1u << right) - 1u);
    uint half = 1u << (right - 1u);
    return uint64_t(quotient + ((remainder > half || (remainder == half && (quotient & 1u) != 0u)) ? 1u : 0u));
}

[numthreads(256, 1, 1)]
void CSClear(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer sums = ResourceDescriptorHeap[sumsIndex];
    RWByteAddressBuffer scales = ResourceDescriptorHeap[scalesIndex];

    if(id.x == 0u)
    {
        RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];
        if(resetHistory != 0u)
        {
            control.Store(0u, 0u);
            control.Store(4u, 0u);
            control.Store(12u, 0u);
        }

        if(attempts == 0u || attempts > (1u << 20u) || precisionBits < 1u ||
            precisionBits > 63u - firstbithigh(attempts * 2u - 1u))
        {
            Fault(8u);
        }
    }

    if(id.x < pixels)
    {
        sums.Store2(id.x * 24u, uint2(0, 0));
        sums.Store2(id.x * 24u + 8u, uint2(0, 0));
        sums.Store2(id.x * 24u + 16u, uint2(0, 0));
        scales.Store(id.x * 4u, 0u);

        if(resetHistory != 0u)
        {
            RWByteAddressBuffer history = ResourceDescriptorHeap[historyIndex];
            history.Store4(id.x * 16u, asuint(float4(0, 0, 0, 1)));
        }
    }
}

[numthreads(256, 1, 1)]
void CSMaximum(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= attempts)
    {
        return;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[recordsIndex];
    RWByteAddressBuffer sums = ResourceDescriptorHeap[sumsIndex];
    const uint4 record = records.Load4(id.x * 16u);

    // Misses are zero-valued attempts, not an escape from finite-value validation.
    if(record.x == 0xFFFFFFFFu)
    {
        if(any((record.yzw & 0x7FFFFFFFu) != 0u))
        {
            Fault(2u);
        }

        return;
    }

    if(record.x >= pixels)
    {
        Fault(1u);
        return;
    }

    for(uint channel = 0; channel < 3u; ++channel)
    {
        const uint bits = record[channel + 1u];

        if(!Valid(bits))
        {
            Fault(2u);
        }
        else
        {
            sums.InterlockedMax(record.x * 24u + channel * 8u, bits & 0x7FFFFFFFu);
        }
    }
}

[numthreads(256, 1, 1)]
void CSInitialize(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= pixels)
    {
        return;
    }

    RWByteAddressBuffer sums = ResourceDescriptorHeap[sumsIndex];
    RWByteAddressBuffer scales = ResourceDescriptorHeap[scalesIndex];
    uint codes = 0;

    for(uint channel = 0; channel < 3u; ++channel)
    {
        const uint offset = id.x * 24u + channel * 8u;
        codes |= uint(EffectiveExponent(sums.Load(offset)) + 149) << (channel * 9u);
        sums.Store2(offset, uint2(0, 0));
    }

    scales.Store(id.x * 4u, codes);
}

[numthreads(256, 1, 1)]
void CSReplay(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= attempts)
    {
        return;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[recordsIndex];
    RWByteAddressBuffer sums = ResourceDescriptorHeap[sumsIndex];
    RWByteAddressBuffer scales = ResourceDescriptorHeap[scalesIndex];
    const uint4 record = records.Load4(id.x * 16u);

    if(record.x == 0xFFFFFFFFu || record.x >= pixels)
    {
        return;
    }

    const uint codes = scales.Load(record.x * 4u);

    for(uint channel = 0; channel < 3u; ++channel)
    {
        const uint bits = record[channel + 1u];

        if(!Valid(bits))
        {
            Fault(2u);
            continue;
        }

        const uint offset = record.x * 24u + channel * 8u;
        const uint code = (codes >> (channel * 9u)) & 511u;

        if(code > 276u)
        {
            Fault(8u);
            continue;
        }

        const uint64_t value = Quantize(bits & 0x7FFFFFFFu, int(code) - 149);
        uint original;
        const uint low = uint(value);
        sums.InterlockedAdd(offset, low, original);
        const uint carry = original + low < original ? 1u : 0u;
        const uint increment = uint(value >> 32u) + carry;
        sums.InterlockedAdd(offset + 4u, increment, original);

        if(original + increment < original)
        {
            Fault(4u);
        }
    }
}

// Frame mean C = sum delta / N, folded into the caustic history H_F = (F H_{F-1} + C) / (F + 1) in FP64.
[numthreads(256, 1, 1)]
void CSDecode(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= pixels)
    {
        return;
    }

    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];

    if(control.Load(0u) != 0u)
    {
        return; // A faulted measurement is never folded.
    }

    RWByteAddressBuffer sums = ResourceDescriptorHeap[sumsIndex];
    RWByteAddressBuffer scales = ResourceDescriptorHeap[scalesIndex];
    RWByteAddressBuffer history = ResourceDescriptorHeap[historyIndex];
    const uint codes = scales.Load(id.x * 4u);
    const uint folded = control.Load(4u);
    const float4 previous = asfloat(history.Load4(id.x * 16u));
    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f);

    for(uint channel = 0; channel < 3u; ++channel)
    {
        const uint code = (codes >> (channel * 9u)) & 511u;

        if(code > 276u || precisionBits < 1u || precisionBits > 63u || folded == 0xFFFFFFFFu)
        {
            Fault(8u);
            return;
        }

        if(folded != 0u && (!Valid(asuint(previous[channel])) || previous.a != 1.0f))
        {
            Fault(16u);
            return;
        }

        const int exponent = int(code) - 149;
        const uint2 words = sums.Load2(id.x * 24u + channel * 8u);
        if((words.y & 0x80000000u) != 0u)
        {
            Fault(4u);
            return;
        }

        // Empty current sum and empty prior history resolve to positive zero
        // exactly. All metadata/prior-history checks above still run; skipping
        // two FP64 divisions here changes neither a sample nor its normalization.
        if(all(words == 0u) && (folded == 0u || (asuint(previous[channel]) & 0x7FFFFFFFu) == 0u))
        {
            result[channel] = 0.0f;
            continue;
        }

        precise double integer = double(words.y) * 4294967296.0 + double(words.x);
        const int power = exponent + 1 - int(precisionBits);
        // 2^power is a normal double for every legal code (power >= -149 - 62).
        precise double scale = asdouble(0u, uint(power + 1023) << 20u);
        precise double mean = (integer * scale) / double(attempts);
        precise double blended = folded == 0u ? mean : (double(folded) * double(previous[channel]) + mean) / double(folded + 1u);
        // Check in FP64 before conversion: finite float inputs alone do not prove
        // finite history after corrupted metadata or a failed accumulation.
        if(!(blended >= 0.0 && blended <= double(asfloat(0x7F7FFFFFu))))
        {
            Fault(16u);
            return;
        }

        result[channel] = float(blended);

        if(!Valid(asuint(result[channel])))
        {
            Fault(16u);
            return;
        }
    }

    history.Store4(id.x * 16u, asuint(result));
}

// A separate dispatch after decode/composite validation prevents cross-group
// races: a fault anywhere clears every history pixel before any consumer runs.
[numthreads(256, 1, 1)]
void CSInvalidate(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];

    if(control.Load(0u) != 0u && id.x < pixels)
    {
        RWByteAddressBuffer history = ResourceDescriptorHeap[historyIndex];
        history.Store4(id.x * 16u, asuint(float4(0, 0, 0, 1)));
    }
}

[numthreads(1, 1, 1)]
void CSFinish(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];
    const uint fault = control.Load(0u);

    if(fault == 0u)
    {
        control.Store(4u, control.Load(4u) + 1u);
    }
    else
    {
        control.Store(4u, 0u);
        control.Store(8u, fault);
    }

    // Saturating telemetry does not affect the all-attempt estimator denominator.
    const uint attemptedFrames = control.Load(12u);
    control.Store(12u, attemptedFrames == 0xFFFFFFFFu ? attemptedFrames : attemptedFrames + 1u);
}

// Composite can fault after the accumulation fold. Seal again without counting
// another measurement; the caller first invalidates every history pixel.
[numthreads(1, 1, 1)]
void CSSeal(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer control = ResourceDescriptorHeap[controlIndex];
    const uint fault = control.Load(0u);

    if(fault != 0u)
    {
        control.Store(4u, 0u);
        control.Store(8u, fault);
    }
}
