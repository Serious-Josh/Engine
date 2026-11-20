// AIR.4F F3: the Editor's volumetric mist (EditorMistPass). Three stages, all reading F1's piece-aware mist field directly:
//   1. CSOccupancy: one flag per 8 x 8 x 8-cell brick of the window, set when any cell within one cell of the brick holds mist (the
//      reach of a sample's footprint), so a ray can skip a clear brick without changing any sample it takes;
//   2. CSRaymarch: per pixel of the reduced-resolution target, a ray from the camera through the window's box, ending at the scene
//      depth, sampled at fixed steps from a stable per-pixel offset; emission-absorption front to back, ending early when nearly
//      opaque; writes (premultiplied colour, transmittance) and the scene distance the ray ended at;
//   3. VSComposite / PSComposite: a full-screen triangle into the final scene image, upsampling depth-aware (the reduced samples whose
//      scene distance matches the pixel's) with straight-alpha blending of (colour / alpha, alpha = 1 - transmittance), which gives
//      scene * transmittance + colour.
// Mist colour is display-referred: it is composited into the final (tone-mapped) image as authored.
//
// AIR.4F F4.1 flow-advected Worley detail (when detailEnabled): each sample's density is the simulated density times
// lerp(1, noise / mean(noise), detailStrength), so mist stays absent where the simulation has none and its average is kept. The noise
// is a tileable 3D texture (wrap, trilinear) sampled at flow-advected coordinates by the dual-phase flow-map method: two phases half
// a period apart, each (x - v * tau) / detailScale + its cycle's offset, with tau the time since the phase's last restart (never the
// total time) and v the local piece-aware velocity (AirRenderMistAndVelocity); the phases crossfade (weights 1 - |2 * phase - 1|), so
// a phase restarts only while invisible. Colour is unchanged: only the density is modulated.
//
// AIR.4F F4.3 single scattering (lightingEnabled): the mist scatters the scene's light toward the camera instead of showing its authored
// colour. Each segment adds transmittance * (1 - exp(-extinction * density * segment)) * albedo * scattering * inScattered, where
// albedo is the mist's colour (premultiplied colour / density), scattering the scattering strength, and inScattered the light arriving
// at the sample weighted by the Henyey-Greenstein phase function (anisotropy g): the sun, shadowed by CSSunVisibility's per-piece
// visibility; each table Light, unshadowed (EvaluateSceneLightIncidence, as deferred lighting does); and (F4.3.1) a fraction of the
// environment's mean radiance, from the diffuse irradiance cube deferred lighting uses (EnvironmentRadiance). That is
// the exact integral over a segment of constant density and light, so F4.2's cut-off last segment is weighted correctly. Lit mist is
// scene radiance: the Renderer composites it into the HDR scene colour before tone mapping.

#include "AirPath.hlsli"
#include "AirMistRender.hlsli"
#include "SceneLights.hlsli"

cbuffer EditorMistConstants : register(b0)
{
    // Clip space (conventional depth: 0 near, 1 far) to world.
    row_major float4x4 clipToWorld;
    float3 cameraPosition;
    // Extinction per metre per unit of mist density (the opacity control).
    float extinction;
    AirFieldBinding field;
    uint mist0Index;
    uint mist1Index;
    uint occupancyIndex;
    uint depthIndex;
    uint2 fullSize;
    uint2 reducedSize;
    uint resultIndex;
    uint distanceIndex;
    // The march's step, at least `minimumStep` metres and long enough that the ray's span in the window needs at most maxSteps.
    uint maxSteps;
    float minimumStep;
    // AIR.4F F4.1 detail (AirMistRender.hlsli).
    AirMistDetail detail;
    uint noiseIndex;
    uint detailEnabled;
    // AIR.4F F4.2: the per-pixel start offset (0: every ray starts at its span's entry), and the composite's 4 x 4 tent (0: the F3 2 x 2
    // bilinear).
    uint jitterEnabled;
    uint wideUpsample;
    // AIR.4F F4.3: single scattering (0: F4.2's unlit colour), the sun visibility buffer (AirNoVisibility: the sun is unshadowed), the
    // scattering strength (a multiplier on the albedo) and the Henyey-Greenstein anisotropy g.
    uint lightingEnabled;
    uint visibilityIndex;
    float scattering;
    float anisotropy;
    // The direction the sun's light travels, and its value (colour x intensity, in render units, as deferred lighting uses it).
    // AIR.4F.2d.3e: the indirect scattering strength k (0 to 1; 0: no indirect fill, and CSIndirectFill does not run) and the gather's
    // reach in metres (CSIndirectFill).
    float3 sunDirection;
    float indirectScattering;
    float3 sunRadiance;
    float indirectReach;
    // AIR.4F F4.3.1: the fraction of the environment's mean radiance that reaches the mist (0: none), and the environment's diffuse
    // irradiance cube (UINT_MAX: none, and no ambient light).
    float ambient;
    uint irradianceIndex;
    // AIR.4F.2d.3d: the sun visibility samples per fluid piece (1, 2, 4 or 8; CSSunVisibility).
    uint visibilitySamples;
    float lightingPadding2;
    // AIR.4F F4.4: curl-warped detail (AirMistRender.hlsli), on when curlEnabled (it needs the detail on), and its texture.
    AirMistCurl curlField;
    uint curlIndex;
    uint curlEnabled;
    uint2 curlPadding;
    // AIR.4F F4.4.2: wisps (AirMistRender.hlsli), on when wispEnabled (they need the detail on), and the filament mask.
    AirMistWisps wisps;
    uint wispIndex;
    uint wispEnabled;
    // AIR.4F F4.4.2e: density-dependent detail (AirMistRender.hlsli), on when its strength is above 0 (it needs the detail on).
    float densityDetailStrength;
    float densityPadding;
    // AIR.4F.2d.2 (lit only): the global scattering tint (each 0 to 1, multiplying the albedo; extinction unchanged) and the
    // multiple-scattering strength s (0 to 1; 0: single scattering exactly as before). See ScatteringIsotropy.
    float3 scatteringTint;
    float multipleScattering;
    // AIR.4F.2d.3: whether the AIR mist is marched (0: haze only, no AIR field), the haze (on, extinction 1 / attenuation distance), the
    // transmittance below which the mist march stops, the haze's albedo (MarchAtmosphere). AIR.4F.2d.3b: the haze's and the AIR mist's
    // own near clearances (0: off), and the haze's height bounds (heightEnabled 0: unbounded).
    uint mistActive;
    uint hazeEnabled;
    float hazeExtinction;
    float opaqueTransmittance;
    float3 hazeAlbedo;
    float hazeNearDistance;
    float hazeNearTransition;
    // AIR.4F.2d.3a: the fraction of the environment's mean radiance that lights the haze (EnvironmentRadiance; 0: the sun and Lights
    // only), independent of the mist's `ambient`.
    float hazeAmbient;
    float mistNearDistance;
    float mistNearTransition;
    // AIR.4F.2d.3b: the layer's heights relative to the camera (world height minus the camera's), so no large coordinate cancels here.
    uint heightEnabled;
    float baseHeight;
    float maxHeight;
    float heightTransition;
    // V1a diagnostics (the froxel kernels only; FroxelSliceOf, FroxelDistanceOf): the camera-aligned sun visibility volume (one float per
    // froxel, x fastest, then y, then the depth slice), its size, the distance of slice 0's near face, S / ln(far / near) and its inverse,
    // and the jitter (FroxelJitterCentre, FroxelJitterHash with its seed, or V1b-1's FroxelJitterOffset) with its seed.
    uint froxelIndex;
    uint3 froxelSize;
    float froxelNear;
    float froxelSlicesPerLog;
    float froxelLogPerSlice;
    uint froxelSeed;
    uint froxelJitter;
    // V1b-1 diagnostics (AirVolumetricV1b1Report.md): the filtered visibility volume (a 3D R16_UNORM texture; its UAV for
    // CSFroxelResolve, its SRV for the *FroxelFiltered marches), the previous frame's resolved volume (SRV) and whether it is usable.
    uint froxelTextureIndex;
    uint froxelHistoryIndex;
    uint froxelHistoryValid;
    // V1b-1: the previous frame's world-to-clip transform, camera and slice mapping (CSFroxelResolve reprojects into them), the
    // current-frame weight of the exponential moving average (1: no history), and this frame's sub-froxel offset in [0, 1)^3 (with
    // FroxelJitterOffset, the same for every froxel).
    row_major float4x4 previousWorldToClip;
    float3 previousCameraPosition;
    float froxelBlend;
    float3 froxelOffset;
    float previousFroxelNear;
    uint3 previousFroxelSize;
    float previousFroxelSlicesPerLog;
    // V1b-2 diagnostics: the resolve's history rectification (FroxelClipNone: V1b-1's plain EMA; FroxelClipVariance: the history clamped
    // to mean +- froxelClipGamma x standard deviation of the traced centre and its six face neighbours, FroxelClipBounds).
    // froxelClipIncludesCurrent (an Engine adaptation, measured in AirVolumetricV1b2Report.md) widens the box to contain the centre's own
    // current value, which the statistics alone may exclude at one-froxel features.
    uint froxelClip;
    float froxelClipGamma;
    uint froxelClipIncludesCurrent;
    uint froxelClipPadding;
    // V1b-2 anti-flicker (Lerviks 2026, eqs. 3.3-3.5; with clipping only): the blend becomes lerp(froxelBlendMinimum, froxelBlend,
    // saturate(d / froxelFlickerDistance)) with d = |history - clipped history| (d_min = 0). froxelBlendMinimum >= froxelBlend: off.
    float froxelBlendMinimum;
    float froxelFlickerDistance;
    uint2 froxelFlickerPadding;
    // V1b-3 diagnostics (AirVolumetricV1b3Report.md). froxelDecorrelate: each froxel's sample is the frame's offset rotated by a fixed
    // per-froxel hash (Cranley-Patterson; only the jittered axes). froxelMoments: the resolved volumes hold a second channel, the EMA of
    // the traced value's square, so the resolve knows each froxel's own temporal deviation sigma_t. With it: FroxelClipHistoryAware
    // widens the spatial box to current +- froxelTemporalGamma sigma_t; froxelAdaptiveBlend raises the blend toward froxelAdaptiveBlendMax
    // as z = |current - history| / (sigma_t + FroxelDeviationFloor) rises from froxelAdaptiveLow to froxelAdaptiveHigh; froxelClipGated
    // applies variance clipping only where z exceeds froxelAdaptiveLow.
    uint froxelDecorrelate;
    uint froxelMoments;
    float froxelTemporalGamma;
    uint froxelAdaptiveBlend;
    float froxelAdaptiveLow;
    float froxelAdaptiveHigh;
    float froxelAdaptiveBlendMax;
    uint froxelClipGated;
    // V1c-2 diagnostics (AirVolumetricV1c2Report.md): the froxel indirect fill, CSIndirectFill's per-cell fill resampled at froxel centres
    // (an R16_UNORM 3D texture; its UAV for CSFroxelIndirect, its SRV for the *FroxelLit marches).
    uint froxelIndirectIndex;
    // V1c-2b diagnostics (AirVolumetricV1c2bReport.md): CSFroxelIndirect's fill-only reconstruction (FroxelIndirectFillAt; 0: V1c-2's
    // full AirRenderMistAndVelocity) and its skipping of froxels whose brick holds no mist (0: every froxel reconstructs).
    uint froxelIndirectFillOnly;
    uint froxelIndirectSkipEmpty;
    // V1c-4 diagnostics (AirVolumetricV1c4Report.md): the froxel local-light volume (R16G16B16A16_FLOAT 3D: rgb the table Lights'
    // in-scattering toward the camera; its UAV for CSFroxelLocalLights, its SRV for the *FroxelLitLocal marches).
    uint froxelLocalLightIndex;
    // V1d (AirVolumetricV1dReport.md): the integrated froxel renderer's volumes (R16G16B16A16_FLOAT 3D; each one's UAV for the pass that
    // writes it, its SRV after): the medium (rgb the mist's scattering coefficient, a its extinction), the integrated light (rgb the
    // scattered light accumulated from the camera to each slice's near face, a the transmittance there) and each slice's segment (rgb its
    // source term S, a its total extinction), and the near-source radius r: a Light's froxel share fades in from r to 2r, the composite
    // integrates the rest along each pixel's ray.
    uint froxelMediumIndex;
    uint froxelIntegratedIndex;
    uint froxelSegmentIndex;
    float froxelNearLightRadius;
    // V1d aliasing investigation (AirVolumetricV1dAliasingInvestigation.md): sub-samples per slice in CSFroxelIntegrate. 1 reads each
    // froxel's visibility, medium and fill at its centre; more sample them trilinearly at that many depths within the slice.
    uint froxelIntegrateSamples;
    // V1d aliasing investigation: nonzero reads the sun visibility from the cells (the production march's source) at each sub-sample in
    // CSFroxelIntegrate instead of from the froxel visibility volume.
    uint froxelIntegrateCellVisibility;
    // V1d correction: the cell-visibility volume (CSFroxelMedium's UAV, CSFroxelIntegrate's SRV) and the medium's AIR samples per froxel.
    uint froxelCellVisibilityIndex;
    uint froxelMediumSamples;
    // V1d experimental filter: CSFilterIntegrated's output (the reduced result's filtered copy, which the composite then reads).
    uint filteredResultIndex;
    // V1d filter diagnostic (temporary): bits 0-3 the taps' stride in reduced pixels (0: 1), bit 4 no depth weighting, bit 5 the output
    // is each pixel's accepted share of the kernel (its total weight over 16) in place of the filtered result.
    uint filterDiagnostic;
    // V1d distant banding: nonzero gives each slice one AIR sample per cell of its depth, at least froxelMediumSamples and at most 16,
    // in place of froxelMediumSamples for every slice.
    uint froxelMediumAdaptive;
    // V1d lateral experiment (diagnostic, temporary): nonzero samples each depth at 4 distinct camera rays across the froxel's XY footprint
    // (a rotated 2 x 2 grid) instead of its centre ray alone.
    uint froxelMediumLateral;
    // Raymarch dither experiment (diagnostic, temporary): PixelOffset's pattern. 0 the production Bayer 4 x 4 with a hashed offset per
    // 4 x 4 block, 1 the Bayer 4 x 4 alone, 2 the R2 low-discrepancy dither.
    uint ditherMode;
};

// AIR.4F F4.3: the scene's Light table (SceneLightConstants), as deferred lighting binds it.
cbuffer SceneLightTable : register(b1)
{
    uint sceneLightCount;
    uint sceneLightBufferIndex;
    uint2 sceneLightPadding;
};

// AIR.4F F4.3: the scene's acceleration structure, for the sun's visibility.
RaytracingAccelerationStructure sceneTopLevel : register(t0);

SamplerState noiseSampler : register(s0);
// V1b-1 diagnostics: trilinear with clamped addressing for the froxel visibility volumes (only the froxel-texture pipelines bind it).
SamplerState froxelSampler : register(s1);

static const uint BrickCells = 8;
// A ray ends once less than this fraction of the background shows through. AIR.4F.2d.3: CSRaymarchAtmosphere reads its threshold from
// the constants (`opaqueTransmittance`, 0.01 in production) so it can be measured; this kernel keeps its constant.
static const float OpaqueTransmittance = 0.01;
// A scene distance standing for "no geometry" (cleared depth).
static const float NoGeometry = 1e30;

AirPathField Path()
{
    AirPathField path;
    path.field = field;
    path.obstacleIndex = field.obstacleIndex;
    path.pieceIndex = field.pieceIndex;
    path.velocityIndex = field.velocityIndex;
    path.velocityPiece1Index = field.velocityPiece1Index;
    path.ambient = float3(0.0, 0.0, 0.0);
    return path;
}

