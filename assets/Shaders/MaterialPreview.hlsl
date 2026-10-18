// Isolated fixed-material preview. No production shader or scene table changes.
#include "MaterialLighting.hlsli"

cbuffer GeometryConstants : register(b0)
{
    row_major float4x4 previewViewProjection;
    row_major float4x4 previewModel;
    float4 previewCameraPosition;
};

cbuffer LightingConstants : register(b1)
{
    uint materialIndex;
    uint normalIndex;
    uint positionIndex;
    uint brdfIndex;
    uint averageIndex;
    float3 cameraPosition;
    uint geometricIndex;
};

SamplerState sampler0 : register(s0);
ConstantBuffer<GPUMaterial> previewMaterial : register(b2);

struct SphereInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct SphereOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};

SphereOutput SphereVS(SphereInput input)
{
    SphereOutput output;
    float4 worldPosition = mul(previewModel, float4(input.position, 1.0f));
    output.position = mul(previewViewProjection, worldPosition);
    output.worldPosition = worldPosition.xyz;
    output.normal = mul((float3x3)previewModel, input.normal);
    output.tangent = float4(mul((float3x3)previewModel, input.tangent.xyz), input.tangent.w);
    output.uv = input.uv;
    return output;
}

struct GBufferOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 position : SV_Target2;
    float4 geometricNormal : SV_Target3;
};

// Adapted from Triangle.hlsl: TryGetWorldViewDirection, TryGetPhysicalHeightUVMetric,
// and TryIntersectPOMHeightfield. Only camera recovery differs: preview b0 supplies its position.
bool TryGetWorldViewDirection(
    float3 worldPosition,
    out float3 worldViewDirection)
{
    worldViewDirection = float3(0.0f, 0.0f, 0.0f);
    float3 cameraWorld = previewCameraPosition.xyz;

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

GBufferOutput SpherePS(SphereOutput input)
{
    GBufferOutput output;

    // Match production: derivatives precede material-dependent control flow.
    float3 positionDx = ddx(input.worldPosition);
    float3 positionDy = ddy(input.worldPosition);
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);
    GPUMaterial material = previewMaterial;

    // Unsupported modes and invalid POM inputs retain the interpolated UV.
    float2 resolvedMaterialUV = input.uv;

    // The physical metric uses the geometric plane, independently of normal mapping.
    float3 worldViewDirection;
    bool hasWorldViewDirection = TryGetWorldViewDirection(input.worldPosition, worldViewDirection);

    float2 uvPerWorldHeight;
    bool hasPhysicalHeightUVMetric = TryGetPhysicalHeightUVMetric(
        input.worldPosition,
        input.uv,
        input.normal,
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
        planeNormal *= dot(planeNormal, input.normal) < 0.0f ? -1.0f : 1.0f;
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


    float3 baseColor = previewMaterial.baseColor;
    float roughness = previewMaterial.roughness;
    float metallic = previewMaterial.model == 0u ? saturate(previewMaterial.metallic) : 0.0f;
    float ao = 1.0f;

    if(previewMaterial.textureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.textureIndex];
        baseColor *= texture.Sample(sampler0, resolvedMaterialUV).rgb;
    }

    bool useARM = previewMaterial.armTextureIndex != 0xFFFFFFFFu && previewMaterial.preferARM != 0u;

    if(useARM)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.armTextureIndex];
        float3 arm = texture.Sample(sampler0, resolvedMaterialUV).rgb;
        ao = saturate(arm.r);
        roughness = saturate(roughness * arm.g);
        metallic = saturate(metallic * arm.b);
    }
    else
    {
        if(previewMaterial.roughnessTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.roughnessTextureIndex];
            roughness = saturate(roughness * texture.Sample(sampler0, resolvedMaterialUV).r);
        }

        if(previewMaterial.metallicTextureIndex != 0xFFFFFFFFu && previewMaterial.model == 0u)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.metallicTextureIndex];
            metallic = saturate(metallic * texture.Sample(sampler0, resolvedMaterialUV).r);
        }

        if(previewMaterial.aoTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.aoTextureIndex];
            ao = saturate(texture.Sample(sampler0, resolvedMaterialUV).r);
        }
    }

    float3 N = normalize(input.normal);
    float3 mappedNormal = N;

    if(previewMaterial.normalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.normalTextureIndex];
        float2 xy = texture.Sample(sampler0, resolvedMaterialUV).rg * 2.0f - 1.0f;
        float3 sampledNormal = float3(xy, sqrt(saturate(1.0f - dot(xy, xy))));
        float3 tangent = input.tangent.xyz - N * dot(N, input.tangent.xyz);
        float tangentLength = dot(tangent, tangent);

        if(all(isfinite(N)) && all(isfinite(tangent)) && isfinite(tangentLength) && tangentLength > 1.0e-8f)
        {
            if(all(isfinite(sampledNormal)) && isfinite(input.tangent.w) && abs(input.tangent.w) > 1.0e-8f)
            {
                float3 T = normalize(tangent);
                float3 B = cross(N, T) * input.tangent.w;
                float3 candidate = T * sampledNormal.x + B * sampledNormal.y + N * sampledNormal.z;
                float lengthSquared = dot(candidate, candidate);

                if(all(isfinite(candidate)) && isfinite(lengthSquared) && lengthSquared > 1.0e-8f)
                {
                    mappedNormal = normalize(candidate);
                }
            }
        }
    }

    output.material = float4(baseColor, roughness);
    output.surface = float4(mappedNormal, ao);
    // Private preview attachment: world position, not scene motion/history.
    output.position = float4(input.worldPosition, 1.0f);
    output.geometricNormal = float4(N, metallic);
    return output;
}

