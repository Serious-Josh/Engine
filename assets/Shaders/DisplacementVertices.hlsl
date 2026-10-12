// Two 16-byte constant registers, bound at b0, space0 like GISpatialDenoise.
cbuffer DisplacementVerticesConstants : register(b0)
{
    uint patchesPerAxis;          // Byte 0
    uint subdivisionsPerPatch;    // Byte 4
    uint heightmapWidth;          // Byte 8
    uint heightmapHeight;         // Byte 12

    float displacementScale;      // Byte 16
    float displacementMidpoint;   // Byte 20
    uint heightmapIndex;          // Byte 24: Texture2D R8_UNORM SRV
    uint vertexBufferIndex;       // Byte 28: raw buffer UAV
};

float SampleHeight(Texture2D<float> heightmap, float2 uv)
{
    const float2 texelPosition = clamp(uv, 0.0f, 1.0f) *
        float2(heightmapWidth - 1u, heightmapHeight - 1u);
    const uint2 texel0 = uint2(texelPosition);
    const uint2 texel1 = min(texel0 + 1u,
        uint2(heightmapWidth - 1u, heightmapHeight - 1u));
    const float2 fraction = texelPosition - float2(texel0);

    // R8_UNORM Load converts each stored byte to a normalized float.
    // Explicit interpolation matches the CPU's (dimension - 1) mapping;
    // no sampler or hardware filtering is involved.
    const float h00 = heightmap.Load(int3(texel0, 0));
    const float h10 = heightmap.Load(int3(texel1.x, texel0.y, 0));
    const float h01 = heightmap.Load(int3(texel0.x, texel1.y, 0));
    const float h11 = heightmap.Load(int3(texel1, 0));
    const float top = lerp(h00, h10, fraction.x);
    const float bottom = lerp(h01, h11, fraction.x);
    return lerp(top, bottom, fraction.y);
}

float3 DisplacedPosition(Texture2D<float> heightmap, uint x, uint z,
    uint gridResolution)
{
    const float u = float(x) / float(gridResolution);
    const float v = float(z) / float(gridResolution);
    const float height = SampleHeight(heightmap, float2(u, v));
    return float3(u * 2.0f - 1.0f,
        displacementScale * (height - displacementMidpoint),
        v * 2.0f - 1.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint gridResolution = patchesPerAxis * subdivisionsPerPatch;
    const uint x = dispatchThreadID.x;
    const uint z = dispatchThreadID.y;

    if(gridResolution == 0u || heightmapWidth == 0u || heightmapHeight == 0u ||
       x > gridResolution || z > gridResolution)
    {
        return;
    }

    Texture2D<float> heightmap = ResourceDescriptorHeap[heightmapIndex];
    RWByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[vertexBufferIndex];

    const uint xPrev = x > 0u ? x - 1u : x;
    const uint xNext = x < gridResolution ? x + 1u : x;
    const uint zPrev = z > 0u ? z - 1u : z;
    const uint zNext = z < gridResolution ? z + 1u : z;
    const float3 dx = DisplacedPosition(heightmap, xNext, z, gridResolution) -
        DisplacedPosition(heightmap, xPrev, z, gridResolution);
    const float3 dz = DisplacedPosition(heightmap, x, zNext, gridResolution) -
        DisplacedPosition(heightmap, x, zPrev, gridResolution);
    const float3 normal = normalize(cross(dz, dx));
    const float3 position = DisplacedPosition(heightmap, x, z, gridResolution);
    const float2 uv = float2(float(x) / float(gridResolution),
        float(z) / float(gridResolution));

    // Match Vertex.h: position 0, normal 12, UV 24, tangent 32; stride 48.
    const uint vertexIndex = x * (gridResolution + 1u) + z;
    const uint byteOffset = vertexIndex * 48u;
    vertexBuffer.Store3(byteOffset + 0u, asuint(position));
    vertexBuffer.Store3(byteOffset + 12u, asuint(normal));
    vertexBuffer.Store2(byteOffset + 24u, asuint(uv));
    vertexBuffer.Store4(byteOffset + 32u, asuint(float4(1.0f, 0.0f, 0.0f, 1.0f)));
}