AirMistField Mist()
{
    AirMistField mist;
    mist.piece0Index = mist0Index;
    mist.piece1Index = mist1Index;
    return mist;
}

uint3 BrickCounts()
{
    return field.dimensions / BrickCells;
}

// The brick (in slot space, so it follows the toroidal storage) holding a cell in the window.
uint BrickOf(int3 cell)
{
    const uint3 brick = AirSlotOf(field, cell) / BrickCells;
    const uint3 counts = BrickCounts();
    return brick.x + counts.x * (brick.y + counts.y * brick.z);
}

groupshared uint g_occupied;

// One group per brick. The window's minimum is a multiple of 8 cells (AirField::RecentreStep), so a slot brick's 8 cells per axis are
// contiguous in the world.
[numthreads(8, 8, 8)]
void CSOccupancy(uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    if(threadIndex == 0)
    {
        g_occupied = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    const int3 first = AirCellOfSlot(field, group * BrickCells) - 1;

    // The brick and a one-cell margin: 10 x 10 x 10 cells over 512 threads.
    for(uint index = threadIndex; index < 1000; index += 512)
    {
        const int3 cell = first + int3(index % 10, (index / 10) % 10, index / 100);

        if(AirContains(field, cell) && (AirPieceMist(Path(), Mist(), cell, 0).a > 0.0 || AirPieceMist(Path(), Mist(), cell, 1).a > 0.0))
        {
            InterlockedOr(g_occupied, 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if(threadIndex == 0)
    {
        const uint3 counts = BrickCounts();
        RWByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
        occupancy.Store((group.x + counts.x * (group.y + counts.y * group.z)) * 4, g_occupied);
    }
}

// AIR.4F.2d.3d: the cell-space point (0 to 1 per axis) of sample `sample` of `count` in a whole cell: a fixed pattern spread over the
// cell (2: a diagonal pair; 4: a regular tetrahedron; 8: the eight octant centres), mirrored in x for half the cells (`mirror`, a hash
// of the world cell) so the pattern does not repeat in step with the grid.
float3 SunSamplePattern(uint sampleIndex, uint count, bool mirror)
{
    float3 location = float3(0.5, 0.5, 0.5);

    if(count == 2)
    {
        location = sampleIndex == 0 ? float3(0.25, 0.25, 0.25) : float3(0.75, 0.75, 0.75);
    }
    else if(count == 4)
    {
        const float3 tetrahedron[4] = {float3(0.25, 0.25, 0.25), float3(0.75, 0.75, 0.25), float3(0.75, 0.25, 0.75), float3(0.25, 0.75, 0.75)};
        location = tetrahedron[sampleIndex];
    }
    else if(count == 8)
    {
        location = float3(float(sampleIndex & 1u), float((sampleIndex >> 1) & 1u), float((sampleIndex >> 2) & 1u)) * 0.5 + 0.25;
    }

    return mirror ? float3(1.0 - location.x, location.y, location.z) : location;
}

// AIR.4F.2d.3d: whether a world cell's sample pattern is mirrored: one bit of a mixed hash of its coordinates, fixed per world cell, so
// it does not change as the camera moves or the window recentres.
bool SunSampleMirror(int3 cell)
{
    uint hash = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    return (hash & 1u) != 0;
}

// AIR.4F.2d.3d: the start of sample `sample` of a piece's visibility, in cell space. One sample: the piece's anchor (as before 2d.3d).
// Several: the pattern's point in a whole cell; in a cell cut by geometry, the centre of the piece's fluid sub-cell nearest the pattern's
// point, so no sample starts in solid geometry or in the other piece (lowest sub-cell index on ties).
float3 SunSamplePoint(uint64_t piece, uint sampleIndex, uint count, bool mirror)
{
    if(count <= 1)
    {
        return AirPieceAnchor(piece);
    }

    const float3 target = SunSamplePattern(sampleIndex, count, mirror);

    if(piece == AirSubcellsAll)
    {
        return target;
    }

    float3 nearest = AirPieceAnchor(piece);
    float nearestDistance = 1e30;
    uint64_t remaining = piece;

    while(remaining != 0)
    {
        const uint low = uint(remaining & 0xFFFFFFFFu);
        const uint index = low != 0 ? firstbitlow(low) : 32 + firstbitlow(uint(remaining >> 32));
        remaining &= remaining - 1;
        const float3 centre = (float3(index & 3u, (index >> 2) & 3u, index >> 4) + 0.5) * 0.25;
        const float3 offset = centre - target;
        const float distance = dot(offset, offset);

        if(distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = centre;
        }
    }

    return nearest;
}

// AIR.4F F4.3: the sun's visibility per fluid piece, for every cell of a brick that a raymarch sample's footprint can reach: the brick or
// one of its 26 neighbours is occupied (occupancy already reaches one cell beyond a brick's mist). Each ray follows the sun's centre
// direction with the conventions of the ray-traced sun shadow (RayTracedShadow.hlsl): every instance (mask 0xFF), geometry opaque, the
// first hit ends it, TMin 0.001 m, TMax 10 km. AIR.4F.2d.3d: `visibilitySamples` rays per piece (SunSamplePoint), and the fraction that
// reaches the sun stored as 8-bit coverage: bits 0 to 7 piece 0, bits 8 to 15 piece 1 (0: shadowed, 255: lit); bits 16 to 31 are 0.
[numthreads(8, 8, 8)]
void CSSunVisibility(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const int3 counts = int3(BrickCounts());
    ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
    bool needed = false;

    for(int neighbour = 0; neighbour < 27; ++neighbour)
    {
        const int3 brick = (int3(group) + int3(neighbour % 3, (neighbour / 3) % 3, neighbour / 9) - 1 + counts) % counts;
        needed = needed || occupancy.Load((brick.x + counts.x * (brick.y + counts.y * brick.z)) * 4) != 0;
    }

    if(!needed)
    {
        return;
    }

    const uint3 slot = group * BrickCells + thread;
    const int3 cell = AirCellOfSlot(field, slot);
    const AirCell pieces = AirLoadCell(Path(), cell);
    uint bits = 0;
    const uint samples = clamp(visibilitySamples, 1u, 8u);
    const bool mirror = SunSampleMirror(cell);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;

        if(mask == 0)
        {
            continue;
        }

        uint visible = 0;

        for(uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
        {
            RayDesc ray;
            ray.Origin = (float3(cell) + SunSamplePoint(mask, sampleIndex, samples, mirror)) * field.cellSize;
            ray.Direction = -normalize(sunDirection);
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
            query.TraceRayInline(sceneTopLevel, RAY_FLAG_NONE, 0xFF, ray);
            query.Proceed();

            if(query.CommittedStatus() == COMMITTED_NOTHING)
            {
                ++visible;
            }
        }

        // Rounded to the nearest of 256 levels: all samples lit is 255, none 0.
        bits |= ((visible * 255u + samples / 2u) / samples) << (8u * piece);
    }

    RWByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    visibility.Store(AirVelocityAddress(field, slot) / 4, bits);
}

// AIR.4F F4.3: the Henyey-Greenstein phase function (per steradian) for the cosine between the direction toward the light and the ray's
// direction (1: looking straight at the light, where g > 0 scatters most).
float HenyeyGreenstein(float cosine, float g)
{
    const float denominator = max(1.0 + g * g - 2.0 * g * cosine, 1e-6);
    return (1.0 - g * g) / (4.0 * 3.14159265 * denominator * sqrt(denominator));
}

// AIR.4F F4.3.1: the environment's mean radiance over all directions, in render units: the mean of the diffuse irradiance E(n) over
// every normal n is pi times the mean radiance, estimated here from the six axis normals. It is what an isotropic medium scatters from
// an environment it sees unoccluded; deferred lighting's environment diffuse reads the same cube ((albedo / pi) x E(n)).
float3 EnvironmentRadiance()
{
    if(irradianceIndex == 0xFFFFFFFFu)
    {
        return float3(0.0, 0.0, 0.0);
    }

    TextureCube<float4> irradiance = ResourceDescriptorHeap[NonUniformResourceIndex(irradianceIndex)];
    const float3 axes[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    float3 sum = float3(0.0, 0.0, 0.0);

    for(uint axis = 0; axis < 6; ++axis)
    {
        sum += irradiance.SampleLevel(noiseSampler, axes[axis], 0.0).rgb;
    }

    return sum / (6.0 * 3.14159265);
}

// AIR.4F.2d.2: how far the sun's phase function has turned isotropic at a sample: m = s (1 - exp(-albedo x opticalDepth)), with
// opticalDepth the optical depth the view ray has crossed before the sample and albedo the sample's mean scattering albedo. Light
// arriving deep in thick, bright mist has scattered many times, and higher orders lose their direction; m is 0 in thin or dark mist.
float ScatteringIsotropy(float opticalDepth, float meanAlbedo)
{
    return multipleScattering > 0.0 ? multipleScattering * (1.0 - exp(-meanAlbedo * opticalDepth)) : 0.0;
}

// AIR.4F F4.3: the light scattered toward the camera at a world point, per unit of scattering coefficient: the sun (seen by
// `sunVisibility` of it), each table Light and the ambient radiance (`ambientRadiance`, isotropic), each weighted by the phase function.
// AIR.4F.2d.2: the sun's phase is (1 - isotropy) HG + isotropy / (4 pi). Both integrate to 1 over the sphere, so the sun's scattered
// energy is unchanged at any isotropy, only spread more evenly; the visibility still multiplies it, so shadows stay shadowed.
// AIR.4F.2d.3e: a separate isotropic term for sunlight that other mist scattered first: k x (1 - V) x I x E / (4 pi), with I the blended
// CSIndirectFill (haze passes 0). Only the unlit share 1 - V receives it, so sunlit mist is unchanged.
float3 InScattered(float3 world, float3 rayDirection, float sunVisibility, float3 ambientRadiance, float isotropy, float indirect)
{
    float3 light = ambientRadiance;

    if(sunVisibility > 0.0)
    {
        const float directional = HenyeyGreenstein(dot(-normalize(sunDirection), rayDirection), anisotropy);
        const float phase = isotropy > 0.0 ? lerp(directional, 1.0 / (4.0 * 3.14159265), isotropy) : directional;
        light += sunRadiance * phase * sunVisibility;
    }

    if(indirectScattering > 0.0 && indirect > 0.0)
    {
        light += sunRadiance * (indirectScattering * (1.0 - sunVisibility) * indirect / (4.0 * 3.14159265));
    }

    const uint count = sceneLightCount;

    for(uint index = 0; index < count; ++index)
    {
        float3 directionToLight;
        const float3 incidence = EvaluateSceneLightIncidence(SceneLightLoad(sceneLightBufferIndex, index), world, directionToLight);

        if(any(incidence > 0.0))
        {
            light += incidence * HenyeyGreenstein(dot(directionToLight, rayDirection), anisotropy);
        }
    }

    return light;
}

// V1c-4 diagnostics: InScattered's two parts for the froxel local-light path. They repeat its code rather than InScattered calling them:
// splitting InScattered itself reorders the production CSRaymarch's DXIL (measured), so the production function stays as it was.
// AddSceneLights: the table Lights' term added onto `light` (CSFroxelLocalLights stores it from 0, per froxel, along the froxel's ray).
float3 AddSceneLights(float3 world, float3 rayDirection, float3 light)
{
    const uint count = sceneLightCount;

    for(uint index = 0; index < count; ++index)
    {
        float3 directionToLight;
        const float3 incidence = EvaluateSceneLightIncidence(SceneLightLoad(sceneLightBufferIndex, index), world, directionToLight);

        if(any(incidence > 0.0))
        {
            light += incidence * HenyeyGreenstein(dot(directionToLight, rayDirection), anisotropy);
        }
    }

    return light;
}

// InScatteredBase: InScattered without the table Lights (the ambient, the sun with its isotropy, and the indirect fill).
float3 InScatteredBase(float3 world, float3 rayDirection, float sunVisibility, float3 ambientRadiance, float isotropy, float indirect)
{
    float3 light = ambientRadiance;

    if(sunVisibility > 0.0)
    {
        const float directional = HenyeyGreenstein(dot(-normalize(sunDirection), rayDirection), anisotropy);
        const float phase = isotropy > 0.0 ? lerp(directional, 1.0 / (4.0 * 3.14159265), isotropy) : directional;
        light += sunRadiance * phase * sunVisibility;
    }

    if(indirectScattering > 0.0 && indirect > 0.0)
    {
        light += sunRadiance * (indirectScattering * (1.0 - sunVisibility) * indirect / (4.0 * 3.14159265));
    }

    return light;
}

// V1c-4: InScattered with the table Lights' term from the froxel local-light volume (FroxelLocalLightsFiltered) in place of the
// per-sample loop; the ambient, sun and indirect terms are InScattered's own.
float3 InScatteredFroxelLights(float3 world, float3 rayDirection, float sunVisibility, float3 ambientRadiance, float isotropy, float indirect,
    float3 localLights)
{
    return InScatteredBase(world, rayDirection, sunVisibility, ambientRadiance, isotropy, indirect) + localLights;
}

// A stable per-pixel offset in [0, 1), so sampling bands become fine noise that does not move (F4.2): every 4 x 4 block holds the 16
// Bayer ranks, so the composite's 4 x 4 tent averages a full set of offsets, and each block's ranks are rotated by a hash of the
// block, so the pattern does not repeat. F3's interleaved gradient noise is a linear ramp per row and column: against a ray length
// that changes smoothly across the floor it beat into repeating arcs.
float PixelOffset(uint2 pixel)
{
    static const uint Bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

    // Raymarch dither experiment: the Bayer 4 x 4 alone (each aligned 4 x 4 block holds all 16 offsets once), or the R2 dither (Roberts'
    // plastic-number sequence over the pixel lattice: no period, little low-frequency energy, a blue-noise approximation).
    if(ditherMode == 1)
    {
        return (float(Bayer[(pixel.x & 3u) + 4u * (pixel.y & 3u)]) + 0.5) / 16.0;
    }

    if(ditherMode == 2)
    {
        return frac(0.5 + 0.7548776662 * float(pixel.x) + 0.5698402910 * float(pixel.y));
    }

    uint hash = (pixel.x >> 2) * 1973u + (pixel.y >> 2) * 9277u + 26699u;
    hash = (hash ^ 61u) ^ (hash >> 16);
    hash *= 9u;
    hash ^= hash >> 4;
    hash *= 0x27d4eb2du;
    hash ^= hash >> 15;
    return frac((float(Bayer[(pixel.x & 3u) + 4u * (pixel.y & 3u)]) + 0.5) / 16.0 + float(hash & 0xFFFFFFu) / 16777216.0);
}

// The world point at a clip position.
float3 WorldAt(float2 clip, float depth)
{
    const float4 world = mul(clipToWorld, float4(clip, depth, 1.0));
    return world.xyz / world.w;
}

// The ray's entry and exit distances through the box [low, high]; entry > exit when it misses.
float2 BoxSpan(float3 origin, float3 direction, float3 low, float3 high)
{
    const float3 inverse = 1.0 / direction;
    const float3 first = (low - origin) * inverse;
    const float3 second = (high - origin) * inverse;
    const float3 nearer = min(first, second);
    const float3 farther = max(first, second);
    return float2(max(max(max(nearer.x, nearer.y), nearer.z), 0.0), min(min(farther.x, farther.y), farther.z));
}

// ================================================================================================ V1a: froxel sun visibility
//
// A camera-aligned volume of sun visibility (AirVolumetricV1aReport.md), diagnostic only: CSFroxelSunVisibility fills it and the
// *Froxel raymarch kernels read it in place of the per-piece cell visibility (AirPieceSunVisibility); every other quantity, the indirect
// fill included, comes from the existing paths.
//
// Froxel (x, y, z) covers the reduced target's uv in [x, x + 1) / froxelSize.x by [y, y + 1) / froxelSize.y, and the camera distance
// (along the pixel's ray, the raymarch's t) between FroxelDistanceOf(z) and FroxelDistanceOf(z + 1). Slices grow geometrically from
// froxelNear to the far distance (the window's farthest corner): t(s) = near (far / near)^(s / S), s(t) = S ln(t / near) / ln(far / near),
// so every slice spans the same ratio of distances and near slices are thin. Distances before the near face map to slice 0.

// Where each froxel's sample lies inside it (froxelJitter): its centre; V1a's per-froxel hash of its index and froxelSeed (the same
// every frame); or (V1b-1) the frame's froxelOffset, shared by every froxel and advanced per frame by EditorMistPass.
static const uint FroxelJitterCentre = 0;
static const uint FroxelJitterHash = 1;
static const uint FroxelJitterOffset = 2;

// V1b-2: the resolve's history rectification (froxelClip).
static const uint FroxelClipNone = 0;
static const uint FroxelClipVariance = 1;
// V1b-3: the spatial box widened by the froxel's own temporal deviation (froxelMoments).
static const uint FroxelClipHistoryAware = 2;
// V1b-3: froxelDecorrelate's values.
static const uint FroxelDecorrelateNone = 0;
static const uint FroxelDecorrelateDepth = 1;
static const uint FroxelDecorrelateAll = 2;
// V1b-3: the smallest temporal deviation z divides by, so a froxel whose samples never varied still gives a finite z.
static const float FroxelDeviationFloor = 0.02;

// V1b-2: what the trace stores for a froxel it did not trace (no mist near) on the texture path, so the resolve can tell an evaluated
// visibility from a stand-in. V1a's buffer lookup still reads 1 there (the trace stores 1 when no froxel texture is bound).
static const float FroxelUntraced = -1.0;

// Which sun visibility a march reads (a literal per kernel, so the production kernels compile without the froxel code): the per-piece
// cell visibility (production), V1a's froxel buffer with an 8-load trilinear lookup, or V1b-1's froxel texture with hardware filtering.
static const uint SunFromCells = 0;
static const uint SunFromFroxelBuffer = 1;
static const uint SunFromFroxelTexture = 2;
// V1c-2: the froxel texture's sun visibility and the froxel indirect fill (both lighting terms from froxels; the cell word is still
// loaded with the density, which the march samples per step as before).
static const uint SunFromFroxelLit = 3;
// V1c-4: SunFromFroxelLit, and the table Lights' in-scattering from the froxel local-light volume in place of the per-sample loop.
static const uint SunFromFroxelLitLocal = 4;

// The continuous slice coordinate (0 at froxelNear, froxelSize.z at the far distance) of a camera distance.
float FroxelSliceOf(float distance)
{
    return log(max(distance, froxelNear) / froxelNear) * froxelSlicesPerLog;
}

// The camera distance of a continuous slice coordinate (the inverse of FroxelSliceOf above the near face).
float FroxelDistanceOf(float slice)
{
    return froxelNear * exp(slice * froxelLogPerSlice);
}

// A froxel's jitter in [0, 1)^3 from its index and the seed (deterministic: the same seed gives the same jitter every frame).
float3 FroxelJitter(uint froxel)
{
    uint state = froxel * 747796405u + froxelSeed * 2891336453u + 1u;
    float3 jitter;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        state = state * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        word = (word >> 22u) ^ word;
        jitter[axis] = float(word >> 8) / 16777216.0;
    }

    return jitter;
}

// The camera ray's direction through a uv of the target (as the raymarch builds it).
float3 FroxelRayDirection(float2 uv)
{
    const float2 clip = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(WorldAt(clip, 1.0) - cameraPosition);
}

// Whether a froxel needs a ray: some brick within one brick of the cell under its sample holds mist (CSOccupancy's flags already reach
// one cell beyond a brick's mist). A froxel wider than a brick could overlap mist beyond that neighbourhood, so it always traces.
bool FroxelNeedsRay(float3 world, float radius)
{
    const int3 cell = int3(floor(world * field.inverseCellSize));

    if(radius > float(BrickCells) * field.cellSize)
    {
        return true;
    }

    ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];

    for(int neighbour = 0; neighbour < 27; ++neighbour)
    {
        const int3 offset = int3(neighbour % 3, (neighbour / 3) % 3, neighbour / 9) - 1;
        const int3 nearby = cell + offset * int(BrickCells);

        if(AirContains(field, nearby) && occupancy.Load(BrickOf(nearby) * 4) != 0)
        {
            return true;
        }
    }

    return false;
}

// One ray toward the sun per froxel, from a point jittered inside the froxel (its centre without jitter), with CSSunVisibility's
// ray (flags, TMin, TMax). Froxels with no mist near them hold 1 (lit): never darker than the path they replace, and the raymarch only
// reads them beside mist.
[numthreads(4, 4, 4)]
void CSFroxelSunVisibility(uint3 froxel : SV_DispatchThreadID)
{
    if(any(froxel >= froxelSize))
    {
        return;
    }

    const uint index = froxel.x + froxelSize.x * (froxel.y + froxelSize.y * froxel.z);
    float3 offset = float3(0.5, 0.5, 0.5);

    if(froxelJitter == FroxelJitterHash)
    {
        offset = FroxelJitter(index);
    }
    else if(froxelJitter == FroxelJitterOffset)
    {
        offset = froxelOffset;

        // V1b-3: a fixed per-froxel rotation (mod 1) of the shared sequence. Each froxel still visits the 16 stratified offsets over a
        // cycle, but neighbours sample different points of their froxels in the same frame. FroxelDecorrelateDepth rotates depth only
        // (the depth-only sequence keeps x and y centred); FroxelDecorrelateAll rotates every axis (the full 3D sequence).
        if(froxelDecorrelate != FroxelDecorrelateNone)
        {
            const float3 rotation = FroxelJitter(index);
            offset.z = frac(froxelOffset.z + rotation.z);

            if(froxelDecorrelate == FroxelDecorrelateAll)
            {
                offset.xy = frac(froxelOffset.xy + rotation.xy);
            }
        }
    }

    const float2 uv = (float2(froxel.xy) + offset.xy) / float2(froxelSize.xy);
    const float distance = FroxelDistanceOf(float(froxel.z) + offset.z);
    const float3 direction = FroxelRayDirection(uv);
    const float3 world = cameraPosition + direction * distance;

    // The froxel's half extent: its depth and its width (the angle between neighbouring columns) at this distance.
    const float depthExtent = FroxelDistanceOf(float(froxel.z) + 1.0) - FroxelDistanceOf(float(froxel.z));
    const float3 sideways = FroxelRayDirection(uv + float2(1.0 / float(froxelSize.x), 0.0));
    const float3 upwards = FroxelRayDirection(uv + float2(0.0, 1.0 / float(froxelSize.y)));
    const float widthExtent = max(length(sideways - direction), length(upwards - direction)) * distance;
    const float radius = 0.5 * length(float3(depthExtent, widthExtent, widthExtent));
    // V1b-2: on the texture path an untraced froxel stores FroxelUntraced; CSFroxelResolve writes it as 1, as V1a stores it.
    float visibility = froxelTextureIndex != 0xFFFFFFFFu ? FroxelUntraced : 1.0;

    // A froxel whose point lies just outside the window still traces when mist is near: rays leaving the window interpolate with it.
    if(FroxelNeedsRay(world, radius))
    {
        RayDesc ray;
        ray.Origin = world;
        ray.Direction = -normalize(sunDirection);
        ray.TMin = 0.001;
        ray.TMax = 10000.0;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
        query.TraceRayInline(sceneTopLevel, RAY_FLAG_NONE, 0xFF, ray);
        query.Proceed();
        visibility = query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
    }

    RWByteAddressBuffer froxels = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndex)];
    froxels.Store(index * 4, asuint(visibility));
}

// The froxel volume's sun visibility at a raymarch sample: trilinear between froxel centres in uv and in the slice coordinate, clamped
// to the volume (no extrapolation past the first and last centres).
float FroxelSunVisibility(float2 uv, float distance)
{
    ByteAddressBuffer froxels = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndex)];
    const float3 limit = float3(froxelSize) - 1.0;
    const float3 position = clamp(float3(uv * float2(froxelSize.xy), FroxelSliceOf(distance)) - 0.5, 0.0, limit);
    const uint3 low = uint3(floor(position));
    const uint3 high = min(low + 1, froxelSize - 1);
    const float3 weight = position - float3(low);
    float corners[8];

    [unroll]
    for(uint corner = 0; corner < 8; ++corner)
    {
        const uint3 at = uint3((corner & 1) != 0 ? high.x : low.x, (corner & 2) != 0 ? high.y : low.y, (corner & 4) != 0 ? high.z : low.z);
        corners[corner] = asfloat(froxels.Load((at.x + froxelSize.x * (at.y + froxelSize.y * at.z)) * 4));
    }

    const float4 alongX = lerp(float4(corners[0], corners[2], corners[4], corners[6]), float4(corners[1], corners[3], corners[5], corners[7]),
        weight.x);
    const float2 alongY = lerp(alongX.xz, alongX.yw, weight.y);
    return lerp(alongY.x, alongY.y, weight.z);
}

