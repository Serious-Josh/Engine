#include "MaterialLighting.hlsli"
#include "SurfaceDetail.hlsli"
#include "SurfaceDetailSilhouette.hlsli"
#include "PsrEligibility.hlsli"

#define DEBUG_DISPLACEMENT_TEST_LOD 1

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    // A pixel shader with a conservative depth output (PSMainParallax) must read the position with this interpolation.
    // The targets have one sample at the pixel centre, where it is the same value as the default.
    linear noperspective centroid float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
    float4 currentClipPosition : TEXCOORD1;
    float4 previousClipPosition : TEXCOORD2;
    float2 viewDepths : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
    float3 worldPosition : TEXCOORD5;
    // The mesh's own position, for the path tracer's lift scale (PathTracePrimaryRasterLiftScale): interpolated here and
    // transformed in the pixel shader, it keeps the surface's precision on triangles kilometres long.
    float3 objectPosition : TEXCOORD6;
};

struct PSOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 motion : SV_Target2;
    float4 geometricNormal : SV_Target3;
    // P5D.2A raster identity (GBuffer::IdentityFormat): (TLAS InstanceID + 1, TLAS primitive), or all ones when this
    // draw has no exact TLAS mapping.
    uint2 identity : SV_Target4;
};

// PSMainParallax's outputs: the same targets, and the depth of the point the view ray meets on the relief. The semantic
// promises a depth at or behind the triangle's, so the early depth test still rejects hidden fragments.
struct PSParallaxOutput
{
    PSOutput targets;
    float depth : SV_DepthGreaterEqual;
};

// What a pixel's motion vector is made from: the clip positions of its point now (without jitter) and a frame ago, and
// the view depths of both.
struct MotionInputs
{
    float4 currentClipPosition;
    float4 previousClipPosition;
    float2 viewDepths;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
    float roughness;
    uint materialIndex;
    uint materialBufferIndex;

    float3 baseColor;
    uint roughnessTextureIndex;
    uint displacementLODDescriptorIndex;
    uint rasterInstance;
    uint rasterPrimitiveBase;
}

cbuffer CameraConstants : register(b1)
{
    // Rasterization (jittered while UPSCALING.2B camera jitter is active).
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 view;
    row_major float4x4 previousView;
    // Motion vectors exclude jitter: the current clip position uses this, never viewProjection.
    row_major float4x4 unjitteredViewProjection;
    // UPSCALING.3D: material texture mip bias, non-zero only while DLSS reconstructs the frame (0 keeps Sample).
    float textureMipBias;
    // PSR.2: non-zero only on Path Tracing frames with in-place replacement enabled: identity.x bit 31 marks
    // PSR-eligible mirrors (PsrEligibility.hlsli).
    uint psrEligibility;
    // The camera the lift scales are measured with (PrimarySurfaceLift.hlsli), which is the path tracer's camera for the
    // decode the eligibility evaluates. Set on every frame.
    uint2 psrResolution;
    row_major float4x4 psrInverseCameraRelativeViewProjection;
    float3 psrCameraPosition;
    // Non-zero on Path Tracing frames with a raster primary: every pixel's geometric normal is stored with its lift scale
    // as its length. (A relief pixel of PSMainParallax stores its scale on every frame.)
    uint primarySurfaceLiftScale;
    // Silhouette parallax occlusion mapping: the ray tracing instance records, or 0xFFFFFFFF
    // (SurfaceDetailSilhouette.hlsli).
    uint instanceDataIndex;
    uint3 cameraPadding;
}

cbuffer ObjectConstants : register(b2)
{
    row_major float4x4 model;
    row_major float4x4 previousModel;
    row_major float4x4 normalMatrix;
}

SamplerState sampler0 : register(s0);

float3 GetCameraWorldPosition()
{
    // The bound view matrix is a rigid world-to-camera transform.
    float3 translation = float3(view[0][3], view[1][3], view[2][3]);
    float3 cameraWorld = -mul(transpose((float3x3)view), translation);

    return cameraWorld;
}

