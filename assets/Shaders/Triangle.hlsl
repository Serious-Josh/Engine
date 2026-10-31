#include "MaterialLighting.hlsli"

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
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
    float4 currentClipPosition : TEXCOORD1;
    float4 previousClipPosition : TEXCOORD2;
    float2 viewDepths : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
    float3 worldPosition : TEXCOORD5;
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
}

float4 SampleMaterialTexture(Texture2D<float4> materialTexture, SamplerState materialSampler, float2 uv)
{
    if(textureMipBias != 0.0f)
    {
        return materialTexture.SampleBias(materialSampler, uv, textureMipBias);
    }

    return materialTexture.Sample(materialSampler, uv);
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

bool TryGetPhysicalHeightUVMetric(
    float3 worldPosition,
    float2 originalUV,
    float3 worldNormal,
    float3 worldViewDirection,
    float3 positionDx,
    float3 positionDy,
    float2 uvDx,
    float2 uvDy,
    out float2 uvPerWorldHeight)
{
    uvPerWorldHeight = float2(0.0f, 0.0f);

    if(!all(isfinite(worldPosition)) || !all(isfinite(originalUV)) ||
       !all(isfinite(worldNormal)) || !all(isfinite(worldViewDirection)))
    {
        return false;
    }

    if(!all(isfinite(positionDx)) || !all(isfinite(positionDy)) ||
       !all(isfinite(uvDx)) || !all(isfinite(uvDy)))
    {
        return false;
    }

    // Normalize the UV Jacobian before testing its relative conditioning.
    // Signed determinants preserve mirrored UV charts.
    const float minimumScale = 1.0e-15f;
    const float conditioningTolerance = 1.0e-6f;
    float uvScale = max(max(abs(uvDx.x), abs(uvDx.y)), max(abs(uvDy.x), abs(uvDy.y)));

    if(uvScale < minimumScale)
    {
        return false;
    }

    float2 Ux = uvDx / uvScale;
    float2 Uy = uvDy / uvScale;
    float D = Ux.x * Uy.y - Ux.y * Uy.x;
    float uvMagnitudeSquared = dot(Ux, Ux) + dot(Uy, Uy);

    if(abs(D) <= conditioningTolerance * uvMagnitudeSquared)
    {
        return false;
    }

    float3 Pu = ((positionDx * Uy.y - positionDy * Ux.y) / D) / uvScale;
    float3 Pv = ((positionDy * Ux.x - positionDx * Uy.x) / D) / uvScale;

    if(!all(isfinite(Pu)) || !all(isfinite(Pv)))
    {
        return false;
    }

    // Scale both axes equally so the Gram matrix cannot overflow from world units.
    float metricScale = max(max(abs(Pu.x), abs(Pu.y)), abs(Pu.z));
    metricScale = max(metricScale, max(max(abs(Pv.x), abs(Pv.y)), abs(Pv.z)));

    if(metricScale < minimumScale)
    {
        return false;
    }

    float3 U = Pu / metricScale;
    float3 V = Pv / metricScale;
    float g00 = dot(U, U);
    float g01 = dot(U, V);
    float g11 = dot(V, V);

    float determinant = g00 * g11 - g01 * g01;
    float trace = g00 + g11;

    if(!isfinite(determinant) || determinant <= conditioningTolerance * trace * trace)
    {
        return false;
    }

    float3 planeNormal = cross(U, V);
    float normalLengthSquared = dot(planeNormal, planeNormal);
    float referenceScale = max(max(abs(worldNormal.x), abs(worldNormal.y)), abs(worldNormal.z));

    if(!isfinite(normalLengthSquared) || normalLengthSquared < minimumScale || referenceScale < minimumScale)
    {
        return false;
    }

    float3 N = planeNormal * rsqrt(normalLengthSquared);
    float orientation = dot(N, worldNormal / referenceScale);

    if(!isfinite(orientation) || abs(orientation) <= conditioningTolerance)
    {
        return false;
    }

    N *= orientation < 0.0f ? -1.0f : 1.0f;
    float viewLengthSquared = dot(worldViewDirection, worldViewDirection);

    if(!isfinite(viewLengthSquared) || viewLengthSquared < minimumScale)
    {
        return false;
    }

    float3 viewDirection = worldViewDirection * rsqrt(viewLengthSquared);
    float Vn = dot(viewDirection, N);

    // Reject back faces and angles within approximately 2.9 degrees of grazing.
    if(!isfinite(Vn) || Vn <= 0.05f)
    {
        return false;
    }

    float3 W = viewDirection / Vn - N;
    float2 rhs = float2(dot(U, W), dot(V, W));
    float2 q = float2(g11 * rhs.x - g01 * rhs.y, g00 * rhs.y - g01 * rhs.x);
    q = (q / determinant) / metricScale;

    if(!all(isfinite(W)) || !all(isfinite(rhs)) || !all(isfinite(q)))
    {
        return false;
    }

    // q is UV travel per world-space height; future POM uses originalUV + q * height.
    uvPerWorldHeight = q;

    return true;
}

bool TryIntersectPOMHeightfield(
    GPUMaterial material,
    float2 originalUV,
    float2 originalUVDx,
    float2 originalUVDy,
    bool metricValid,
    float2 uvPerWorldHeight,
    float viewNormalCosine,
    out float2 parallaxUV)
{
    parallaxUV = originalUV;

    // Geometry fallback requires a separate representation-selection contract.
    if(material.surfaceDetailMode != SurfaceDetailModeParallaxOcclusion ||
       material.heightTextureIndex == 0xFFFFFFFFu || !metricValid)
    {
        return false;
    }

    if(!all(isfinite(originalUV)) || !all(isfinite(originalUVDx)) ||
       !all(isfinite(originalUVDy)) || !all(isfinite(uvPerWorldHeight)))
    {
        return false;
    }

    if(!isfinite(material.heightScale) || !isfinite(material.heightBias) ||
       !isfinite(viewNormalCosine) || viewNormalCosine <= 0.05f || viewNormalCosine > 1.0f)
    {
        return false;
    }

    // Heights are world distances. Do not normalize away the reference-plane bias.
    const float minimumHeightRange = 1.0e-6f;
    float heightEnd = material.heightBias + material.heightScale;
    float hMin = min(material.heightBias, heightEnd);
    float hMax = max(material.heightBias, heightEnd);
    float heightRange = hMax - hMin;

    if(!isfinite(heightEnd) || !isfinite(heightRange) ||
       abs(material.heightScale) <= minimumHeightRange || heightRange <= minimumHeightRange)
    {
        return false;
    }

    // Bound the entire signed interval, including bias, without clipping the ray.
    // A one-UV-unit Euclidean radius is a conservative initial sampling limit.
    const float maximumUVDisplacement = 1.0f;
    float2 minOffset = uvPerWorldHeight * hMin;
    float2 maxOffset = uvPerWorldHeight * hMax;

    if(!all(isfinite(minOffset)) || !all(isfinite(maxOffset)))
    {
        return false;
    }

    float minTravelSquared = dot(minOffset, minOffset);
    float maxTravelSquared = dot(maxOffset, maxOffset);

    if(!isfinite(minTravelSquared) || !isfinite(maxTravelSquared) ||
       max(minTravelSquared, maxTravelSquared) > maximumUVDisplacement * maximumUVDisplacement)
    {
        return false;
    }

    if(!all(isfinite(originalUV + minOffset)) || !all(isfinite(originalUV + maxOffset)))
    {
        return false;
    }

    float projectedTravel = length(maxOffset - minOffset);
    float obliqueness = 1.0f - saturate(viewNormalCosine);
    uint stepCount = 8u + (uint)ceil(24.0f * max(saturate(projectedTravel), obliqueness));
    stepCount = min(stepCount, 32u);

    Texture2D<float4> heightTexture = ResourceDescriptorHeap[
        NonUniformResourceIndex(material.heightTextureIndex)];
    float sampledHeight = heightTexture.SampleGrad(
        sampler0, originalUV + maxOffset, originalUVDx, originalUVDy).r;

    // The interval assumes a normalized height channel; other ranges are unsupported.
    if(!isfinite(sampledHeight) || sampledHeight < 0.0f || sampledHeight > 1.0f)
    {
        return false;
    }

    float surfaceHeight = material.heightScale * sampledHeight + material.heightBias;

    if(!isfinite(surfaceHeight) || surfaceHeight > hMax)
    {
        return false;
    }

    if(surfaceHeight == hMax)
    {
        parallaxUV = originalUV + maxOffset;
        return true;
    }

    float upperHeight = hMax;
    float lowerHeight = hMin;
    bool bracketFound = false;

    [loop]
    for(uint step = 1u; step <= 32u; ++step)
    {
        if(step > stepCount)
        {
            break;
        }

        float rayHeight = lerp(hMax, hMin, float(step) / float(stepCount));
        float2 rayUV = originalUV + uvPerWorldHeight * rayHeight;
        if(!isfinite(rayHeight) || !all(isfinite(rayUV)))
        {
            return false;
        }

        sampledHeight = heightTexture.SampleGrad(sampler0, rayUV, originalUVDx, originalUVDy).r;

        if(!isfinite(sampledHeight) || sampledHeight < 0.0f || sampledHeight > 1.0f)
        {
            return false;
        }

        surfaceHeight = material.heightScale * sampledHeight + material.heightBias;

        if(!isfinite(surfaceHeight))
        {
            return false;
        }

        if(rayHeight <= surfaceHeight)
        {
            lowerHeight = rayHeight;
            bracketFound = true;
            break;
        }

        upperHeight = rayHeight;
    }

    if(!bracketFound)
    {
        return false;
    }

    [unroll]
    for(uint refinement = 0u; refinement < 4u; ++refinement)
    {
        float rayHeight = lerp(lowerHeight, upperHeight, 0.5f);
        float2 rayUV = originalUV + uvPerWorldHeight * rayHeight;
        if(!isfinite(rayHeight) || !all(isfinite(rayUV)))
        {
            return false;
        }

        sampledHeight = heightTexture.SampleGrad(sampler0, rayUV, originalUVDx, originalUVDy).r;

        if(!isfinite(sampledHeight) || sampledHeight < 0.0f || sampledHeight > 1.0f)
        {
            return false;
        }

        surfaceHeight = material.heightScale * sampledHeight + material.heightBias;

        if(!isfinite(surfaceHeight))
        {
            return false;
        }

        if(rayHeight <= surfaceHeight)
        {
            lowerHeight = rayHeight;
        }
        else
        {
            upperHeight = rayHeight;
        }
    }

    float2 refinedUV = originalUV + uvPerWorldHeight * lerp(lowerHeight, upperHeight, 0.5f);

    if(!all(isfinite(refinedUV)))
    {
        return false;
    }

    parallaxUV = refinedUV;

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

PSOutput PSMain(VSOutput input, uint primitiveID : SV_PrimitiveID)
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

    // Unsupported modes and invalid POM inputs retain the interpolated UV.
    float2 resolvedMaterialUV = input.uv;

    // The physical metric uses the geometric plane, independently of normal mapping.
    float3 worldViewDirection;
    bool hasWorldViewDirection = TryGetWorldViewDirection(input.worldPosition, worldViewDirection);

    float2 uvPerWorldHeight;
    bool hasPhysicalHeightUVMetric = TryGetPhysicalHeightUVMetric(
        input.worldPosition,
        input.uv,
        input.worldNormal,
        worldViewDirection,
        positionDx,
        positionDy,
        uvDx,
        uvDy,
        uvPerWorldHeight);

    if(material.surfaceDetailMode == SurfaceDetailModeParallaxOcclusion &&
       hasWorldViewDirection && hasPhysicalHeightUVMetric)
    {
        // Normalized derivative axes avoid overflow from world-space magnitudes.
        float3 planeNormal = normalize(cross(normalize(positionDx), normalize(positionDy)));
        planeNormal *= dot(planeNormal, input.worldNormal) < 0.0f ? -1.0f : 1.0f;
        float viewNormalCosine = dot(worldViewDirection, planeNormal);
        float2 parallaxUV;

        if(TryIntersectPOMHeightfield(
            material,
            input.uv,
            uvDx,
            uvDy,
            hasPhysicalHeightUVMetric,
            uvPerWorldHeight,
            viewNormalCosine,
            parallaxUV))
        {
            resolvedMaterialUV = parallaxUV;
        }
    }

    Texture2D<float4> baseColorTexture =
        ResourceDescriptorHeap[
            textureIndex
        ];

    float3 sampledBaseColor =
        SampleMaterialTexture(
            baseColorTexture,
            sampler0,
            resolvedMaterialUV
        ).rgb;

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
        float3 sampledARM = SampleMaterialTexture(armTexture, sampler0, resolvedMaterialUV).rgb;
        armRoughnessMetallic = sampledARM.gb;
        materialAO = saturate(sampledARM.r);
    }
    else if(material.aoTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> aoTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.aoTextureIndex)];
        materialAO = saturate(SampleMaterialTexture(aoTexture, sampler0, resolvedMaterialUV).r);
    }

    float finalRoughness = roughness;
    if(useARM)
    {
        finalRoughness = saturate(roughness * armRoughnessMetallic.x);
    }
    else if(roughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[roughnessTextureIndex];
        float sampledRoughness = SampleMaterialTexture(roughnessTexture, sampler0, resolvedMaterialUV).r;
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
        float2 normalXY = SampleMaterialTexture(normalTexture, sampler0, resolvedMaterialUV).rg * 2.0f - 1.0f;
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

    float effectiveMetallic = 0.0f;
    if(material.model == 0u)
    {
        effectiveMetallic = saturate(material.metallic);
        if(useARM)
        {
            effectiveMetallic = saturate(effectiveMetallic * armRoughnessMetallic.y);
        }
        else if(material.metallicTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> metallicTexture = ResourceDescriptorHeap[
                NonUniformResourceIndex(material.metallicTextureIndex)];
            float sampledMetallic = SampleMaterialTexture(metallicTexture, sampler0, resolvedMaterialUV).r;
            effectiveMetallic = saturate(effectiveMetallic * sampledMetallic);
        }
    }

    output.geometricNormal = float4(normalize(input.worldNormal), effectiveMetallic);

    output.motion = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.identity = rasterInstance == 0xFFFFFFFFu ? uint2(0xFFFFFFFFu, 0xFFFFFFFFu) :
        uint2(rasterInstance + 1u, rasterPrimitiveBase + primitiveID);

    if(all(isfinite(input.currentClipPosition)) &&
       all(isfinite(input.previousClipPosition)) &&
       all(isfinite(input.viewDepths)) &&
       abs(input.currentClipPosition.w) > 1.0e-6f &&
       abs(input.previousClipPosition.w) > 1.0e-6f)
    {
        float2 currentNDC =
            input.currentClipPosition.xy / input.currentClipPosition.w;
        float2 previousNDC =
            input.previousClipPosition.xy / input.previousClipPosition.w;

        float2 currentUV =
            currentNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        float2 previousUV =
            previousNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

        float2 motion = currentUV - previousUV;

        float4 motionVector = float4(
            motion,
            input.viewDepths.y - input.viewDepths.x,
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