// ================================================================================================ V1b-1: filtered and accumulated froxels
//
// AirVolumetricV1b1Report.md. The same volume as a 3D texture read with hardware trilinear filtering. Texel (x, y, z)'s centre lies at
// normalized ((x, y, z) + 0.5) / froxelSize, which is froxel (x, y, z)'s centre in (uv, slice / S): the lookup below reads the texture at
// (uv, FroxelSliceOf(t) / S), and clamped addressing equals FroxelSunVisibility's clamp to the first and last centres. The only
// difference from that manual lookup is the hardware's fixed-point filter weights.

float FroxelSunVisibilityFiltered(float2 uv, float distance)
{
    Texture3D<float> volume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelTextureIndex)];
    const float3 coordinate = float3(uv, FroxelSliceOf(distance) / float(froxelSize.z));
    return volume.SampleLevel(froxelSampler, coordinate, 0.0);
}

// A world point's normalized coordinate in the previous frame's volume, through the previous frame's camera, projection and slice
// mapping (its far distance followed its camera). False when the point lies outside the volume the previous frame covered: behind
// its camera, outside its view in x or y, before its near face or beyond its last slice.
bool FroxelHistoryCoordinate(float3 world, out float3 coordinate)
{
    coordinate = float3(0.0, 0.0, 0.0);
    const float4 clip = mul(previousWorldToClip, float4(world, 1.0));

    if(!(clip.w > 0.0))
    {
        return false;
    }

    const float2 ndc = clip.xy / clip.w;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const float distance = length(world - previousCameraPosition);

    if(!(distance >= previousFroxelNear))
    {
        return false;
    }

    const float slice = log(distance / previousFroxelNear) * previousFroxelSlicesPerLog;
    coordinate = float3(uv, slice / float(previousFroxelSize.z));
    return all(coordinate >= 0.0) && all(coordinate <= 1.0);
}

// V1b-2: variance clipping (Lerviks 2026, eqs. 2.3-2.8, the 6-tap neighbourhood of its Frostbite implementation). Over the traced
// centre and its traced face neighbours (N of them; neighbours outside the volume or untraced are left out): m1 = sum c, m2 = sum c^2,
// mu = m1 / N, sigma = sqrt(|m2 / N - mu^2|), and the history is limited to [mu - gamma sigma, mu + gamma sigma]. With one scalar
// channel, the thesis's clip toward the box centre is exactly this clamp, and no colour space applies.
float2 FroxelClipBounds(uint3 froxel, float current, ByteAddressBuffer traced)
{
    static const int3 Faces[6] = {int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0), int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1)};
    float firstMoment = current;
    float secondMoment = current * current;
    float count = 1.0;

    [unroll]
    for(uint face = 0; face < 6; ++face)
    {
        const int3 neighbour = int3(froxel) + Faces[face];

        if(all(neighbour >= 0) && all(neighbour < int3(froxelSize)))
        {
            const uint at = uint(neighbour.x) + froxelSize.x * (uint(neighbour.y) + froxelSize.y * uint(neighbour.z));
            const float value = asfloat(traced.Load(at * 4));

            if(value >= 0.0)
            {
                firstMoment += value;
                secondMoment += value * value;
                count += 1.0;
            }
        }
    }

    const float mean = firstMoment / count;
    const float deviation = sqrt(abs(secondMoment / count - mean * mean));
    float lower = mean - froxelClipGamma * deviation;
    float upper = mean + froxelClipGamma * deviation;

    if(froxelClipIncludesCurrent != 0)
    {
        lower = min(lower, current);
        upper = max(upper, current);
    }

    return float2(lower, upper);
}

// One thread per froxel: this frame's traced visibility (V1a's buffer) blended into the previous frame's resolved volume, reprojected
// at the froxel's centre, as an exponential moving average with current-frame weight froxelBlend; written into this frame's volume.
// Sun visibility is a function of world position, the sun's direction and the occluders only, so a reprojected world point keeps its
// value while the camera moves; EditorMistPass invalidates the whole history for the cases a reprojection cannot detect (the sun's
// direction, resizes, configuration, cuts). Moving occluders are not detected (V1b-2).
[numthreads(4, 4, 4)]
void CSFroxelResolve(uint3 froxel : SV_DispatchThreadID)
{
    if(any(froxel >= froxelSize))
    {
        return;
    }

    const uint index = froxel.x + froxelSize.x * (froxel.y + froxelSize.y * froxel.z);
    ByteAddressBuffer traced = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndex)];
    const float tracedValue = asfloat(traced.Load(index * 4));
    // An untraced froxel resolves as V1b-1 resolved its stored 1; it never contributes to, nor is clipped by, the statistics.
    const bool untraced = tracedValue < 0.0;
    const float current = untraced ? 1.0 : saturate(tracedValue);
    float resolved = current;
    float resolvedSecondMoment = current * current;

    if(froxelHistoryValid != 0)
    {
        const float2 uv = (float2(froxel.xy) + 0.5) / float2(froxelSize.xy);
        const float3 world = cameraPosition + FroxelRayDirection(uv) * FroxelDistanceOf(float(froxel.z) + 0.5);
        float3 coordinate;

        if(FroxelHistoryCoordinate(world, coordinate))
        {
            // V1b-3: with froxelMoments the history's second channel is the EMA of the squared samples, so
            // sigma_t = sqrt(m2 - m1^2) is the froxel's own sample spread (about 0.5 where jitter flips a thin wall in and out, 0 where
            // the froxel has been steadily lit or shadowed), and z measures the disagreement in units of it.
            Texture3D<float2> history = ResourceDescriptorHeap[NonUniformResourceIndex(froxelHistoryIndex)];
            const float2 reprojected = froxelMoments != 0 ? history.SampleLevel(froxelSampler, coordinate, 0.0) :
                float2(history.SampleLevel(froxelSampler, coordinate, 0.0).x, 0.0);
            const float temporalDeviation = sqrt(max(reprojected.y - reprojected.x * reprojected.x, 0.0));
            const float disagreement = abs(current - reprojected.x) / (temporalDeviation + FroxelDeviationFloor);
            float previous = reprojected.x;
            float blend = froxelBlend;
            const bool clipped = froxelClip != FroxelClipNone && !untraced && (froxelClipGated == 0 || disagreement > froxelAdaptiveLow);

            if(clipped)
            {
                float2 bounds = FroxelClipBounds(froxel, current, traced);

                // V1b-3: history-aware bounds also accept what this froxel's own recent samples make plausible around the current one.
                // The widening is at most the deviation of samples the froxel actually traced, so a stale value cannot widen it.
                if(froxelClip == FroxelClipHistoryAware)
                {
                    bounds.x = min(bounds.x, current - froxelTemporalGamma * temporalDeviation);
                    bounds.y = max(bounds.y, current + froxelTemporalGamma * temporalDeviation);
                }

                previous = clamp(reprojected.x, bounds.x, bounds.y);

                // A history just clipped (small d) is blended in more slowly, damping the flicker of impulses the clip lets through.
                if(froxelBlendMinimum < froxelBlend)
                {
                    const float clipDistance = abs(reprojected.x - previous);
                    blend = lerp(froxelBlendMinimum, froxelBlend, saturate(clipDistance / froxelFlickerDistance));
                }
            }

            // V1b-3: adaptive blending: a disagreement beyond the froxel's own spread weights the current sample more.
            if(froxelAdaptiveBlend != 0 && !untraced)
            {
                const float weight = saturate((disagreement - froxelAdaptiveLow) / max(froxelAdaptiveHigh - froxelAdaptiveLow, 1e-3));
                blend = lerp(blend, froxelAdaptiveBlendMax, weight);
            }

            resolved = lerp(previous, current, blend);
            // The second moment follows the same blend from the reprojected one, kept consistent with the first: for values in [0, 1],
            // m1^2 <= m2 <= m1.
            const float secondMoment = lerp(reprojected.y, current * current, blend);
            resolvedSecondMoment = clamp(secondMoment, resolved * resolved, resolved);
        }
    }

    // A non-finite value (none is expected) never enters the history.
    const bool finiteResult = isfinite(resolved) && isfinite(resolvedSecondMoment);
    resolved = finiteResult ? saturate(resolved) : current;
    resolvedSecondMoment = finiteResult ? saturate(resolvedSecondMoment) : current * current;

    if(froxelMoments != 0)
    {
        RWTexture3D<unorm float2> output = ResourceDescriptorHeap[NonUniformResourceIndex(froxelTextureIndex)];
        output[froxel] = float2(resolved, resolvedSecondMoment);
    }
    else
    {
        RWTexture3D<unorm float> output = ResourceDescriptorHeap[NonUniformResourceIndex(froxelTextureIndex)];
        output[froxel] = resolved;
    }
}

