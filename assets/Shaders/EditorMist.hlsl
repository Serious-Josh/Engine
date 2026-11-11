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
    float3 sunDirection;
    float lightingPadding0;
    float3 sunRadiance;
    float lightingPadding1;
    // AIR.4F F4.3.1: the fraction of the environment's mean radiance that reaches the mist (0: none), and the environment's diffuse
    // irradiance cube (UINT_MAX: none, and no ambient light).
    float ambient;
    uint irradianceIndex;
    float2 lightingPadding2;
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
};

// AIR.4F F4.3: the scene's Light table (SceneLightConstants), as deferred lighting binds it.
cbuffer SceneLightTable : register(b1)
{
    uint sceneLightCount;
    uint3 sceneLightPadding;
    GPULight sceneLights[SCENE_LIGHT_CAPACITY];
};

// AIR.4F F4.3: the scene's acceleration structure, for the sun's visibility.
RaytracingAccelerationStructure sceneTopLevel : register(t0);

SamplerState noiseSampler : register(s0);

static const uint BrickCells = 8;
// A ray ends once less than this fraction of the background shows through.
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

// AIR.4F F4.3: one visibility sample per fluid piece toward the sun, for every cell of a brick that a raymarch sample's footprint can
// reach: the brick or one of its 26 neighbours is occupied (occupancy already reaches one cell beyond a brick's mist). The ray starts at
// the piece's anchor, a point inside the piece's fluid sub-cells, and follows the sun's centre direction with the conventions of the
// ray-traced sun shadow (RayTracedShadow.hlsl): every instance (mask 0xFF), geometry opaque, the first hit ends it, TMin 0.001 m, TMax
// 10 km. One uint per cell slot: bit p set when piece p sees the sun.
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

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;

        if(mask == 0)
        {
            continue;
        }

        RayDesc ray;
        ray.Origin = (float3(cell) + AirPieceAnchor(mask)) * field.cellSize;
        ray.Direction = -normalize(sunDirection);
        ray.TMin = 0.001;
        ray.TMax = 10000.0;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
        query.TraceRayInline(sceneTopLevel, RAY_FLAG_NONE, 0xFF, ray);
        query.Proceed();

        if(query.CommittedStatus() == COMMITTED_NOTHING)
        {
            bits |= 1u << piece;
        }
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

// AIR.4F F4.3: the light scattered toward the camera at a world point, per unit of scattering coefficient: the sun (seen by
// `sunVisibility` of it), each table Light and the ambient radiance (`ambientRadiance`, isotropic), each weighted by the phase function.
float3 InScattered(float3 world, float3 rayDirection, float sunVisibility, float3 ambientRadiance)
{
    float3 light = ambientRadiance;

    if(sunVisibility > 0.0)
    {
        light += sunRadiance * HenyeyGreenstein(dot(-normalize(sunDirection), rayDirection), anisotropy) * sunVisibility;
    }

    const uint count = min(sceneLightCount, SCENE_LIGHT_CAPACITY);

    for(uint index = 0; index < count; ++index)
    {
        float3 directionToLight;
        const float3 incidence = EvaluateSceneLightIncidence(sceneLights[index], world, directionToLight);

        if(any(incidence > 0.0))
        {
            light += incidence * HenyeyGreenstein(dot(directionToLight, rayDirection), anisotropy);
        }
    }

    return light;
}

// A stable per-pixel offset in [0, 1), so sampling bands become fine noise that does not move (F4.2): every 4 x 4 block holds the 16
// Bayer ranks, so the composite's 4 x 4 tent averages a full set of offsets, and each block's ranks are rotated by a hash of the
// block, so the pattern does not repeat. F3's interleaved gradient noise is a linear ramp per row and column: against a ray length
// that changes smoothly across the floor it beat into repeating arcs.
float PixelOffset(uint2 pixel)
{
    static const uint Bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
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

[numthreads(8, 8, 1)]
void CSRaymarch(uint3 thread : SV_DispatchThreadID)
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
                    const float3 world = cameraPosition + direction * t;
                    color += transmittance * absorbed * albedo * scattering * InScattered(world, direction, sample.sunVisibility, ambientRadiance);
                }
                else
                {
                    color += transmittance * absorbed * albedo;
                }

                transmittance *= 1.0 - absorbed;
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