float4 LightingVS(uint vertexID : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 LightingPS(float4 position : SV_POSITION) : SV_Target0
{
    Texture2D<float4> positions = ResourceDescriptorHeap[positionIndex];
    float4 world = positions.Load(int3(int2(position.xy), 0));

    if(world.w == 0.0f)
    {
        return float4(0.018f, 0.018f, 0.018f, 1.0f);
    }

    Texture2D<float4> normals = ResourceDescriptorHeap[normalIndex];
    Texture2D<float4> materials = ResourceDescriptorHeap[materialIndex];
    Texture2D<float4> geometry = ResourceDescriptorHeap[geometricIndex];
    Texture2D<float2> brdf = ResourceDescriptorHeap[brdfIndex];
    Texture2D<float> average = ResourceDescriptorHeap[averageIndex];
    float4 surface = normals.Load(int3(int2(position.xy), 0));
    float3 N = normalize(surface.xyz);
    float3 V = normalize(cameraPosition - world.xyz);
    GPUMaterial material = previewMaterial;
    float4 sampled = materials.Load(int3(int2(position.xy), 0));
    material.baseColor = sampled.rgb;
    material.roughness = sampled.a;
    material.metallic = geometry.Load(int3(int2(position.xy), 0)).a;
    float2 viewAlbedo = brdf.SampleLevel(sampler0, float2(saturate(dot(N, V)), material.roughness), 0);
    float averageAlbedo = average.SampleLevel(sampler0, float2(material.roughness, 0.5f), 0);
    float3 radiance = (material.model == 2u ? material.averageConductorFresnel : material.baseColor) * 0.05f * surface.a; // AO affects ambient only, never studio direct lighting.
    const float3 lights[3] = {
        normalize(float3(-3.0f, 4.0f, -4.0f)),
        normalize(float3(4.0f, 1.0f, -1.0f)),
        normalize(float3(1.0f, 3.0f, 4.0f))};
    const float strengths[3] = {3.5f, 1.2f, 2.0f};

    for(uint index = 0; index < 3; ++index)
    {
        float NdotL = saturate(dot(N, lights[index]));
        float2 lightAlbedo = brdf.SampleLevel(sampler0, float2(NdotL, material.roughness), 0);
        float3 directBRDF;

        if(material.model == 0u)
        {
            directBRDF = EvaluateMetallicRoughnessDirectBRDF(
                material, material.baseColor, N, lights[index], V,
                lightAlbedo.x + lightAlbedo.y, viewAlbedo.x + viewAlbedo.y, averageAlbedo);
        }
        else if(material.model == 1u)
        {
            directBRDF = EvaluateDielectricDirectBRDF(
                material, material.baseColor, N, lights[index], V,
                lightAlbedo.x + lightAlbedo.y, viewAlbedo.x + viewAlbedo.y, averageAlbedo);
        }
        else
        {
            directBRDF = EvaluateConductorDirectBRDF(
                material, N, lights[index], V,
                lightAlbedo.x + lightAlbedo.y, viewAlbedo.x + viewAlbedo.y, averageAlbedo);
        }

        radiance += directBRDF * NdotL * strengths[index];
    }

    return float4(radiance, 1.0f);
}