// AIR.4F.2d.3: environmental haze and near clearance (Documentation/Air/AirEnvironmentalHaze4F2d3.md). Used instead of the march above
// whenever haze or the near clearance is on, or the AIR mist is not marched; otherwise the march above runs unchanged, so a scene without
// them renders bit for bit as before.
//
// The medium along the ray: extinction w_mist(d) sigma_mist + w_haze(d) h(y) sigma_haze at camera distance d and height y,
// sigma_haze = 1 / attenuation distance everywhere (the whole world, not only the AIR window). AIR.4F.2d.3b: each medium has its own
// near clearance w (smoothstep from its distance over its transition; 1 when off), and the haze its height profile h (HeightWeight;
// 1 when unbounded). Each medium scatters with its own albedo (mist: its colour; haze: hazeAlbedo; both times the scattering tint and strength), so
// a segment's scattered light is (sigma_mist albedo_mist L_mist + sigma_haze albedo_haze L_haze) / sigma x (1 - exp(-sigma ds)).
//   - L_mist is the march's light at the sample, with the mist's per-piece sun visibility;
//   - L_haze is the same light with the sun unshadowed: geometry does not shadow the haze (there is no visibility outside the mist's
//     bricks, nor beyond the window);
//   - the multiple-scattering isotropy follows the total optical depth crossed (haze included).
// Inside the AIR window the ray is marched at the mist's steps. Where nothing but haze lies (a clear brick, or anywhere without mist marched,
// or beyond the window) the haze is integrated exactly: for constant albedo and light, any extinction profile scatters
// albedo L (1 - exp(-tau)) with tau its optical depth (HazeDepth: the clearance's closed form, NearIntegral, and the height bounds
// piecewise). Light is
// constant along such an interval except for local Lights, so long intervals are split into HazeSubintervals, each lit at its middle.
// Rays without geometry end at the camera's far plane (the sky lies there).

static const uint HazeSubintervals = 8;

// 1 - exp(-x), accurate for tiny x (where 1 - exp(-x) loses its digits).
float OneMinusExp(float x)
{
    return x < 1e-4 ? x * (1.0 - 0.5 * x) : 1.0 - exp(-x);
}

// AIR.4F.2d.3e: whether a brick's cells can be read by a raymarch sample's footprint, as CSSunVisibility decides it: the brick or one of
// its 26 neighbours is occupied.
bool BrickNeeded(uint3 group)
{
    const int3 counts = int3(BrickCounts());
    ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
    bool needed = false;

    for(int neighbour = 0; neighbour < 27; ++neighbour)
    {
        const int3 brick = (int3(group) + int3(neighbour % 3, (neighbour / 3) % 3, neighbour / 9) - 1 + counts) % counts;
        needed = needed || occupancy.Load((brick.x + counts.x * (brick.y + counts.y * brick.z)) * 4) != 0;
    }

    return needed;
}

// AIR.4F.2d.3e: the six gather directions: plus and minus the axes of a fixed orthonormal basis, rotated off the grid's axes (31, 23 and
// 17 degrees about x, y and z) so no ray runs along a row of cells or an axis-aligned wall. The same for every cell: the fill is
// world-locked.
static const uint IndirectDirectionCount = 6;
static const float3 IndirectAxes[3] = {float3(0.880283169, -0.058163349, 0.470870010), float3(0.269129573, 0.878550460, -0.394611660),
    float3(-0.390731128, 0.474095048, 0.789026661)};
// A gather ray's longest march, in cell-sized steps.
static const uint IndirectMaximumSteps = 32;

// AIR.4F.2d.3e: the scattering medium at a point (cell units from the window's minimum), interpreted as the raymarch interprets the mist:
// extinction sigma = density x extinction, albedo the mean of colour / density x tint x scattering, and the sun's coverage V. Taken
// from the point's own fluid piece without blending across pieces; a solid sub-cell holds no medium. Beyond the window `inside` is
// false: nothing is represented there.
struct IndirectMedium
{
    bool inside;
    float sigma;
    float albedo;
    float visibility;
};

IndirectMedium IndirectMediumAt(float3 position, RWByteAddressBuffer visibility)
{
    IndirectMedium medium;
    medium.inside = false;
    medium.sigma = 0.0;
    medium.albedo = 0.0;
    medium.visibility = 0.0;
    const int3 cell = field.minimum + int3(floor(position));

    if(!AirContains(field, cell))
    {
        return medium;
    }

    medium.inside = true;
    const uint3 slot = AirSlotOf(field, cell);
    const uint index = AirRenderSlotIndex(field, slot);
    const AirCell holder = AirRenderLoadCell(Path(), slot);
    const int3 subcell = clamp(int3(floor(position * 4.0)) - (cell - field.minimum) * 4, 0, 3);
    const uint piece = AirPieceOfSubcell(holder, subcell);

    if(piece == AirNoPiece)
    {
        return medium;
    }

    AirPathBuffer mistValues = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mist0Index : mist1Index)];
    const float4 mist = asfloat(mistValues.Load4(index * 16));

    // Clear or invalid mist scatters nothing (and is never divided by).
    if(!(mist.a > 0.0) || !isfinite(mist.a) || any(!isfinite(mist.rgb)))
    {
        return medium;
    }

    medium.sigma = mist.a * extinction;
    medium.albedo = saturate(dot(saturate(mist.rgb / mist.a) * scatteringTint * scattering, float3(1.0, 1.0, 1.0) / 3.0));
    medium.visibility = AirUnpackSunVisibility(visibility.Load(index * 4), piece);
    return medium;
}

// AIR.4F.2d.3e: the sunlight that mist along one gather ray scatters back toward its start, in units of the sun's isotropic
// in-scattering E / (4 pi): the sum over steps of T x (1 - exp(-sigma ds)) x albedo x V, with T the transmittance from the start (the
// exact integral over a step of constant medium). The ray ends at the first scene hit (the TLAS, with the sun visibility's
// conventions), at `indirectReach`, or where it leaves the window. With albedo <= 1 the sum is at most 1 - T at its end: never more
// than the share of the light the mist along the ray intercepts.
float GatherIndirect(float3 origin, float3 worldOrigin, float3 direction, RWByteAddressBuffer visibility)
{
    RayDesc ray;
    ray.Origin = worldOrigin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = indirectReach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneTopLevel, RAY_FLAG_NONE, 0xFF, ray);

    while(query.Proceed())
    {
    }

    const float reach = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : indirectReach;
    const float step = field.cellSize;
    const uint steps = min(uint(ceil(reach / step)), IndirectMaximumSteps);
    float transmittance = 1.0;
    float gathered = 0.0;

    for(uint index = 0; index < steps; ++index)
    {
        const float from = float(index) * step;
        const float to = min(from + step, reach);
        const IndirectMedium medium = IndirectMediumAt(origin + direction * (0.5 * (from + to) / field.cellSize), visibility);

        if(!medium.inside)
        {
            break;
        }

        if(medium.sigma > 0.0)
        {
            const float absorbed = OneMinusExp(medium.sigma * (to - from));
            gathered += transmittance * absorbed * medium.albedo * medium.visibility;
            transmittance *= 1.0 - absorbed;

            if(transmittance < 1e-3)
            {
                break;
            }
        }
    }

    return gathered;
}

// AIR.4F.2d.3e: whether a cell's fill can be seen: one of its pieces, or a cell around it (piece 0's coverage), is not fully sunlit. A
// sample blends its corners' fill as it blends their visibility, so a sunlit cell needs a fill where a corner beside it is shadowed;
// anywhere else the fill is weighted by 1 - V = 0.
bool IndirectNeeded(int3 cell, uint ownWord, AirCell pieces, RWByteAddressBuffer visibility)
{
    if((pieces.piece0 != 0 && (ownWord & 0xFFu) != 0xFFu) || (pieces.piece1 != 0 && ((ownWord >> 8) & 0xFFu) != 0xFFu))
    {
        return true;
    }

    for(int neighbour = 0; neighbour < 27; ++neighbour)
    {
        const int3 other = cell + int3(neighbour % 3, (neighbour / 3) % 3, neighbour / 9) - 1;

        if(neighbour != 13 && AirContains(field, other) &&
            (visibility.Load(AirRenderSlotIndex(field, AirSlotOf(field, other)) * 4) & 0xFFu) != 0xFFu)
        {
            return true;
        }
    }

    return false;
}

// AIR.4F.2d.3e: the indirect fill I of every fluid piece in a brick a raymarch footprint can read (BrickNeeded), after CSSunVisibility:
// the mean over the IndirectDirectionCount world-locked rays from the piece's anchor of GatherIndirect, at most 1. Stored square-root
// encoded (AirUnpackIndirect) in the cell's own word, bits 16 to 23 piece 0 and 24 to 31 piece 1. CSSunVisibility leaves those bits 0
// and this pass only ORs into them, so the direct coverage in bits 0 to 15, which other threads read as their sources, is never written.
[numthreads(8, 8, 8)]
void CSIndirectFill(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if(!BrickNeeded(group))
    {
        return;
    }

    const uint3 slot = group * BrickCells + thread;
    const int3 cell = AirCellOfSlot(field, slot);
    const AirCell pieces = AirLoadCell(Path(), cell);
    RWByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    const uint address = AirRenderSlotIndex(field, slot) * 4;
    const uint ownWord = visibility.Load(address);

    if(!IndirectNeeded(cell, ownWord, pieces, visibility))
    {
        return;
    }

    uint bits = 0;

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;

        if(mask == 0)
        {
            continue;
        }

        const float3 anchor = AirPieceAnchor(mask);
        const float3 origin = float3(cell - field.minimum) + anchor;
        const float3 worldOrigin = (float3(cell) + anchor) * field.cellSize;
        float fill = 0.0;

        for(uint direction = 0; direction < IndirectDirectionCount; ++direction)
        {
            const float3 axis = IndirectAxes[direction / 2];
            fill += GatherIndirect(origin, worldOrigin, (direction & 1u) != 0 ? -axis : axis, visibility);
        }

        fill = saturate(fill / float(IndirectDirectionCount));
        bits |= uint(sqrt(fill) * 255.0 + 0.5) << (16u + 8u * piece);
    }

    if(bits != 0)
    {
        visibility.InterlockedOr(address, bits);
    }
}

// A near clearance's weight at camera distance d (distance 0: off).
float NearWeight(float d, float distance, float transition)
{
    return distance > 0.0 ? smoothstep(distance, distance + transition, d) : 1.0;
}

// The integral of NearWeight from the camera to d: 0 before the distance, then W (x^3 - x^4 / 2) with x the fraction of the transition
// W crossed (the smoothstep's integral), then W / 2 plus the distance beyond the transition.
float NearIntegral(float d, float distance, float transition)
{
    if(distance <= 0.0)
    {
        return d;
    }

    const float x = (d - distance) / transition;

    if(x <= 0.0)
    {
        return 0.0;
    }

    if(x < 1.0)
    {
        return transition * (x * x * x - 0.5 * x * x * x * x);
    }

    return transition * 0.5 + (d - distance - transition);
}

// AIR.4F.2d.3b: the haze's height profile h(y), y relative to the camera like the heights: 1 from baseHeight to maxHeight; below,
// smoothstep from baseHeight - heightTransition up
// to baseHeight; above, 1 - smoothstep from maxHeight to maxHeight + heightTransition; 0 beyond (a transition of 0 is a hard edge). The
// fades lie outside the layer, so a layer thinner than its transitions keeps its full density inside.
float HeightWeight(float y)
{
    if(heightEnabled == 0)
    {
        return 1.0;
    }

    if(y < baseHeight)
    {
        return heightTransition > 0.0 ? smoothstep(baseHeight - heightTransition, baseHeight, y) : 0.0;
    }

    if(y > maxHeight)
    {
        return heightTransition > 0.0 ? 1.0 - smoothstep(maxHeight, maxHeight + heightTransition, y) : 0.0;
    }

    return 1.0;
}

// The haze's weight (its clearance times its height profile) at camera distance d along the ray.
float HazeWeight(float d, float3 rayDirection)
{
    return NearWeight(d, hazeNearDistance, hazeNearTransition) * HeightWeight(d * rayDirection.y);
}

// The clearance's integral over [from, to) (the closed form NearIntegral), for the haze.
float HazeNearSpan(float from, float to)
{
    return NearIntegral(to, hazeNearDistance, hazeNearTransition) - NearIntegral(from, hazeNearDistance, hazeNearTransition);
}

// The integral of HazeWeight over [from, to) inside one height fade, where the weight is a cubic in d (the fade) times the clearance: cut
// at the clearance's two distances, each part is a polynomial of degree 6 at most, which 4-point Gauss-Legendre integrates exactly
// (degree 7). Unrolled over constant nodes: no indexable arrays, which would cost the march its registers.
float HazeFadeSpan(float from, float to, float3 rayDirection)
{
    float integral = 0.0;
    float start = from;

    [unroll]
    for(uint part = 0; part < 3; ++part)
    {
        // The parts end at the clearance's start, its end, and the interval's end (an off clearance never cuts).
        float end = to;

        if(part < 2 && hazeNearDistance > 0.0)
        {
            end = clamp(part == 0 ? hazeNearDistance : hazeNearDistance + hazeNearTransition, start, to);
        }

        if(end > start)
        {
            const float middle = 0.5 * (start + end);
            const float halfLength = 0.5 * (end - start);
            integral += halfLength * (0.3478548451 * (HazeWeight(middle - 0.8611363116 * halfLength, rayDirection) +
                HazeWeight(middle + 0.8611363116 * halfLength, rayDirection)) + 0.6521451549 * (HazeWeight(middle - 0.3399810436 * halfLength,
                rayDirection) + HazeWeight(middle + 0.3399810436 * halfLength, rayDirection)));
        }

        start = max(start, end);
    }

    return integral;
}

