#ifndef ENGINE_PATH_TRACE_SAMPLE_RECORD
#define ENGINE_PATH_TRACE_SAMPLE_RECORD

// Shared raw-buffer ABI. Each field is one 16-byte row, never a packed float3.
#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#define PT_DIAG_UINT4(name) std::uint32_t name[4]{}
#define PT_DIAG_FLOAT4(name) float name[4]{}
#else
#define PT_DIAG_UINT4(name) uint4 name
#define PT_DIAG_FLOAT4(name) float4 name
#endif
struct PathTraceSampleRecord
{
    PT_DIAG_UINT4(meta);       // frame serial low, global sample index, old count, batch size
    PT_DIAG_UINT4(config);     // depth, target, seed, flags: cache=1, rejected=2, detail=4
    PT_DIAG_FLOAT4(sample);    // finalized raw estimate (before rejection), luminance
    PT_DIAG_FLOAT4(indirect);  // passive sum of post-primary transport, luminance
    PT_DIAG_FLOAT4(mean);      // actual progressive mean after this batch, luminance
    PT_DIAG_FLOAT4(primary);
    PT_DIAG_FLOAT4(secondary); // direct at vertex 1, weighted to camera
    PT_DIAG_FLOAT4(deeper);    // direct at vertices >=2, weighted to camera
    PT_DIAG_FLOAT4(miss);      // non-primary miss terms, weighted to camera
    PT_DIAG_FLOAT4(cache);     // actual substituted term, weighted to camera
    PT_DIAG_FLOAT4(dominant);  // largest individual visible secondary-light term by luminance
    PT_DIAG_FLOAT4(diffuse);   // dominant light's camera-weighted diffuse part at that vertex
    PT_DIAG_FLOAT4(specular);  // dominant light's camera-weighted remainder
    PT_DIAG_FLOAT4(factors);   // pre-MIS f*cos*Li RGB, cosine (not raw BSDF f)
    PT_DIAG_FLOAT4(incidence); // Li RGB, light distance
    PT_DIAG_FLOAT4(throughput);// incoming throughput RGB, incoming BSDF PDF
    PT_DIAG_FLOAT4(hit);       // first secondary distance, dominant roughness, light PDF, MIS/pdf weight
    PT_DIAG_UINT4(info);       // dominant vertex, light index/sentinel, light type/sentinel, material index
};
// R.9I.1D automatic capture: a fixed array of hashed pixel buckets, each a 32-byte header then one record.
// Header: [0] max indirect luminance bits (0 empty), [1] write lock, [2] owner pixel x | y << 16,
// [3] threshold crossings, [4] record lock failures, [5] mean lock failures, [6..7] reserved.
#ifdef __cplusplus
#define PT_DIAG_CONST constexpr std::uint32_t
#else
#define PT_DIAG_CONST static const uint
#endif
PT_DIAG_CONST PathTraceAutoBucketCount = 256;
PT_DIAG_CONST PathTraceAutoHeaderBytes = 32;
PT_DIAG_CONST PathTraceAutoBucketStride = PathTraceAutoHeaderBytes + 288;
#undef PT_DIAG_CONST
#ifdef __cplusplus
static_assert(sizeof(PathTraceSampleRecord) == 288);
static_assert(offsetof(PathTraceSampleRecord, info) == 272);
static_assert(PathTraceAutoBucketStride == PathTraceAutoHeaderBytes + sizeof(PathTraceSampleRecord));
#endif
#undef PT_DIAG_UINT4
#undef PT_DIAG_FLOAT4
#endif
