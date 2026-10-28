#ifndef ENGINE_PATH_TRACE_CAUSTIC_NUMERIC
#define ENGINE_PATH_TRACE_CAUSTIC_NUMERIC

// Dormant Stage 2 contract: exact nonnegative binary32 sum in units of 2^-149.
// Ten 32-bit words cover 320 bits; <=2^20 finite deposits need at most 297 bits.
// One attempted light path may deposit at most one RGB value in one pixel.
// No production rendering includes this file. Resolve is a separate numerical contract.
#ifdef __cplusplus
#include "PathTraceCausticClassification.h"
#include <bit>
#include <cstdint>
#define PT_NUM_UINT std::uint32_t
#define PT_NUM_FUNCTION inline
#else
#include "PathTraceCausticClassification.h"
#define PT_NUM_UINT uint
#define PT_NUM_FUNCTION
#endif

static const PT_NUM_UINT CausticSumWordCount = 10u;
static const PT_NUM_UINT CausticMaximumAttempts = 1u << 20u;
static const PT_NUM_UINT CausticSumBytesPerPixel = 3u * CausticSumWordCount * 4u;

struct CausticEncodedDeposit
{
    PT_NUM_UINT valid;
    PT_NUM_UINT firstWord;
    PT_NUM_UINT low;
    PT_NUM_UINT high;
};

PT_NUM_FUNCTION CausticEncodedDeposit EncodeCausticDeposit(float value)
{
    CausticEncodedDeposit result;
    result.valid = 0u;
    result.firstWord = 0u;
    result.low = 0u;
    result.high = 0u;

#ifdef __cplusplus
    PT_NUM_UINT bits = std::bit_cast<std::uint32_t>(value);
#else
    PT_NUM_UINT bits = asuint(value);
#endif
    PT_NUM_UINT magnitude = bits & 0x7FFFFFFFu;

    // Accept either signed zero; reject every negative, infinity and NaN without conversion.
    if(magnitude >= 0x7F800000u || ((bits & 0x80000000u) != 0u && magnitude != 0u))
    {
        return result;
    }

    PT_NUM_UINT exponent = magnitude >> 23u;
    PT_NUM_UINT significand = magnitude & 0x007FFFFFu;
    PT_NUM_UINT shift = 0u;

    if(exponent != 0u)
    {
        significand |= 0x00800000u;
        shift = exponent - 1u;
    }

    result.valid = 1u;
    result.firstWord = shift / 32u;
    PT_NUM_UINT offset = shift % 32u;
    result.low = significand << offset;

    // Never shift by 32: C++ and HLSL do not share semantics for that operation.
    if(offset != 0u)
    {
        result.high = significand >> (32u - offset);
    }

    return result;
}

#undef PT_NUM_FUNCTION
#undef PT_NUM_UINT
#endif