// AIR.4F.2d.3b: the haze's optical depth over [from, to) along the ray: hazeExtinction x the integral of HazeWeight. Unbounded, the
// clearance's closed form, as before. Bounded, a ray that is not level meets the profile's regions in one order whichever way it points:
// outside, a fade, the layer, a fade, outside (the four heights' crossing distances, reversed for a downward ray). Outside is 0; inside
// the layer the weight is the clearance alone (the closed form); in a fade, HazeFadeSpan (exact). A level ray stays at one height, so its
// weight there is a constant times the clearance. No step size: long rays stay exact and cheap. `bounded` is a compile-time constant
// (CSRaymarchAtmosphereBounded passes true): without it no height code is compiled into the ordinary kernel.
float HazeDepth(float from, float to, float3 rayDirection, bool bounded)
{
    if(!bounded)
    {
        return hazeExtinction * HazeNearSpan(from, to);
    }

    if(abs(rayDirection.y) <= 1e-7)
    {
        return hazeExtinction * HeightWeight(0.0) * HazeNearSpan(from, to);
    }

    float4 crossings = float4(baseHeight - heightTransition, baseHeight, maxHeight, maxHeight + heightTransition) / rayDirection.y;
    crossings = rayDirection.y > 0.0 ? crossings : crossings.wzyx;
    float integral = 0.0;
    const float2 firstFade = float2(max(from, crossings.x), min(to, crossings.y));
    const float2 layer = float2(max(from, crossings.y), min(to, crossings.z));
    const float2 secondFade = float2(max(from, crossings.z), min(to, crossings.w));

    if(firstFade.y > firstFade.x)
    {
        integral += HazeFadeSpan(firstFade.x, firstFade.y, rayDirection);
    }

    if(layer.y > layer.x)
    {
        integral += HazeNearSpan(layer.x, layer.y);
    }

    if(secondFade.y > secondFade.x)
    {
        integral += HazeFadeSpan(secondFade.x, secondFade.y, rayDirection);
    }

    return hazeExtinction * integral;
}

// The light the haze scatters toward the camera at a point, per unit scattering coefficient: the march's light with the sun unshadowed.
float3 HazeLight(float3 world, float3 rayDirection, float3 ambientRadiance, float isotropy)
{
    return lightingEnabled != 0 ? InScattered(world, rayDirection, 1.0, ambientRadiance, isotropy, 0.0) : float3(1.0, 1.0, 1.0);
}

// The haze alone over [from, to): exact for its extinction profile, lit at each subinterval's middle.
void AddHazeInterval(float from, float to, uint subintervals, float3 rayDirection, float3 ambientRadiance, bool bounded, inout float3 color,
    inout float transmittance, inout float opticalDepth)
{
    if(hazeEnabled == 0 || !(to > from))
    {
        return;
    }

    const float length = (to - from) / float(subintervals);
    const float3 albedo = lightingEnabled != 0 ? hazeAlbedo * scatteringTint * scattering : hazeAlbedo;
    const float meanAlbedo = dot(albedo, float3(1.0, 1.0, 1.0) / 3.0);

    for(uint index = 0; index < subintervals; ++index)
    {
        const float start = from + float(index) * length;
        const float end = index + 1 == subintervals ? to : start + length;
        const float depth = HazeDepth(start, end, rayDirection, bounded);

        if(depth <= 0.0)
        {
            continue;
        }

        const float isotropy = ScatteringIsotropy(opticalDepth, meanAlbedo);
        const float3 world = cameraPosition + rayDirection * (0.5 * (start + end));
        color += transmittance * OneMinusExp(depth) * albedo * HazeLight(world, rayDirection, ambientRadiance, isotropy);
        transmittance *= exp(-depth);
        opticalDepth += depth;
    }
}

// The simulated density at a march sample, with the detail as the march above applies it.
float DetailedDensity(AirRenderSample sample, float density, float3 world)
{
    if(detailEnabled == 0)
    {
        return density;
    }

    Texture3D<float> noise = ResourceDescriptorHeap[NonUniformResourceIndex(noiseIndex)];
    float valueA;
    float valueB;
    const bool densityDetail = densityDetailStrength > 0.0;
    const float thickness = densityDetail ? AirMistDensityDetailThickness(density, extinction) : 0.0;
    AirMistDetail worley = detail;

    if(densityDetail)
    {
        worley.strength = AirMistDensityDetailStrength(detail.strength, thickness, densityDetailStrength);
    }

    if(curlEnabled != 0)
    {
        Texture3D<float4> curl = ResourceDescriptorHeap[NonUniformResourceIndex(curlIndex)];
        density *= AirMistCurledDetailFactor(noise, curl, noiseSampler, worley, curlField, world, sample.velocity, valueA, valueB);
    }
    else
    {
        density *= AirMistDetailFactor(noise, noiseSampler, worley, world, sample.velocity, valueA, valueB);
    }

    if(wispEnabled != 0)
    {
        Texture3D<float> filaments = ResourceDescriptorHeap[NonUniformResourceIndex(wispIndex)];

        if(densityDetail)
        {
            density *= AirMistDensityWispFactor(filaments, noiseSampler, detail, wisps, world, sample.velocity, thickness, densityDetailStrength);
        }
        else
        {
            density *= AirMistWispFactor(filaments, noiseSampler, detail, wisps, world, sample.velocity);
        }
    }

    return density;
}

// ================================================================================================ V1c-2: froxel indirect fill
//
// AirVolumetricV1c2Report.md. CSIndirectFill's fill I (a scalar per fluid piece per cell, 0 to 1, in units of the sun's isotropic
// in-scattering E / 4 pi; stored square-root encoded in 8 bits) resampled at each froxel's centre with exactly the raymarch's
// reconstruction (AirRenderMistAndVelocity: piece-aware trilinear over the 8 cells around the point, decoded before blending; 0 outside
// the window or where no cell has a fill). No ray is traced and nothing is accumulated over frames: it is this frame's fill. The march
// then reads it trilinearly with the same (uv, slice) convention as the visibility texture (FroxelSunVisibilityFiltered).

float FroxelIndirectFiltered(float2 uv, float distance)
{
    Texture3D<float> volume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndirectIndex)];
    const float3 coordinate = float3(uv, FroxelSliceOf(distance) / float(froxelSize.z));
    return volume.SampleLevel(froxelSampler, coordinate, 0.0);
}

// ================================================================================================ V1c-4: froxel local lights
//
// AirVolumetricV1c4Report.md. The table Lights' in-scattering (AddSceneLights) at each froxel's centre, along that froxel's own
// camera ray: per unit of scattering coefficient, before albedo, tint and density, which the march applies. The phase is evaluated for
// the froxel's ray, so the volume is outgoing light toward this frame's camera, rebuilt every frame (no history), never a view-independent
// field. Every froxel is evaluated: the haze is lit by the Lights wherever it lies, mist or not.

float3 FroxelLocalLightsFiltered(float2 uv, float distance)
{
    Texture3D<float4> volume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelLocalLightIndex)];
    const float3 coordinate = float3(uv, FroxelSliceOf(distance) / float(froxelSize.z));
    return volume.SampleLevel(froxelSampler, coordinate, 0.0).rgb;
}

[numthreads(4, 4, 4)]
void CSFroxelLocalLights(uint3 froxel : SV_DispatchThreadID)
{
    if(any(froxel >= froxelSize))
    {
        return;
    }

    const float2 uv = (float2(froxel.xy) + 0.5) / float2(froxelSize.xy);
    const float3 rayDirection = FroxelRayDirection(uv);
    const float3 world = cameraPosition + rayDirection * FroxelDistanceOf(float(froxel.z) + 0.5);
    const float3 light = AddSceneLights(world, rayDirection, float3(0.0, 0.0, 0.0));
    RWTexture3D<float4> output = ResourceDescriptorHeap[NonUniformResourceIndex(froxelLocalLightIndex)];
    output[froxel] = float4(all(isfinite(light)) ? max(light, 0.0) : float3(0.0, 0.0, 0.0), 1.0);
}

// V1c-2b: AirRenderMistAndVelocity's `indirect` at a point, without its colour, velocity and sun visibility. The fill there is 0 unless the
// blended density is positive, so the density's blend (alone, not the colour) is still needed. On the open path (every footprint corner
// a whole cell or outside the window, the common case) this repeats that function's arithmetic on the density channel alone:
// AirRenderMistOpen's piece-0 blend of `.a` (one 4-byte load per corner instead of 16), then the same fill blend. Any footprint touching
// geometry takes the full function, so the result is identical by construction there.
float FroxelIndirectFillAt(float3 position)
{
    const int3 cell = field.minimum + int3(floor(position));

    if(!AirContains(field, cell))
    {
        return 0.0;
    }

    const uint3 cellSlot = AirSlotOf(field, cell);
    const uint cellIndex = AirRenderSlotIndex(field, cellSlot);
    const AirCell holder = AirRenderLoadCell(Path(), cellSlot);
    const int3 subcell = clamp(int3(floor(position * 4.0)) - (cell - field.minimum) * 4, 0, 3);
    const uint piece = AirPieceOfSubcell(holder, subcell);

    if(piece == AirNoPiece)
    {
        return 0.0;
    }

    const float3 shifted = position - 0.5;
    const int3 base = int3(floor(shifted));
    const float3 fraction = shifted - float3(base);
    const AirRenderFootprint footprint = AirRenderFootprintOf(field, base);

    if(!AirRenderCornersOpen(Path(), footprint))
    {
        return AirRenderMistAndVelocity(Path(), Mist(), position, false, visibilityIndex).indirect;
    }

    // AirRenderMistOpen's density: piece 0's values (every open corner inside the window is a whole cell), 0 outside the window, the
    // point's own piece's density when no corner weighs anything.
    AirPathBuffer densities = ResourceDescriptorHeap[NonUniformResourceIndex(mist0Index)];
    float densitySum = 0.0;
    float densityTotal = 0.0;

    [unroll]
    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;

        if(weight > 0.0)
        {
            const float value = footprint.inside[corner] ? asfloat(densities.Load(AirRenderSlotIndex(field, footprint.slot[corner]) * 16 + 12)) :
                0.0;
            densitySum += value * weight;
            densityTotal += weight;
        }
    }

    AirPathBuffer ownMist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mist0Index : mist1Index)];
    const float density = densityTotal > 0.0 ? densitySum / densityTotal : asfloat(ownMist.Load(cellIndex * 16 + 12));

    if(!(density > 0.0))
    {
        return 0.0;
    }

    // The fill's blend exactly as AirRenderMistAndVelocity's open path computes it.
    ByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    const float ownIndirect = AirUnpackIndirect(visibility.Load(cellIndex * 4), piece);
    float indirectSum = 0.0;
    float indirectTotal = 0.0;

    [unroll]
    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;

        if(weight > 0.0)
        {
            const float cornerIndirect = footprint.inside[corner] ?
                AirUnpackIndirect(visibility.Load(AirRenderSlotIndex(field, footprint.slot[corner]) * 4), 0) : ownIndirect;
            indirectSum += cornerIndirect * weight;
            indirectTotal += weight;
        }
    }

    return indirectTotal > 0.0 ? indirectSum / indirectTotal : ownIndirect;
}

// One thread per froxel, after CSIndirectFill (its fill and the visibility word are read through the visibility SRV). A froxel whose
// centre has no fluid piece (inside a solid) or lies outside the window gets the fill AirRenderMistAndVelocity returns there: 0.
// V1c-2b: with froxelIndirectSkipEmpty, a froxel whose centre's brick holds no mist writes 0 without reconstructing: CSOccupancy flags a
// brick when any cell within one cell of it holds mist, and every footprint corner lies within one cell of the centre's cell, so an
// unflagged brick means every corner's density is 0, the blended density is 0, and the reconstruction returns exactly 0.
[numthreads(4, 4, 4)]
void CSFroxelIndirect(uint3 froxel : SV_DispatchThreadID)
{
    if(any(froxel >= froxelSize))
    {
        return;
    }

    const float2 uv = (float2(froxel.xy) + 0.5) / float2(froxelSize.xy);
    const float3 world = cameraPosition + FroxelRayDirection(uv) * FroxelDistanceOf(float(froxel.z) + 0.5);
    // The window-relative cell coordinate the raymarch passes for a world point.
    const float3 position = world * field.inverseCellSize - float3(field.minimum);
    float fill = 0.0;
    bool empty = false;

    if(froxelIndirectSkipEmpty != 0)
    {
        const int3 cell = field.minimum + int3(floor(position));
        ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
        empty = !AirContains(field, cell) || occupancy.Load(BrickOf(cell) * 4) == 0;
    }

    if(!empty)
    {
        fill = froxelIndirectFillOnly != 0 ? FroxelIndirectFillAt(position) :
            AirRenderMistAndVelocity(Path(), Mist(), position, false, visibilityIndex).indirect;
    }

    const float indirect = isfinite(fill) ? saturate(fill) : 0.0;
    RWTexture3D<unorm float> output = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndirectIndex)];
    output[froxel] = indirect;
}

// The froxel volume's sun visibility at a march sample, from the buffer or the texture as `sunSource` names (never SunFromCells: the
// marches keep the per-piece visibility's expression unchanged, so the production kernels compile exactly as before).
float FroxelSunVisibilityFrom(uint sunSource, float2 uv, float distance)
{
    if(sunSource == SunFromFroxelTexture || sunSource == SunFromFroxelLit || sunSource == SunFromFroxelLitLocal)
    {
        return FroxelSunVisibilityFiltered(uv, distance);
    }

    return FroxelSunVisibility(uv, distance);
}

