// Editor-only adaptive grid on the world plane Y = 0 (G.3), drawn as one fullscreen triangle
// into the final Editor viewport image after tone mapping.
//
// Each pixel's view ray is reconstructed in camera-relative space (world translated so the camera
// is at the origin) and intersected with the plane. The hit's depth is written to SV_Depth and
// tested LESS_EQUAL against the read-only scene depth, so solid geometry occludes the grid while
// the grid never writes depth. Constants and formulas mirror src/Renderer/EditorGrid.h.
//
// Forty root constants; CPU Matrix4 and HLSL both use row-major storage with column vectors.
cbuffer GridConstants : register(b0)
{
    row_major float4x4 clipFromCameraRelative;
    row_major float4x4 cameraRelativeFromClip;
    float3 cameraPosition;
    float baseSpacing;
    // Camera XZ reduced by a whole lattice period on the CPU (double precision).
    float2 latticeOffset;
    float2 padding;
};

static const float FadeStartPixels = 4.0;
static const float FadeEndPixels = 12.0;
static const float MajorStartPixels = 40.0;
static const float MajorEndPixels = 200.0;
static const float MinorAlpha = 0.24;
static const float MajorAlpha = 0.42;
static const float AxisAlpha = 0.65;
static const int MaxLevelIndex = 12;
static const int LevelsPerPixel = 7;

// Rays closer to parallel than this never meet the plane in front of the camera usefully;
// GrazingFade* then fades lines out before float precision and moire take over.
static const float ParallelRayLimit = 1.0e-6;
static const float GrazingFadeStart = 0.015;
static const float GrazingFadeEnd = 0.08;

static const float3 LineColor = float3(0.62, 0.64, 0.68);
static const float3 XAxisColor = float3(0.86, 0.36, 0.36);
static const float3 ZAxisColor = float3(0.36, 0.52, 0.92);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 clipXY : TEXCOORD0;
};

struct PixelOutput
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

VertexOutput VSMain(uint vertexID : SV_VertexID)
{
    // Vertices (-1, 1), (3, 1), (-1, -3) cover the viewport with one triangle.
    const float2 corner = float2((vertexID << 1) & 2, vertexID & 2);
    VertexOutput output;
    output.clipXY = corner * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.position = float4(output.clipXY, 0.0, 1.0);
    return output;
}

// Integer multiple of the base spacing: 1, 5, 10, 50, 100, ...
float LevelMultiple(int levelIndex)
{
    const float decade = pow(10.0, float(levelIndex / 2));
    return (levelIndex % 2 == 0) ? decade : 5.0 * decade;
}

int FinestLevelAtMost(float requiredMultiple)
{
    if(requiredMultiple <= 1.0)
    {
        return 0;
    }

    const float decade = floor(log10(requiredMultiple));
    const float mantissa = requiredMultiple / pow(10.0, decade);
    const int levelIndex = int(decade) * 2 + (mantissa >= 5.0 ? 1 : 0);
    return clamp(levelIndex, 0, MaxLevelIndex);
}

float LevelAlpha(float pixelSpacing)
{
    const float visibility = smoothstep(FadeStartPixels, FadeEndPixels, pixelSpacing);
    const float emphasis = smoothstep(MajorStartPixels, MajorEndPixels, pixelSpacing);
    return visibility * lerp(MinorAlpha, MajorAlpha, emphasis);
}

// Anti-aliased coverage of the nearest lattice line, about one pixel wide. latticeDerivative is
// the lattice-coordinate change per pixel, so the distance is measured in screen pixels.
float LineCoverage(float2 latticeCoordinate, float2 latticeDerivative)
{
    const float2 distanceToLine = abs(frac(latticeCoordinate - 0.5) - 0.5) / max(latticeDerivative, 1.0e-8);
    return saturate(1.0 - min(distanceToLine.x, distanceToLine.y));
}

float AxisCoverage(float worldCoordinate, float worldDerivative)
{
    // Slightly wider than grid lines (about 1.5 pixels).
    return saturate(1.25 - abs(worldCoordinate) / max(worldDerivative, 1.0e-8));
}