bool TryGetWorldViewDirection(
    float3 worldPosition,
    out float3 worldViewDirection)
{
    worldViewDirection = float3(0.0f, 0.0f, 0.0f);
    float3 cameraWorld = GetCameraWorldPosition();

    if(!all(isfinite(worldPosition)) || !all(isfinite(cameraWorld)))
    {
        return false;
    }

    float3 direction = cameraWorld - worldPosition;
    float lengthSquared = dot(direction, direction);

    if(!all(isfinite(direction)) || !isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
    {
        return false;
    }

    worldViewDirection = direction * rsqrt(lengthSquared);

    return true;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    float4 worldPosition =
        mul(
            model,
            float4(
                input.position,
                1.0f
            )
        );

    output.position =
        mul(
            viewProjection,
            worldPosition
        );

    output.worldPosition = worldPosition.xyz;
    output.objectPosition = input.position;
    output.currentClipPosition =
        mul(
            unjitteredViewProjection,
            worldPosition
        );

    float4 previousWorldPosition =
        mul(
            previousModel,
            float4(input.position, 1.0f)
        );

    output.previousClipPosition =
        mul(
            previousViewProjection,
            previousWorldPosition
        );

    output.viewDepths = float2(
        mul(view, worldPosition).z,
        mul(previousView, previousWorldPosition).z
    );

    output.worldNormal = mul((float3x3)normalMatrix, input.normal);
    // Tangents are surface directions; only normals use the inverse transpose.
    float modelOrientation = determinant((float3x3)model) < 0.0f ? -1.0f : 1.0f;
    output.worldTangent = float4(
        mul((float3x3)model, input.tangent.xyz),
        input.tangent.w * modelOrientation
    );

    output.uv =
        input.uv;

    return output;
}

PathTracePrimaryCamera GetLiftCamera()
{
    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = psrInverseCameraRelativeViewProjection;
    camera.cameraPosition = psrCameraPosition;
    camera.resolution = psrResolution;
    return camera;
}

// The depth the rasterizer would store for a world point (the jittered viewProjection). Never in front of
// rasterizedDepth, which SV_DepthGreaterEqual promises, and never the background's 1.
float GetReliefDepth(float3 worldPosition, float rasterizedDepth)
{
    const float largestSurfaceDepth = 0.99999994f;
    float4 clipPosition = mul(viewProjection, float4(worldPosition, 1.0f));
    float depth = clipPosition.z / clipPosition.w;

    if(!isfinite(depth))
    {
        return rasterizedDepth;
    }

    return max(min(depth, largestSurfaceDepth), rasterizedDepth);
}

// The motion inputs of the point worldOffset away from the rasterized one (the visible point of a relief). The offset
// goes back to object space (the inverse of model's 3x3 is the transpose of normalMatrix's) and forward through the
// previous frame's matrices, as the vertex shader takes the vertex itself. Every term is linear in the offset, so adding
// to the interpolated values is exact. This is the one place a displaced point's motion is worked out.
MotionInputs GetOffsetMotionInputs(VSOutput input, float3 worldOffset)
{
    float3 objectOffset = mul(worldOffset, (float3x3)normalMatrix);
    float3 previousWorldOffset = mul((float3x3)previousModel, objectOffset);

    MotionInputs motionInputs;
    motionInputs.currentClipPosition = input.currentClipPosition +
        mul(unjitteredViewProjection, float4(worldOffset, 0.0f));
    motionInputs.previousClipPosition = input.previousClipPosition +
        mul(previousViewProjection, float4(previousWorldOffset, 0.0f));
    motionInputs.viewDepths = input.viewDepths + float2(
        mul(view, float4(worldOffset, 0.0f)).z,
        mul(previousView, float4(previousWorldOffset, 0.0f)).z);
    return motionInputs;
}

// Both pixel shaders. reliefDepth is PSMainParallax's: a parallax pixel then behaves as the point the view ray really
// meets (depth, motion vectors and the lift scale that brings ray origins back to the mesh surface) instead of the flat
// triangle. `depth` is the depth to store, the rasterized one wherever that does not apply.
PSOutput ResolveGBuffer(VSOutput input, uint primitiveID, bool reliefDepth, out float depth)
{
    PSOutput output;

    // Derivatives must precede divergent material-dependent control flow.
    float3 positionDx = ddx(input.worldPosition);
    float3 positionDy = ddy(input.worldPosition);
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);

    // Reuse the normal descriptor DWORD for access to the complete GPU material.
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    GPUMaterial material = materials[materialIndex];
    uint normalTextureIndex = material.normalTextureIndex;

    // Parallax occlusion mapping (SurfaceDetail.hlsli): the UV where the view ray meets the material's height field. The
    // mesh itself carries no relief, so its reference height is 0.
    SurfaceDetailResult detail;
    detail.uv = input.uv;
    detail.height = 0.0f;
    detail.viewCosine = 0.0f;
    detail.fade = 0.0f;
    detail.displaced = false;
    detail.explicitGradients = false;
    detail.uvDx = uvDx;
    detail.uvDy = uvDy;

    // The offset from the rasterized point to the visible point of the relief (zero for every other pixel).
    float3 reliefOffset = float3(0.0f, 0.0f, 0.0f);

    if(SurfaceDetailUsesParallax(material))
    {
        float3 worldViewDirection;

        if(TryGetWorldViewDirection(input.worldPosition, worldViewDirection))
        {
            detail = SurfaceDetailResolve(material, sampler0, input.uv, uvDx, uvDy, positionDx, positionDy,
                input.worldNormal, input.worldTangent, worldViewDirection, 0.0f);

            if(reliefDepth)
            {
                reliefOffset = SurfaceDetailWorldOffset(detail, worldViewDirection);

                // Silhouette parallax occlusion mapping: a view ray that leaves the mesh before it meets the relief
                // shows what lies behind. Only parallax draws, whose identity names their triangle.
                bool silhouette = (material.lobeFlags & GPU_MATERIAL_PARALLAX_SILHOUETTE) != 0u && detail.displaced &&
                    rasterInstance != 0xFFFFFFFFu && instanceDataIndex != 0xFFFFFFFFu;

                if(silhouette)
                {
                    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[instanceDataIndex];

                    if(SurfaceDetailLeavesSilhouette(instances[rasterInstance], rasterPrimitiveBase + primitiveID,
                        detail.uv))
                    {
                        discard;
                    }
                }
            }
        }
    }

    const bool relief = any(reliefOffset != 0.0f);
    depth = input.position.z;

    if(relief)
    {
        depth = GetReliefDepth(input.worldPosition + reliefOffset, input.position.z);
    }

    Texture2D<float4> baseColorTexture =
        ResourceDescriptorHeap[
            textureIndex
        ];

    float3 sampledBaseColor =
        SurfaceDetailSample(baseColorTexture, sampler0, detail, textureMipBias).rgb;

    float3 finalBaseColor =
        baseColor *
        sampledBaseColor;

    bool useARM = material.armTextureIndex != 0xFFFFFFFFu && material.preferARM != 0u;
    float2 armRoughnessMetallic = float2(1.0f, 1.0f);
    float materialAO = 1.0f;
    if(useARM)
    {
        Texture2D<float4> armTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.armTextureIndex)];
        float3 sampledARM = SurfaceDetailSample(armTexture, sampler0, detail, textureMipBias).rgb;
        armRoughnessMetallic = sampledARM.gb;
        materialAO = saturate(sampledARM.r);
    }
    else if(material.aoTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> aoTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.aoTextureIndex)];
        materialAO = saturate(SurfaceDetailSample(aoTexture, sampler0, detail, textureMipBias).r);
    }

    float finalRoughness = roughness;
    if(useARM)
    {
        finalRoughness = saturate(roughness * armRoughnessMetallic.x);
    }
    else if(roughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[roughnessTextureIndex];
        float sampledRoughness = SurfaceDetailSample(roughnessTexture, sampler0, detail, textureMipBias).r;
        finalRoughness = saturate(roughness * sampledRoughness);
    }

    output.material =
        float4(
            finalBaseColor,
            finalRoughness
        );

    float3 N = normalize(input.worldNormal);
    float3 mappedNormal = N;

    if(normalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalTextureIndex];
        float2 normalXY = SurfaceDetailSample(normalTexture, sampler0, detail, textureMipBias).rg * 2.0f - 1.0f;
        float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
        float3 sampledNormal = float3(normalXY, normalZ);
        float3 tangent = input.worldTangent.xyz - N * dot(N, input.worldTangent.xyz);
        float tangentLengthSquared = dot(tangent, tangent);
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);
        float handedness = input.worldTangent.w;
        if(all(isfinite(N)) && all(isfinite(tangent)) &&
           isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
           all(isfinite(sampledNormal)) && isfinite(sampledLengthSquared) &&
           sampledLengthSquared > 1.0e-8f && isfinite(handedness) && abs(handedness) > 1.0e-8f)
        {
            float3 T = normalize(tangent);
            float3 B = cross(N, T) * handedness;
            float3 worldMappedNormal = T * sampledNormal.x + B * sampledNormal.y + N * sampledNormal.z;
            float mappedLengthSquared = dot(worldMappedNormal, worldMappedNormal);

            if(all(isfinite(B)) && all(isfinite(worldMappedNormal)) &&
               isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
            {
                mappedNormal = normalize(worldMappedNormal);
            }
        }
    }

    output.surface = float4(mappedNormal, float(materialIndex));

    // Every OpenPBR material has a metalness (base_metalness), textured by ARM blue or the metallic texture.
    float effectiveMetallic = saturate(material.metallic);
    if(useARM)
    {
        effectiveMetallic = saturate(effectiveMetallic * armRoughnessMetallic.y);
    }
    else if(material.metallicTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> metallicTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.metallicTextureIndex)];
        float sampledMetallic = SurfaceDetailSample(metallicTexture, sampler0, detail, textureMipBias).r;
        effectiveMetallic = saturate(effectiveMetallic * sampledMetallic);
    }

    output.geometricNormal = float4(normalize(input.worldNormal), effectiveMetallic);

    output.motion = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.identity = rasterInstance == 0xFFFFFFFFu ? uint2(0xFFFFFFFFu, 0xFFFFFFFFu) :
        uint2(rasterInstance + 1u, rasterPrimitiveBase + primitiveID);

    // A relief pixel is never an eligible mirror: PSR recovers a mirror as its flat triangle.
    if(psrEligibility != 0u && rasterInstance != 0xFFFFFFFFu && !relief)
    {
        StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];

        if(PsrRasterEligible(uint2(input.position.xy), input.position.z, output.material, output.surface,
            output.geometricNormal, GetLiftCamera(), materials))
        {
            output.identity.x |= PSR_IDENTITY_ELIGIBLE;
        }
    }

    // The pixel's lift scale, as the stored geometric normal's length (PrimarySurfaceLift.hlsli).
    if(relief)
    {
        // On every frame: the stored depth lies under the mesh, and every pass that traces from it reads this scale to
        // return to the mesh surface. The screen derivatives of the position span the triangle's own plane.
        float3 surfacePosition = mul(model, float4(input.objectPosition, 1.0f)).xyz;

        output.geometricNormal.xyz *= PathTracePrimaryReliefLiftScale(uint2(input.position.xy), depth, surfacePosition,
            output.geometricNormal.xyz, cross(positionDx, positionDy), GetLiftCamera());
    }
    else if(primarySurfaceLiftScale != 0u && !PsrIdentityEligible(output.identity.x))
    {
        // The path tracer's lift scale. An eligible mirror keeps its unit normal: PSR recovers the mirror from its
        // triangle, not from this sample.
        float3 surfacePosition = mul(model, float4(input.objectPosition, 1.0f)).xyz;

        output.geometricNormal.xyz *= PathTracePrimaryRasterLiftScale(uint2(input.position.xy), input.position.z,
            surfacePosition, output.geometricNormal.xyz, GetLiftCamera());
    }

    MotionInputs motionInputs;
    motionInputs.currentClipPosition = input.currentClipPosition;
    motionInputs.previousClipPosition = input.previousClipPosition;
    motionInputs.viewDepths = input.viewDepths;

    if(relief)
    {
        motionInputs = GetOffsetMotionInputs(input, reliefOffset);
    }

    if(all(isfinite(motionInputs.currentClipPosition)) &&
       all(isfinite(motionInputs.previousClipPosition)) &&
       all(isfinite(motionInputs.viewDepths)) &&
       abs(motionInputs.currentClipPosition.w) > 1.0e-6f &&
       abs(motionInputs.previousClipPosition.w) > 1.0e-6f)
    {
        float2 currentNDC =
            motionInputs.currentClipPosition.xy / motionInputs.currentClipPosition.w;
        float2 previousNDC =
            motionInputs.previousClipPosition.xy / motionInputs.previousClipPosition.w;

        float2 currentUV =
            currentNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        float2 previousUV =
            previousNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

        float2 motion = currentUV - previousUV;

        float4 motionVector = float4(
            motion,
            motionInputs.viewDepths.y - motionInputs.viewDepths.x,
            0.0f
        );

        if(all(isfinite(motionVector)))
        {
            output.motion = motionVector;
        }
    }

    output.motion.a = materialAO;

    return output;
}

// Every draw whose material has no parallax: the depth is the rasterizer's.
PSOutput PSMain(VSOutput input, uint primitiveID : SV_PrimitiveID)
{
    float depth;
    return ResolveGBuffer(input, primitiveID, false, depth);
}

// Draws whose material uses parallax occlusion mapping (SurfaceDetailUsesParallax; the renderer selects the pipeline per
// draw), so that only they pay for a pixel shader that writes depth.
PSParallaxOutput PSMainParallax(VSOutput input, uint primitiveID : SV_PrimitiveID)
{
    PSParallaxOutput output;
    output.targets = ResolveGBuffer(input, primitiveID, true, output.depth);
    return output;
}