// The whole ray through mist and haze (see the comment above): (premultiplied colour, transmittance). V1a: `sunSource` (a literal at
// each kernel, so the production kernels compile without the froxel code) picks the mist's sun visibility (FroxelSunVisibilityFrom).
float4 MarchAtmosphere(uint2 pixel, float2 clip, float3 direction, float sceneDistance, bool bounded, uint sunSource)
{
    // The sky lies at the camera's far plane along this ray.
    const float rayEnd = sceneDistance < NoGeometry ? sceneDistance : length(WorldAt(clip, 1.0) - cameraPosition);
    float3 color = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float opticalDepth = 0.0;
    // The mist's ambient light (its Ambient setting) and the haze's own (AIR.4F.2d.3a: hazeAmbient), from one environment estimate.
    const bool environmentNeeded = lightingEnabled != 0 && (ambient > 0.0 || (hazeEnabled != 0 && hazeAmbient > 0.0));
    const float3 environment = environmentNeeded ? EnvironmentRadiance() : float3(0.0, 0.0, 0.0);
    const float3 ambientRadiance = ambient > 0.0 ? ambient * environment : float3(0.0, 0.0, 0.0);
    const float3 hazeAmbientRadiance = hazeEnabled != 0 && hazeAmbient > 0.0 ? hazeAmbient * environment : float3(0.0, 0.0, 0.0);
    // Where the haze-only tail starts: the camera, or where the mist march stopped.
    float tailStart = 0.0;

    if(mistActive != 0)
    {
        const float3 low = float3(field.minimum) * field.cellSize;
        const float3 high = float3(field.minimum + int3(field.dimensions)) * field.cellSize;
        float2 span = BoxSpan(cameraPosition, direction, low, high);
        span.y = min(span.y, rayEnd);

        if(span.x < span.y)
        {
            // Before the window (only when the camera is outside it): haze alone.
            AddHazeInterval(0.0, span.x, 1, direction, hazeAmbientRadiance, bounded, color, transmittance, opticalDepth);
            tailStart = span.x;
            const float step = max(minimumStep, (span.y - span.x) / float(maxSteps));
            const float offset = jitterEnabled != 0 ? PixelOffset(pixel) : 0.0;
            const float3 origin = cameraPosition * field.inverseCellSize - float3(field.minimum);
            const float3 rayCells = direction * field.inverseCellSize;
            ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
            const float3 hazeScattering = lightingEnabled != 0 ? hazeAlbedo * scatteringTint * scattering : hazeAlbedo;
            uint index = 0;

            // Segments as the march above: k covers [span.x + k step, span.x + (k + 1) step), the last cut off at span.y.
            while(transmittance > opaqueTransmittance)
            {
                const float start = span.x + float(index) * step;

                if(start >= span.y)
                {
                    tailStart = span.y;
                    break;
                }

                const float segment = min(step, span.y - start);
                const float t = start + offset * segment;
                const float3 position = origin + rayCells * t;
                const int3 cell = field.minimum + int3(floor(position));

                if(!AirContains(field, cell))
                {
                    tailStart = start;
                    break;
                }

                // A clear brick: its haze, exactly, up to the first segment beyond it.
                if(occupancy.Load(BrickOf(cell) * 4) == 0)
                {
                    const int3 brickMask = int3(~(BrickCells - 1), ~(BrickCells - 1), ~(BrickCells - 1));
                    const float3 brickLow = float3((cell - field.minimum) & brickMask);
                    const float2 brickSpan = BoxSpan(position, rayCells, brickLow, brickLow + float(BrickCells));
                    const float exitT = t + brickSpan.y;
                    index = max(index + 1, uint(ceil((exitT - span.x) / step - offset)));
                    tailStart = min(span.x + float(index) * step, span.y);
                    AddHazeInterval(start, tailStart, 1, direction, hazeAmbientRadiance, bounded, color, transmittance, opticalDepth);
                    continue;
                }

                const AirRenderSample sample = AirRenderMistAndVelocity(Path(), Mist(), position, detailEnabled != 0,
                    lightingEnabled != 0 ? visibilityIndex : AirNoVisibility);
                const float3 world = cameraPosition + direction * t;
                // AIR.4F.2d.3b: each medium with its own clearance; the haze with its height profile.
                const float mistWeight = NearWeight(t, mistNearDistance, mistNearTransition);
                const float mistDensity = sample.mist.a > 0.0 ? DetailedDensity(sample, sample.mist.a, world) : 0.0;
                const float mistSigma = mistWeight * mistDensity * extinction;
                // Bounded haze changes across a height fade within one step: it takes the segment's mean weight by Simpson's rule (the
                // fades are smooth, so a short segment's error is far below one jittered sample's; the exact HazeDepth here would cost the
                // whole march its registers). Unbounded haze only changes in its clearance and keeps its per-sample weight (as before 2d.3b).
                float hazeSigma = 0.0;

                if(hazeEnabled != 0)
                {
                    const float hazeWeight = bounded ? (HazeWeight(start, direction) + 4.0 * HazeWeight(start + 0.5 * segment, direction) +
                        HazeWeight(start + segment, direction)) / 6.0 : NearWeight(t, hazeNearDistance, hazeNearTransition);
                    hazeSigma = hazeWeight * hazeExtinction;
                }

                const float sigma = mistSigma + hazeSigma;

                if(sigma > 0.0)
                {
                    const float absorbed = OneMinusExp(sigma * segment);
                    const float3 mistAlbedo = sample.mist.a > 0.0 ? sample.mist.rgb / sample.mist.a : float3(0.0, 0.0, 0.0);
                    const float3 mistScattering = lightingEnabled != 0 ? mistAlbedo * scatteringTint * scattering : mistAlbedo;
                    // Each medium's share of the segment's extinction scatters with its own albedo and light.
                    const float mistShare = mistSigma / sigma;
                    const float hazeShare = hazeSigma / sigma;
                    float3 radiance = mistShare * mistScattering + hazeShare * hazeScattering;

                    if(lightingEnabled != 0)
                    {
                        const float meanAlbedo = dot(mistShare * mistScattering + hazeShare * hazeScattering, float3(1.0, 1.0, 1.0) / 3.0);
                        const float isotropy = ScatteringIsotropy(opticalDepth, meanAlbedo);
                        radiance = float3(0.0, 0.0, 0.0);

                        // Explicit branches: a vector conditional would evaluate both lights.
                        if(mistShare > 0.0)
                        {
                            const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
                            const float sunVisibility = sunSource != SunFromCells ? FroxelSunVisibilityFrom(sunSource, uv, t) :
                                sample.sunVisibility;
                            const float indirect = sunSource == SunFromFroxelLit || sunSource == SunFromFroxelLitLocal ?
                                FroxelIndirectFiltered(uv, t) : sample.indirect;
                            radiance += mistShare * mistScattering * (sunSource == SunFromFroxelLitLocal ?
                                InScatteredFroxelLights(world, direction, sunVisibility, ambientRadiance, isotropy, indirect, FroxelLocalLightsFiltered(uv, t)) :
                                InScattered(world, direction, sunVisibility, ambientRadiance, isotropy, indirect));
                        }

                        if(hazeShare > 0.0)
                        {
                            const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
                            radiance += hazeShare * hazeScattering * (sunSource == SunFromFroxelLitLocal ?
                                InScatteredFroxelLights(world, direction, 1.0, hazeAmbientRadiance, isotropy, 0.0, FroxelLocalLightsFiltered(uv, t)) :
                                InScattered(world, direction, 1.0, hazeAmbientRadiance, isotropy, 0.0));
                        }
                    }

                    color += transmittance * absorbed * radiance;
                    transmittance *= 1.0 - absorbed;
                    opticalDepth += sigma * segment;
                }

                ++index;
                tailStart = min(span.x + float(index) * step, span.y);
            }
        }
    }

    // Beyond the mist march (beyond the window, or after the march stopped): the haze to the geometry or the sky, exactly. After an early
    // stop the mist left on the ray is not marched; the haze still is, so it never ends abruptly.
    AddHazeInterval(tailStart, rayEnd, HazeSubintervals, direction, hazeAmbientRadiance, bounded, color, transmittance, opticalDepth);
    return float4(color, transmittance);
}

// The mist march without haze or clearance. V1a: `sunSource` as in MarchAtmosphere.
void RaymarchMist(uint3 thread, uint sunSource)
{
    if(any(thread.xy >= reducedSize))
    {
        return;
    }

    // The full-resolution pixel under this reduced pixel's centre: its depth ends the ray.
    const float2 uv = (float2(thread.xy) + 0.5) / float2(reducedSize);
    const uint2 fullPixel = min(uint2(uv * float2(fullSize)), fullSize - 1);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[NonUniformResourceIndex(depthIndex)];
    const float depth = depthTexture.Load(int3(fullPixel, 0));
    const float2 clip = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 direction = normalize(WorldAt(clip, 1.0) - cameraPosition);
    const float sceneDistance = depth < 1.0 ? length(WorldAt(clip, depth) - cameraPosition) : NoGeometry;

    const float3 low = float3(field.minimum) * field.cellSize;
    const float3 high = float3(field.minimum + int3(field.dimensions)) * field.cellSize;
    float2 span = BoxSpan(cameraPosition, direction, low, high);
    span.y = min(span.y, sceneDistance);

    float3 color = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    if(span.x < span.y)
    {
        const float step = max(minimumStep, (span.y - span.x) / float(maxSteps));
        // The sample's place within its segment, as a fraction of the segment.
        const float offset = jitterEnabled != 0 ? PixelOffset(thread.xy) : 0.0;
        const float3 origin = cameraPosition * field.inverseCellSize - float3(field.minimum);
        const float3 rayCells = direction * field.inverseCellSize;
        ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
        uint index = 0;
        // AIR.4F.2d.2: the optical depth crossed so far, for the multiple-scattering isotropy.
        float opticalDepth = 0.0;
        // AIR.4F F4.3.1: the ambient radiance, once per ray.
        float3 ambientRadiance = float3(0.0, 0.0, 0.0);

        if(lightingEnabled != 0 && ambient > 0.0)
        {
            ambientRadiance = ambient * EnvironmentRadiance();
        }

        // Segment k covers [span.x + k * step, span.x + (k + 1) * step), the last one cut off at span.y (F4.2), so the segments cover
        // the span exactly and a sample's weight is its segment's length; sample k lies offset of the way through its segment.
        // Skipping a clear brick moves to a later k and changes no sample taken.
        while(transmittance > OpaqueTransmittance)
        {
            const float start = span.x + float(index) * step;

            if(start >= span.y)
            {
                break;
            }

            const float segment = min(step, span.y - start);
            const float t = start + offset * segment;

            const float3 position = origin + rayCells * t;
            const int3 cell = field.minimum + int3(floor(position));

            if(!AirContains(field, cell))
            {
                break;
            }

            if(occupancy.Load(BrickOf(cell) * 4) == 0)
            {
                // The ray's exit from this brick (in cell units), then the first sample at or beyond it.
                const int3 brickMask = int3(~(BrickCells - 1), ~(BrickCells - 1), ~(BrickCells - 1));
                const float3 brickLow = float3((cell - field.minimum) & brickMask);
                const float2 brickSpan = BoxSpan(position, rayCells, brickLow, brickLow + float(BrickCells));
                const float exitT = t + brickSpan.y;
                index = max(index + 1, uint(ceil((exitT - span.x) / step - offset)));
                continue;
            }

            const AirRenderSample sample = AirRenderMistAndVelocity(Path(), Mist(), position, detailEnabled != 0,
                lightingEnabled != 0 ? visibilityIndex : AirNoVisibility);
            const float4 mist = sample.mist;

            if(mist.a > 0.0)
            {
                float density = mist.a;

                if(detailEnabled != 0)
                {
                    Texture3D<float> noise = ResourceDescriptorHeap[NonUniformResourceIndex(noiseIndex)];
                    float valueA;
                    float valueB;
                    // AIR.4F F4.4.2e: the detail follows the simulated mist's thickness (before any detail); off, the detail is unchanged.
                    const bool densityDetail = densityDetailStrength > 0.0;
                    const float thickness = densityDetail ? AirMistDensityDetailThickness(density, extinction) : 0.0;
                    AirMistDetail worley = detail;

                    if(densityDetail)
                    {
                        worley.strength = AirMistDensityDetailStrength(detail.strength, thickness, densityDetailStrength);
                    }

                    // AIR.4F F4.4: with curl, the same detail factor from warped Worley coordinates.
                    if(curlEnabled != 0)
                    {
                        Texture3D<float4> curl = ResourceDescriptorHeap[NonUniformResourceIndex(curlIndex)];
                        density *= AirMistCurledDetailFactor(noise, curl, noiseSampler, worley, curlField, cameraPosition + direction * t,
                            sample.velocity, valueA, valueB);
                    }
                    else
                    {
                        density *= AirMistDetailFactor(noise, noiseSampler, worley, cameraPosition + direction * t, sample.velocity, valueA,
                            valueB);
                    }

                    // AIR.4F F4.4.2: wisps multiply the detail factor.
                    if(wispEnabled != 0)
                    {
                        Texture3D<float> filaments = ResourceDescriptorHeap[NonUniformResourceIndex(wispIndex)];

                        if(densityDetail)
                        {
                            density *= AirMistDensityWispFactor(filaments, noiseSampler, detail, wisps, cameraPosition + direction * t,
                                sample.velocity, thickness, densityDetailStrength);
                        }
                        else
                        {
                            density *= AirMistWispFactor(filaments, noiseSampler, detail, wisps, cameraPosition + direction * t,
                                sample.velocity);
                        }
                    }
                }

                const float absorbed = 1.0 - exp(-density * extinction * segment);
                // The mist's colour: a convex blend of the pieces' colours, bounded even where the density is tiny.
                const float3 albedo = mist.rgb / mist.a;

                if(lightingEnabled != 0)
                {
                    // AIR.4F.2d.2: the tint multiplies the scattering albedo (never above it); the isotropy follows the depth crossed.
                    const float3 world = cameraPosition + direction * t;
                    const float3 tintedAlbedo = albedo * scatteringTint * scattering;
                    const float isotropy = ScatteringIsotropy(opticalDepth, dot(tintedAlbedo, float3(1.0, 1.0, 1.0) / 3.0));
                    const float sunVisibility = sunSource != SunFromCells ? FroxelSunVisibilityFrom(sunSource, uv, t) :
                        sample.sunVisibility;
                    color += transmittance * absorbed * tintedAlbedo * InScattered(world, direction, sunVisibility, ambientRadiance,
                        isotropy, sample.indirect);
                }
                else
                {
                    color += transmittance * absorbed * albedo;
                }

                transmittance *= 1.0 - absorbed;
                opticalDepth += density * extinction * segment;
            }

            ++index;
        }
    }

    const uint pixelIndex = thread.x + reducedSize.x * thread.y;
    RWByteAddressBuffer result = ResourceDescriptorHeap[NonUniformResourceIndex(resultIndex)];
    RWByteAddressBuffer distances = ResourceDescriptorHeap[NonUniformResourceIndex(distanceIndex)];
    result.Store4(pixelIndex * 16, asuint(float4(color, transmittance)));
    distances.Store(pixelIndex * 4, asuint(sceneDistance));
}

[numthreads(8, 8, 1)]
void CSRaymarch(uint3 thread : SV_DispatchThreadID)
{
    RaymarchMist(thread, SunFromCells);
}

// AIR.4F.2d.3: the combined march (MarchAtmosphere), a kernel of its own so CSRaymarch above keeps its code and its register use when
// haze and the near clearance are off. EditorMistPass picks it whenever haze or the clearance is on or the AIR mist is not marched.
// AIR.4F.2d.3b: the two atmosphere kernels differ only in the haze's height bounds (CSRaymarchAtmosphereBounded below).
void RaymarchAtmosphere(uint3 thread, bool bounded, uint sunSource)
{
    if(any(thread.xy >= reducedSize))
    {
        return;
    }

    // As CSRaymarch: the full-resolution depth under this reduced pixel's centre ends the ray.
    const float2 uv = (float2(thread.xy) + 0.5) / float2(reducedSize);
    const uint2 fullPixel = min(uint2(uv * float2(fullSize)), fullSize - 1);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[NonUniformResourceIndex(depthIndex)];
    const float depth = depthTexture.Load(int3(fullPixel, 0));
    const float2 clip = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 direction = normalize(WorldAt(clip, 1.0) - cameraPosition);
    const float sceneDistance = depth < 1.0 ? length(WorldAt(clip, depth) - cameraPosition) : NoGeometry;
    const uint pixelIndex = thread.x + reducedSize.x * thread.y;
    RWByteAddressBuffer result = ResourceDescriptorHeap[NonUniformResourceIndex(resultIndex)];
    RWByteAddressBuffer distances = ResourceDescriptorHeap[NonUniformResourceIndex(distanceIndex)];
    result.Store4(pixelIndex * 16, asuint(MarchAtmosphere(thread.xy, clip, direction, sceneDistance, bounded, sunSource)));
    distances.Store(pixelIndex * 4, asuint(sceneDistance));
}

[numthreads(8, 8, 1)]
void CSRaymarchAtmosphere(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, false, SunFromCells);
}

// AIR.4F.2d.3b: with the haze's height bounds. A kernel of its own, as CSRaymarchAtmosphere is beside CSRaymarch: the bounded haze's
// integration inside the march would otherwise cost the ordinary atmosphere kernel its registers (measured: +30% per step in dense fog).
[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereBounded(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, true, SunFromCells);
}

// V1a diagnostics: the three marches with the froxel volume's sun visibility (EditorMistPass picks them only on request).
[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereFroxel(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, false, SunFromFroxelBuffer);
}

