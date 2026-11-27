// Isolated fixed-material preview. No production shader or scene table changes.
#include "MaterialLighting.hlsli"
#include "SurfaceDetail.hlsli"

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
// The fields of the previewed GPUMaterial its Legacy shading reads (MaterialPreviewRenderer's
// PreviewMaterialConstants): the full OpenPBR record no longer fits the 64-DWORD root signature beside the geometry
// constants, so the renderer sends this subset and PreviewMaterial rebuilds a GPUMaterial from it.
struct PreviewMaterialConstants
{
    float3 baseColor;
    uint textureIndex;

    float roughness;
    float metallic;
    float baseWeight;
    float diffuseRoughness;

    float dielectricF0;
    uint roughnessTextureIndex;
    uint metallicTextureIndex;
    uint normalTextureIndex;

    uint armTextureIndex;
    uint preferARM;
    uint aoTextureIndex;
    uint surfaceDetailMode;

    uint heightTextureIndex;
    float heightScale;
    float heightBias;
    uint reserved;
};

ConstantBuffer<PreviewMaterialConstants> previewMaterialConstants : register(b2);

GPUMaterial PreviewMaterial()
{
    GPUMaterial material = (GPUMaterial)0;
    material.diffuseOcclusion = 1.0f;
    material.baseColor = previewMaterialConstants.baseColor;
    material.textureIndex = previewMaterialConstants.textureIndex;
    material.roughness = previewMaterialConstants.roughness;
    material.metallic = previewMaterialConstants.metallic;
    material.baseWeight = previewMaterialConstants.baseWeight;
    material.diffuseRoughness = previewMaterialConstants.diffuseRoughness;
    material.dielectricF0 = previewMaterialConstants.dielectricF0;
    material.roughnessTextureIndex = previewMaterialConstants.roughnessTextureIndex;
    material.metallicTextureIndex = previewMaterialConstants.metallicTextureIndex;
    material.normalTextureIndex = previewMaterialConstants.normalTextureIndex;
    material.armTextureIndex = previewMaterialConstants.armTextureIndex;
    material.preferARM = previewMaterialConstants.preferARM;
    material.aoTextureIndex = previewMaterialConstants.aoTextureIndex;
    material.surfaceDetailMode = previewMaterialConstants.surfaceDetailMode;
    material.heightTextureIndex = previewMaterialConstants.heightTextureIndex;
    material.heightScale = previewMaterialConstants.heightScale;
    material.heightBias = previewMaterialConstants.heightBias;
    material.specularWeight = 1.0f;
    material.specularColor = float3(1.0f, 1.0f, 1.0f);
    return material;
}

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

GBufferOutput SpherePS(SphereOutput input)
{
    GBufferOutput output;

    // Match production: derivatives precede material-dependent control flow.
    float3 positionDx = ddx(input.worldPosition);
    float3 positionDy = ddy(input.worldPosition);
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);
    const GPUMaterial previewMaterial = PreviewMaterial();
    GPUMaterial material = previewMaterial;

    // The same parallax occlusion mapping as the scene's G-buffer pass (SurfaceDetail.hlsli).
    SurfaceDetailResult detail;
    detail.uv = input.uv;
    detail.height = 0.0f;
    detail.viewCosine = 0.0f;
    detail.fade = 0.0f;
    detail.displaced = false;
    detail.explicitGradients = false;
    detail.uvDx = uvDx;
    detail.uvDy = uvDy;

    if(SurfaceDetailUsesParallax(material))
    {
        float3 toCamera = previewCameraPosition.xyz - input.worldPosition;
        float distanceSquared = dot(toCamera, toCamera);

        if(distanceSquared > 1.0e-8f)
        {
            detail = SurfaceDetailResolve(material, sampler0, input.uv, uvDx, uvDy, positionDx, positionDy,
                input.normal, input.tangent, toCamera * rsqrt(distanceSquared), 0.0f);
        }
    }

    float3 baseColor = previewMaterial.baseColor;
    float roughness = previewMaterial.roughness;
    float metallic = saturate(previewMaterial.metallic);
    float ao = 1.0f;

    if(previewMaterial.textureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.textureIndex];
        baseColor *= SurfaceDetailSample(texture, sampler0, detail, 0.0f).rgb;
    }

    bool useARM = previewMaterial.armTextureIndex != 0xFFFFFFFFu && previewMaterial.preferARM != 0u;

    if(useARM)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.armTextureIndex];
        float3 arm = SurfaceDetailSample(texture, sampler0, detail, 0.0f).rgb;
        ao = saturate(arm.r);
        roughness = saturate(roughness * arm.g);
        metallic = saturate(metallic * arm.b);
    }
    else
    {
        if(previewMaterial.roughnessTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.roughnessTextureIndex];
            roughness = saturate(roughness * SurfaceDetailSample(texture, sampler0, detail, 0.0f).r);
        }

        if(previewMaterial.metallicTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.metallicTextureIndex];
            metallic = saturate(metallic * SurfaceDetailSample(texture, sampler0, detail, 0.0f).r);
        }

        if(previewMaterial.aoTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.aoTextureIndex];
            ao = saturate(SurfaceDetailSample(texture, sampler0, detail, 0.0f).r);
        }
    }

    float3 N = normalize(input.normal);
    float3 mappedNormal = N;

    if(previewMaterial.normalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[previewMaterial.normalTextureIndex];
        float2 xy = SurfaceDetailSample(texture, sampler0, detail, 0.0f).rg * 2.0f - 1.0f;
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
    GPUMaterial material = PreviewMaterial();
    float4 sampled = materials.Load(int3(int2(position.xy), 0));
    material.baseColor = sampled.rgb;
    material.roughness = sampled.a;
    material.metallic = geometry.Load(int3(int2(position.xy), 0)).a;
    float2 viewAlbedo = brdf.SampleLevel(sampler0, float2(saturate(dot(N, V)), material.roughness), 0);
    float averageAlbedo = average.SampleLevel(sampler0, float2(material.roughness, 0.5f), 0);
    float3 radiance = lerp(material.baseColor, LegacyMaterialF0(material, material.baseColor, material.metallic),
        material.metallic) * 0.05f * surface.a; // AO affects ambient only, never studio direct lighting.
    const float3 lights[3] = {
        normalize(float3(-3.0f, 4.0f, -4.0f)),
        normalize(float3(4.0f, 1.0f, -1.0f)),
        normalize(float3(1.0f, 3.0f, 4.0f))};
    const float strengths[3] = {3.5f, 1.2f, 2.0f};

    for(uint index = 0; index < 3; ++index)
    {
        float NdotL = saturate(dot(N, lights[index]));
        float2 lightAlbedo = brdf.SampleLevel(sampler0, float2(NdotL, material.roughness), 0);
        // The preview uses Legacy's metallic-roughness BRDF for every OpenPBR material (coat, fuzz and thin film are
        // path-tracer lobes; OpenPBR report §9.1).
        float3 directBRDF = EvaluateMetallicRoughnessDirectBRDF(
            material, material.baseColor, N, lights[index], V,
            lightAlbedo.x + lightAlbedo.y, viewAlbedo.x + viewAlbedo.y, averageAlbedo);

        radiance += directBRDF * NdotL * strengths[index];
    }

    return float4(radiance, 1.0f);
}