PixelOutput PSMain(VertexOutput input)
{
    // Reconstruct the camera-relative ray between the near (z = 0) and far (z = 1) planes.
    const float4 nearHomogeneous = mul(cameraRelativeFromClip, float4(input.clipXY, 0.0, 1.0));
    const float4 farHomogeneous = mul(cameraRelativeFromClip, float4(input.clipXY, 1.0, 1.0));
    const float3 rayOrigin = nearHomogeneous.xyz / nearHomogeneous.w;
    const float3 rayDirection = farHomogeneous.xyz / farHomogeneous.w - rayOrigin;

    // The plane Y = 0 is at camera-relative height -cameraPosition.y. Keep the math branch-free
    // until the end so screen-space derivatives stay valid across the pixel quad.
    const bool parallelRay = abs(rayDirection.y) < ParallelRayLimit;
    const float safeDirectionY = parallelRay ? ParallelRayLimit : rayDirection.y;
    const float rayParameter = (-cameraPosition.y - rayOrigin.y) / safeDirectionY;
    const float3 hit = rayOrigin + rayDirection * rayParameter;

    const float4 hitClip = mul(clipFromCameraRelative, float4(hit, 1.0));
    const float hitDepth = hitClip.z / hitClip.w;

    // Lattice coordinates in base-spacing units. Derivatives come from the camera-relative hit
    // only; the lattice offset is constant across the screen.
    const float2 hitDerivative = fwidth(hit.xz);
    const float2 baseCoordinate = (latticeOffset + hit.xz) / baseSpacing;
    const float2 baseDerivative = hitDerivative / baseSpacing;
    const float footprint = max(max(baseDerivative.x, baseDerivative.y), 1.0e-8);

    // Draw the finest levels that are not denser than FadeStartPixels, plus coarser ones for
    // major emphasis. Every level is a multiple of the base spacing and of all finer levels.
    const int firstLevel = FinestLevelAtMost(footprint * FadeStartPixels);
    float lineAlpha = 0.0;

    for(int levelOffset = 0; levelOffset < LevelsPerPixel; ++levelOffset)
    {
        const int levelIndex = firstLevel + levelOffset;

        if(levelIndex > MaxLevelIndex)
        {
            break;
        }

        const float multiple = LevelMultiple(levelIndex);
        const float levelAlpha = LevelAlpha(multiple / footprint);
        const float coverage = LineCoverage(baseCoordinate / multiple, baseDerivative / multiple);
        lineAlpha = max(lineAlpha, levelAlpha * coverage);
    }

    // Origin axes use true world coordinates: the X axis is the line Z = 0, the Z axis X = 0.
    const float2 world = cameraPosition.xz + hit.xz;
    const float xAxisAlpha = AxisAlpha * AxisCoverage(world.y, hitDerivative.y);
    const float zAxisAlpha = AxisAlpha * AxisCoverage(world.x, hitDerivative.x);

    const float grazing = abs(rayDirection.y) / max(length(rayDirection), 1.0e-8);
    const float grazingFade = smoothstep(GrazingFadeStart, GrazingFadeEnd, grazing);

    const float alpha = grazingFade * max(lineAlpha, max(xAxisAlpha, zAxisAlpha));
    const float colorWeight = max(lineAlpha + xAxisAlpha + zAxisAlpha, 1.0e-6);
    const float3 color = (LineColor * lineAlpha + XAxisColor * xAxisAlpha + ZAxisColor * zAxisAlpha) / colorWeight;

    // Reject rays that miss the forward plane: parallel, behind the camera, outside the depth
    // range or non-finite.
    const bool validHit = !parallelRay && rayParameter > 0.0 && hitClip.w > 0.0 &&
        hitDepth >= 0.0 && hitDepth <= 1.0 && !isnan(hitDepth) && !isinf(hitDepth) && alpha > 0.0;

    if(!validHit)
    {
        discard;
    }

    PixelOutput output;
    output.color = float4(color, alpha);
    output.depth = hitDepth;
    return output;
}