[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereBoundedFroxel(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, true, SunFromFroxelBuffer);
}

[numthreads(8, 8, 1)]
void CSRaymarchFroxel(uint3 thread : SV_DispatchThreadID)
{
    RaymarchMist(thread, SunFromFroxelBuffer);
}

// V1b-1 diagnostics: the three marches reading the froxel texture with hardware filtering (V1a's volume copied into the texture, or the
// temporally resolved volume).
[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereFroxelFiltered(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, false, SunFromFroxelTexture);
}

[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereBoundedFroxelFiltered(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, true, SunFromFroxelTexture);
}

[numthreads(8, 8, 1)]
void CSRaymarchFroxelFiltered(uint3 thread : SV_DispatchThreadID)
{
    RaymarchMist(thread, SunFromFroxelTexture);
}

// V1c-2 diagnostics: the atmosphere marches reading both lighting terms from froxels (the visibility texture and the froxel indirect
// fill). There is no RaymarchMist variant: any change at its sample site reorders the production CSRaymarch's DXIL, so EditorMistPass
// runs the lit froxel path through MarchAtmosphere, which draws the mist alone exactly as well when haze and clearance are off.
[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereFroxelLit(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, false, SunFromFroxelLit);
}

[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereBoundedFroxelLit(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, true, SunFromFroxelLit);
}

// V1c-4 diagnostics: the lit atmosphere marches with the table Lights from the froxel local-light volume (at the mist and haze samples; the
// haze's analytic intervals between samples, HazeLight, keep the per-point loop).
[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereFroxelLitLocal(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, false, SunFromFroxelLitLocal);
}

[numthreads(8, 8, 1)]
void CSRaymarchAtmosphereBoundedFroxelLitLocal(uint3 thread : SV_DispatchThreadID)
{
    RaymarchAtmosphere(thread, true, SunFromFroxelLitLocal);
}

// ================================================================================================ V1d: integrated froxel rendering
//
// AirVolumetricV1dReport.md. The mist and the haze inside the froxel range are integrated once per froxel column instead of marched per
// pixel, then each reduced pixel looks the result up at its scene distance (CSRaymarchIntegrated), writing what the march writes, so the
// composite is unchanged. Three passes:
//   CSFroxelMedium      the AIR sample at each froxel centre (MarchAtmosphere's per-sample reconstruction with detail): the mist's
//                       extinction and scattering coefficient, and the indirect fill (V1c-2's resample, consolidated into this read);
//   CSFroxelIntegrate   front to back along each froxel column: per slice the source S = sigma_mist albedo_mist L_mist + sigma_haze
//                       albedo_haze L_haze (MarchAtmosphere's), lit by the resolved sun visibility, the fill, the ambient and the table
//                       Lights' far field, with the sun's isotropy from the optical depth at the slice's start; L += T S (1 - e^-sigma ds)
//                       / sigma, T *= e^-sigma ds; the mist stops counting once T falls to the termination transmittance (the haze goes on,
//                       as the march's tail does);
//   CSRaymarchIntegrated the cumulative (L, T) at the slice holding the pixel's end, the partial slice exactly (homogeneous segment), the
//                       haze beyond the froxel range (AddHazeInterval), and the Lights' near field along the pixel's own ray.

static const uint NearLightSamples = 16;

// froxelIntegrateCellVisibility: the integration's sun visibility from the froxel visibility volume (V1b), from the cells looked up at
// each of its sub-samples (the investigation's diagnostic), or from CSFroxelMedium's cell-visibility volume (the correction).
static const uint FroxelVisibilityVolume = 0;
static const uint CellVisibilityLookup = 1;
static const uint CellVisibilityVolume = 2;

// A table Light's froxel share at distance d from it: 0 within the near radius r, 1 beyond 2r, smoothstep between. The composite adds the
// complementary 1 - share along each pixel's ray, so the two parts sum to each Light's whole contribution.
float FarLightShare(float d)
{
    return froxelNearLightRadius > 0.0 ? smoothstep(froxelNearLightRadius, 2.0 * froxelNearLightRadius, d) : 1.0;
}

// The table Lights' far-field in-scattering toward `rayDirection` at a point (AddSceneLights with each Light's FarLightShare).
float3 FarSceneLights(float3 world, float3 rayDirection)
{
    float3 light = float3(0.0, 0.0, 0.0);
    const uint count = sceneLightCount;

    for(uint index = 0; index < count; ++index)
    {
        float3 directionToLight;
        const float3 incidence = EvaluateSceneLightIncidence(SceneLightLoad(sceneLightBufferIndex, index), world, directionToLight);

        if(any(incidence > 0.0))
        {
            const float share = SceneLightLoad(sceneLightBufferIndex, index).type == SCENE_LIGHT_DIRECTIONAL ? 1.0 : FarLightShare(length(SceneLightLoad(sceneLightBufferIndex, index).position - world));
            light += share * incidence * HenyeyGreenstein(dot(directionToLight, rayDirection), anisotropy);
        }
    }

    return light;
}

// The distance of a slice's near face (0 for the first slice: it starts at the camera).
float FroxelSliceStart(uint slice)
{
    return slice == 0 ? 0.0 : FroxelDistanceOf(float(slice));
}

[numthreads(4, 4, 4)]
void CSFroxelMedium(uint3 froxel : SV_DispatchThreadID)
{
    if(any(froxel >= froxelSize))
    {
        return;
    }

    // V1d correction (AirVolumetricV1dAliasingCorrection.md): froxelMediumSamples AIR samples at that many depths within the slice (1: its
    // centre), each giving its own density, visibility and fill, as the march's samples do. The medium is their length-weighted mean;
    // the visibility and the fill their means weighted by each sample's extinction too, so the slice's in-scattering is the samples'
    // sum. A trilinear medium with point visibility is not consistent: where a sample holds no mist (a solid or empty cell) the AIR
    // sample's visibility is its default 1 while the interpolated medium is not 0, and light leaks.
    const float2 uv = (float2(froxel.xy) + 0.5) / float2(froxelSize.xy);
    const float3 direction = FroxelRayDirection(uv);
    const float sliceStart = FroxelSliceStart(froxel.z);
    const float sliceLength = FroxelDistanceOf(float(froxel.z) + 1.0) - sliceStart;
    // Distant slices are several cells deep: a fixed count skips cells there, and a mist or shadow boundary crossing the gap between two
    // samples moves the whole slice's value at once (bands). Adaptive, every cell along the slice gets a sample.
    const uint fixedSamples = clamp(froxelMediumSamples, 1u, 8u);
    const uint samples = froxelMediumAdaptive != 0 ? clamp(uint(ceil(sliceLength * field.inverseCellSize)), fixedSamples, 16u) : fixedSamples;
    // V1d lateral experiment: with froxelMediumLateral, every depth sample is taken on 4 distinct camera rays through a rotated 2 x 2 grid
    // of the froxel's XY footprint (offsets in froxel widths), each at the same camera distance, and all are averaged into the froxel's
    // medium like the depth samples (a footprint-averaged medium, integrated once per column).
    const uint laterals = froxelMediumLateral != 0 ? 4u : 1u;
    const bool single = samples == 1 && laterals == 1;
    ByteAddressBuffer occupancy = ResourceDescriptorHeap[NonUniformResourceIndex(occupancyIndex)];
    float4 medium = float4(0.0, 0.0, 0.0, 0.0);
    float weightedVisibility = 0.0;
    float weightedIndirect = 0.0;
    float centreIndirect = 0.0;

    for(uint sampleIndex = 0; sampleIndex < samples * laterals; ++sampleIndex)
    {
        const uint index = sampleIndex / laterals;
        const uint lateral = sampleIndex % laterals;
        const float2 lateralOffset = laterals == 1 ? float2(0.0, 0.0) : float2(lateral == 0 ? -0.375 : (lateral == 1 ? 0.125 :
            (lateral == 2 ? 0.375 : -0.125)), lateral == 0 ? -0.125 : (lateral == 1 ? -0.375 : (lateral == 2 ? 0.125 : 0.375)));
        const float3 sampleDirection = laterals == 1 ? direction : FroxelRayDirection(uv + lateralOffset / float2(froxelSize.xy));
        const float position = samples == 1 ? 0.5 : (float(index) + 0.5) / float(samples);
        const float distance = FroxelDistanceOf(float(froxel.z) + position);
        const float start = index == 0 ? sliceStart : FroxelDistanceOf(float(froxel.z) + float(index) / float(samples));
        const float end = FroxelDistanceOf(float(froxel.z) + float(index + 1) / float(samples));
        const float weight = (samples == 1 ? 1.0 : (end - start) / sliceLength) / float(laterals);
        const float3 world = cameraPosition + sampleDirection * distance;
        const float3 fieldPosition = world * field.inverseCellSize - float3(field.minimum);
        const int3 cell = field.minimum + int3(floor(fieldPosition));

        // As the march: no mist outside the window or in a clear brick (CSOccupancy reaches a footprint's every corner).
        if(AirContains(field, cell) && occupancy.Load(BrickOf(cell) * 4) != 0)
        {
            const AirRenderSample sample = AirRenderMistAndVelocity(Path(), Mist(), fieldPosition, detailEnabled != 0, visibilityIndex);
            const float mistWeight = NearWeight(distance, mistNearDistance, mistNearTransition);
            const float mistDensity = sample.mist.a > 0.0 ? DetailedDensity(sample, sample.mist.a, world) : 0.0;
            const float mistSigma = mistWeight * mistDensity * extinction;
            const float3 mistAlbedo = sample.mist.a > 0.0 ? sample.mist.rgb / sample.mist.a : float3(0.0, 0.0, 0.0);
            const float3 mistScattering = mistAlbedo * scatteringTint * scattering;
            medium += weight * float4(mistSigma * mistScattering, mistSigma);
            weightedVisibility += weight * mistSigma * sample.sunVisibility;
            weightedIndirect += weight * mistSigma * sample.indirect;
            centreIndirect = single ? sample.indirect : centreIndirect;
        }
    }

    // One sample keeps V1d's fill (the centre's, whatever its extinction); its visibility is the sample's own.
    const float indirect = single ? centreIndirect : (medium.a > 0.0 ? weightedIndirect / medium.a : 0.0);
    const float visibility = medium.a > 0.0 ? weightedVisibility / medium.a : 1.0;
    const bool finite = all(isfinite(medium)) && isfinite(indirect) && isfinite(visibility);
    RWTexture3D<float4> mediumOutput = ResourceDescriptorHeap[NonUniformResourceIndex(froxelMediumIndex)];
    mediumOutput[froxel] = finite ? max(medium, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
    RWTexture3D<unorm float> indirectOutput = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndirectIndex)];
    indirectOutput[froxel] = finite ? saturate(indirect) : 0.0;

    if(froxelIntegrateCellVisibility == CellVisibilityVolume)
    {
        RWTexture3D<unorm float> visibilityOutput = ResourceDescriptorHeap[NonUniformResourceIndex(froxelCellVisibilityIndex)];
        visibilityOutput[froxel] = finite ? saturate(visibility) : 1.0;
    }
}

// One thread per froxel column. `bounded`: the haze's height bounds (Simpson over each slice, as the march over each segment).
void FroxelIntegrate(uint2 column, bool bounded)
{
    if(any(column >= froxelSize.xy))
    {
        return;
    }

    Texture3D<float4> mediumVolume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelMediumIndex)];
    Texture3D<float> visibilityVolume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelTextureIndex)];
    Texture3D<float> indirectVolume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIndirectIndex)];
    Texture3D<float> cellVisibilityVolume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelCellVisibilityIndex)];
    RWTexture3D<float4> integrated = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIntegratedIndex)];
    RWTexture3D<float4> segments = ResourceDescriptorHeap[NonUniformResourceIndex(froxelSegmentIndex)];
    const float2 uv = (float2(column) + 0.5) / float2(froxelSize.xy);
    const float3 direction = FroxelRayDirection(uv);
    const bool environmentNeeded = ambient > 0.0 || (hazeEnabled != 0 && hazeAmbient > 0.0);
    const float3 environment = environmentNeeded ? EnvironmentRadiance() : float3(0.0, 0.0, 0.0);
    const float3 ambientRadiance = ambient > 0.0 ? ambient * environment : float3(0.0, 0.0, 0.0);
    const float3 hazeAmbientRadiance = hazeEnabled != 0 && hazeAmbient > 0.0 ? hazeAmbient * environment : float3(0.0, 0.0, 0.0);
    const float3 hazeScattering = hazeAlbedo * scatteringTint * scattering;
    const float sunPhase = HenyeyGreenstein(dot(-normalize(sunDirection), direction), anisotropy);
    float3 light = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float opticalDepth = 0.0;
    bool mistStopped = false;

    // Investigation: the sub-samples per slice (1: each froxel's own values; more: trilinear samples at that many depths in the slice).
    const uint parts = max(froxelIntegrateSamples, 1u);

    for(uint slice = 0; slice < froxelSize.z; ++slice)
    {
        const uint3 froxel = uint3(column, slice);
        integrated[froxel] = float4(light, transmittance);
        float3 sliceSource = float3(0.0, 0.0, 0.0);
        float sliceSigma = 0.0;

        for(uint part = 0; part < parts; ++part)
        {
            const float start = part == 0 ? FroxelSliceStart(slice) : FroxelDistanceOf(float(slice) + float(part) / float(parts));
            const float end = FroxelDistanceOf(float(slice) + float(part + 1) / float(parts));
            const float sliceLength = end - start;
            const float middle = FroxelDistanceOf(float(slice) + (float(part) + 0.5) / float(parts));
            const float3 world = cameraPosition + direction * middle;
            const float3 coordinate = float3(uv, (float(slice) + (float(part) + 0.5) / float(parts)) / float(froxelSize.z));

            // The march stops marching mist once the transmittance reaches the termination; the haze goes on (its tail).
            mistStopped = mistStopped || transmittance <= opaqueTransmittance;
            const float4 medium = mistStopped ? float4(0.0, 0.0, 0.0, 0.0) :
                (parts == 1 ? mediumVolume.Load(int4(froxel, 0)) : mediumVolume.SampleLevel(froxelSampler, coordinate, 0.0));
            float hazeSigma = 0.0;

            if(hazeEnabled != 0)
            {
                const float hazeWeight = bounded ? (HazeWeight(start, direction) + 4.0 * HazeWeight(middle, direction) +
                    HazeWeight(end, direction)) / 6.0 : NearWeight(middle, hazeNearDistance, hazeNearTransition);
                hazeSigma = hazeWeight * hazeExtinction;
            }

            const float sigma = medium.a + hazeSigma;
            float3 source = float3(0.0, 0.0, 0.0);

            if(sigma > 0.0)
            {
                const float3 mistScatteringCoefficient = medium.rgb;
                const float3 hazeScatteringCoefficient = hazeSigma * hazeScattering;
                const float meanAlbedo = dot(mistScatteringCoefficient + hazeScatteringCoefficient, float3(1.0, 1.0, 1.0) / 3.0) / sigma;
                const float isotropy = ScatteringIsotropy(opticalDepth, meanAlbedo);
                const float phase = isotropy > 0.0 ? lerp(sunPhase, 1.0 / (4.0 * 3.14159265), isotropy) : sunPhase;
                const float3 lights = FarSceneLights(world, direction);

                if(medium.a > 0.0)
                {
                    float visibility = parts == 1 ? visibilityVolume.Load(int4(froxel, 0)) :
                        visibilityVolume.SampleLevel(froxelSampler, coordinate, 0.0);

                    if(froxelIntegrateCellVisibility == CellVisibilityLookup)
                    {
                        const float3 position = world * field.inverseCellSize - float3(field.minimum);
                        visibility = AirRenderMistAndVelocity(Path(), Mist(), position, false, visibilityIndex).sunVisibility;
                    }
                    else if(froxelIntegrateCellVisibility == CellVisibilityVolume)
                    {
                        visibility = parts == 1 ? cellVisibilityVolume.Load(int4(froxel, 0)) :
                            cellVisibilityVolume.SampleLevel(froxelSampler, coordinate, 0.0);
                    }
                    const float indirect = parts == 1 ? indirectVolume.Load(int4(froxel, 0)) :
                        indirectVolume.SampleLevel(froxelSampler, coordinate, 0.0);
                    float3 mistLight = ambientRadiance + sunRadiance * phase * visibility + lights;

                    if(indirectScattering > 0.0 && indirect > 0.0)
                    {
                        mistLight += sunRadiance * (indirectScattering * (1.0 - visibility) * indirect / (4.0 * 3.14159265));
                    }

                    source += mistScatteringCoefficient * mistLight;
                }

                if(hazeSigma > 0.0)
                {
                    source += hazeScatteringCoefficient * (hazeAmbientRadiance + sunRadiance * phase + lights);
                }

                light += transmittance * OneMinusExp(sigma * sliceLength) * source / sigma;
                transmittance *= exp(-sigma * sliceLength);
                opticalDepth += sigma * sliceLength;
            }

            // One part: the slice's own values; several: their length-weighted mean (the partial slice treats it as homogeneous).
            const float weight = parts == 1 ? 1.0 : sliceLength / (FroxelDistanceOf(float(slice) + 1.0) - FroxelSliceStart(slice));
            sliceSource += weight * source;
            sliceSigma += weight * sigma;
        }

        const bool finite = all(isfinite(sliceSource)) && isfinite(sliceSigma);
        segments[froxel] = finite ? float4(max(sliceSource, 0.0), max(sliceSigma, 0.0)) : float4(0.0, 0.0, 0.0, 0.0);
    }
}

[numthreads(8, 8, 1)]
void CSFroxelIntegrate(uint3 column : SV_DispatchThreadID)
{
    FroxelIntegrate(column.xy, false);
}

[numthreads(8, 8, 1)]
void CSFroxelIntegrateBounded(uint3 column : SV_DispatchThreadID)
{
    FroxelIntegrate(column.xy, true);
}

// The integrated (L, T) from the camera to distance `distance` along the reduced pixel's ray at `uv`: the cumulative values at the near
// face of the slice holding it (bilinear between columns), then that slice's homogeneous segment over the remaining length, exactly.
// Distances beyond the froxel range end at its far face.
float4 FroxelIntegratedAt(float2 uv, float distance)
{
    Texture3D<float4> integrated = ResourceDescriptorHeap[NonUniformResourceIndex(froxelIntegratedIndex)];
    Texture3D<float4> segments = ResourceDescriptorHeap[NonUniformResourceIndex(froxelSegmentIndex)];
    const float lastSlice = float(froxelSize.z) - 1.0;
    const float slice = min(floor(FroxelSliceOf(distance)), lastSlice);
    const float start = FroxelSliceStart(uint(slice));
    const float end = FroxelDistanceOf(slice + 1.0);
    const float covered = clamp(distance, start, end) - start;
    const float3 coordinate = float3(uv, (slice + 0.5) / float(froxelSize.z));
    const float4 cumulative = integrated.SampleLevel(froxelSampler, coordinate, 0.0);
    const float4 segment = segments.SampleLevel(froxelSampler, coordinate, 0.0);
    const float absorbed = segment.a > 0.0 ? OneMinusExp(segment.a * covered) : 0.0;
    const float3 added = segment.a > 0.0 ? cumulative.a * absorbed * segment.rgb / segment.a : float3(0.0, 0.0, 0.0);
    return float4(cumulative.rgb + added, cumulative.a * (1.0 - absorbed));
}

// The table Lights' near field along a pixel's ray up to `rayEnd`: for each point or spot Light whose sphere of radius 2r the ray crosses,
// NearLightSamples equiangular samples over that crossing of (1 - FarLightShare) x its in-scattering, weighted by the medium's scattering
// coefficient there (the medium volume's mist, the haze's own) and the integrated transmittance from the camera.
float3 NearLights(float2 uv, float3 direction, float rayEnd, bool bounded)
{
    float3 added = float3(0.0, 0.0, 0.0);

    if(froxelNearLightRadius <= 0.0)
    {
        return added;
    }

    Texture3D<float4> mediumVolume = ResourceDescriptorHeap[NonUniformResourceIndex(froxelMediumIndex)];
    const float3 hazeScattering = hazeAlbedo * scatteringTint * scattering;
    const float reach = 2.0 * froxelNearLightRadius;
    const float rangeEnd = min(rayEnd, FroxelDistanceOf(float(froxelSize.z)));
    const uint count = sceneLightCount;

    for(uint index = 0; index < count; ++index)
    {
        const GPULight sceneLight = SceneLightLoad(sceneLightBufferIndex, index);

        if(sceneLight.type == SCENE_LIGHT_DIRECTIONAL)
        {
            continue;
        }

        // The ray's crossing of the sphere of radius 2r about the Light.
        const float3 toLight = sceneLight.position - cameraPosition;
        const float along = dot(toLight, direction);
        const float squaredMiss = dot(toLight, toLight) - along * along;

        if(squaredMiss >= reach * reach)
        {
            continue;
        }

        const float halfChord = sqrt(reach * reach - squaredMiss);
        const float from = max(along - halfChord, 0.0);
        const float to = min(along + halfChord, rangeEnd);

        if(!(to > from))
        {
            continue;
        }

        // Equiangular samples (uniform in the angle subtended at the Light), so the 1/d^2 peak at the closest approach is resolved: each
        // sample's length is the angle step x (miss^2 + offset^2) / miss.
        const float miss = max(sqrt(squaredMiss), 1e-3);
        const float firstAngle = atan((from - along) / miss);
        const float angleStep = (atan((to - along) / miss) - firstAngle) / float(NearLightSamples);

        for(uint sampleIndex = 0; sampleIndex < NearLightSamples; ++sampleIndex)
        {
            const float offset = miss * tan(firstAngle + (float(sampleIndex) + 0.5) * angleStep);
            const float t = along + offset;
            const float step = angleStep * (miss * miss + offset * offset) / miss;
            const float3 world = cameraPosition + direction * t;
            const float nearShare = 1.0 - FarLightShare(length(sceneLight.position - world));

            if(nearShare <= 0.0)
            {
                continue;
            }

            float3 directionToLight;
            const float3 incidence = EvaluateSceneLightIncidence(sceneLight, world, directionToLight);

            if(!any(incidence > 0.0))
            {
                continue;
            }

            const float3 coordinate = float3(uv, FroxelSliceOf(t) / float(froxelSize.z));
            const float3 mistScattering = mediumVolume.SampleLevel(froxelSampler, coordinate, 0.0).rgb;
            float hazeSigma = 0.0;

            if(hazeEnabled != 0)
            {
                hazeSigma = (bounded ? HazeWeight(t, direction) : NearWeight(t, hazeNearDistance, hazeNearTransition)) * hazeExtinction;
            }

            const float transmittance = FroxelIntegratedAt(uv, t).a;

            // Past the mist's opaque point the integration holds T at the threshold; the march stops there, so the near field does too.
            if(transmittance <= opaqueTransmittance)
            {
                break;
            }
            const float phase = HenyeyGreenstein(dot(directionToLight, direction), anisotropy);
            added += transmittance * (mistScattering + hazeSigma * hazeScattering) * nearShare * incidence * phase * step;
        }
    }

    return added;
}

// One reduced pixel: as RaymarchAtmosphere (the same depth, ray and outputs), with the march replaced by the integrated lookup.
void RaymarchIntegrated(uint3 thread, bool bounded)
{
    if(any(thread.xy >= reducedSize))
    {
        return;
    }

    const float2 uv = (float2(thread.xy) + 0.5) / float2(reducedSize);
    const uint2 fullPixel = min(uint2(uv * float2(fullSize)), fullSize - 1);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[NonUniformResourceIndex(depthIndex)];
    const float depth = depthTexture.Load(int3(fullPixel, 0));
    const float2 clip = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 direction = normalize(WorldAt(clip, 1.0) - cameraPosition);
    const float sceneDistance = depth < 1.0 ? length(WorldAt(clip, depth) - cameraPosition) : NoGeometry;
    const float rayEnd = sceneDistance < NoGeometry ? sceneDistance : length(WorldAt(clip, 1.0) - cameraPosition);

    const float4 integrated = FroxelIntegratedAt(uv, rayEnd);
    float3 color = integrated.rgb + NearLights(uv, direction, rayEnd, bounded);
    float transmittance = integrated.a;
    // The haze beyond the froxel range, exactly as the march's tail (its optical depth so far for the isotropy).
    float opticalDepth = -log(max(transmittance, 1e-30));
    const float3 environment = hazeEnabled != 0 && hazeAmbient > 0.0 ? EnvironmentRadiance() : float3(0.0, 0.0, 0.0);
    AddHazeInterval(FroxelDistanceOf(float(froxelSize.z)), rayEnd, HazeSubintervals, direction, hazeAmbient * environment, bounded, color,
        transmittance, opticalDepth);

    const uint pixelIndex = thread.x + reducedSize.x * thread.y;
    RWByteAddressBuffer result = ResourceDescriptorHeap[NonUniformResourceIndex(resultIndex)];
    RWByteAddressBuffer distances = ResourceDescriptorHeap[NonUniformResourceIndex(distanceIndex)];
    const bool finite = all(isfinite(color)) && isfinite(transmittance);
    result.Store4(pixelIndex * 16, asuint(finite ? float4(max(color, 0.0), saturate(transmittance)) : float4(0.0, 0.0, 0.0, 1.0)));
    distances.Store(pixelIndex * 4, asuint(sceneDistance));
}

[numthreads(8, 8, 1)]
void CSRaymarchIntegrated(uint3 thread : SV_DispatchThreadID)
{
    RaymarchIntegrated(thread, false);
}

[numthreads(8, 8, 1)]
void CSRaymarchIntegratedBounded(uint3 thread : SV_DispatchThreadID)
{
    RaymarchIntegrated(thread, true);
}

struct CompositeVertex
{
    float4 position : SV_Position;
};

CompositeVertex VSComposite(uint vertex : SV_VertexID)
{
    CompositeVertex output;
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// The reduced result at (x, y), clamped to the target.
float4 ReducedResult(int2 pixel, out float distance)
{
    const int2 clamped = clamp(pixel, int2(0, 0), int2(reducedSize) - 1);
    const uint index = uint(clamped.x) + reducedSize.x * uint(clamped.y);
    ByteAddressBuffer result = ResourceDescriptorHeap[NonUniformResourceIndex(resultIndex)];
    ByteAddressBuffer distances = ResourceDescriptorHeap[NonUniformResourceIndex(distanceIndex)];
    distance = asfloat(distances.Load(index * 4));
    return asfloat(result.Load4(index * 16));
}

// How alike two scene distances are (1 alike; falls off over 5% of the distance). Both without geometry are alike.
float DistanceWeight(float reduced, float full)
{
    if(reduced >= NoGeometry && full >= NoGeometry)
    {
        return 1.0;
    }

    const float difference = abs(reduced - full) / max(min(reduced, full), 1e-3);
    return exp(-difference * 20.0);
}

// Experimental filter (diagnostic; off by default; the raymarch's result or the integrated lookup's, which are written alike): a 3 x 3
// Gaussian (1 2 1 x 1 2 1) over the reduced result, each neighbour also
// weighted by how alike its scene distance is to the centre's (DistanceWeight, the composite's), so mist does not bleed across
// silhouettes. In-scattered light and transmittance take the same normalized weights: both are linear in the composite (scene x T + L),
// so the mean of nearby pixels' results keeps energy and colour. Neighbours off the target are skipped; the centre always counts.
[numthreads(8, 8, 1)]
void CSFilterIntegrated(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread.xy >= reducedSize))
    {
        return;
    }

    ByteAddressBuffer result = ResourceDescriptorHeap[NonUniformResourceIndex(resultIndex)];
    ByteAddressBuffer distances = ResourceDescriptorHeap[NonUniformResourceIndex(distanceIndex)];
    const uint centreIndex = thread.x + reducedSize.x * thread.y;
    const float centreDistance = asfloat(distances.Load(centreIndex * 4));
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    const int stride = max(int(filterDiagnostic & 15u), 1);
    const bool depthWeighted = (filterDiagnostic & 16u) == 0;

    for(int y = -1; y <= 1; ++y)
    {
        for(int x = -1; x <= 1; ++x)
        {
            const int2 pixel = int2(thread.xy) + int2(x, y) * stride;

            if(any(pixel < 0) || any(pixel >= int2(reducedSize)))
            {
                continue;
            }

            const uint index = uint(pixel.x) + reducedSize.x * uint(pixel.y);
            const float gaussian = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            const float weight = gaussian * (depthWeighted ? DistanceWeight(asfloat(distances.Load(index * 4)), centreDistance) : 1.0);
            sum += weight * asfloat(result.Load4(index * 16));
            total += weight;
        }
    }

    // The centre's own weight is 4 (its distance matches itself), so the total is never 0.
    RWByteAddressBuffer filtered = ResourceDescriptorHeap[NonUniformResourceIndex(filteredResultIndex)];
    filtered.Store4(centreIndex * 16, asuint((filterDiagnostic & 32u) != 0 ? float4(total / 16.0, total / 16.0, total / 16.0, 0.0) :
        sum / total));
}

float4 PSComposite(CompositeVertex input) : SV_Target
{
    const uint2 fullPixel = uint2(input.position.xy);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[NonUniformResourceIndex(depthIndex)];
    const float depth = depthTexture.Load(int3(fullPixel, 0));
    const float2 uv = (float2(fullPixel) + 0.5) / float2(fullSize);
    const float2 clip = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float sceneDistance = depth < 1.0 ? length(WorldAt(clip, depth) - cameraPosition) : NoGeometry;

    // The reduced samples around the pixel, each weighted by a spatial kernel and by how well its scene distance matches this pixel's;
    // where none matches (a silhouette finer than the reduced target), the best-matching sample alone, so foreground mist never
    // spills onto the background or back. F3's kernel is bilinear over the 4 nearest; F4.2's (wideUpsample) is a tent of radius 2
    // reduced pixels over the 4 x 4 nearest, which averages the jittered starts of about 9 neighbouring rays instead of 2 x 2.
    const float2 reducedPosition = uv * float2(reducedSize) - 0.5;
    const int2 base = int2(floor(reducedPosition));
    const float2 fraction = reducedPosition - float2(base);
    const int first = wideUpsample != 0 ? -1 : 0;
    const int last = wideUpsample != 0 ? 2 : 1;
    const float radius = wideUpsample != 0 ? 2.0 : 1.0;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    float4 best = float4(0.0, 0.0, 0.0, 1.0);
    float bestWeight = -1.0;

    for(int y = first; y <= last; ++y)
    {
        for(int x = first; x <= last; ++x)
        {
            float distance;
            const float4 value = ReducedResult(base + int2(x, y), distance);
            const float2 axisWeight = max(1.0 - abs(float2(x, y) - fraction) / radius, 0.0);
            const float match = DistanceWeight(distance, sceneDistance);
            const float weight = axisWeight.x * axisWeight.y * match;
            sum += value * weight;
            total += weight;

            if(match > bestWeight)
            {
                bestWeight = match;
                best = value;
            }
        }
    }

    const float4 mist = total > 1e-4 ? sum / total : best;
    const float alpha = saturate(1.0 - mist.a);

    if(alpha <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(mist.rgb / alpha, alpha);
}
