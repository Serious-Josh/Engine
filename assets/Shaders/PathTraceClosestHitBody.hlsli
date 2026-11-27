// The closest-hit evaluation of a radiance ray: the hit's surface (geometric and shading normal, UV) and material,
// written to payload. Shared by PathTraceClosestHit (DXR) and PathTraceTraceRadianceInline (the ReSTIR DI direct
// compute pass's inline RayQuery radiance ray), so both reconstruct a hit identically from one source. The includer
// defines how the hit is read, then includes this file inside its function body with payload in scope:
//   PATH_TRACE_HIT_INSTANCE_ID: the hit instance's InstanceID (its RayTracingInstanceData index)
//   PATH_TRACE_HIT_PRIMITIVE_INDEX: the hit triangle's index in its geometry
//   PATH_TRACE_HIT_BARYCENTRICS: the hit's barycentrics (float2)
//   PATH_TRACE_HIT_WORLD_TO_OBJECT: the hit instance's world-to-object transform (float3x4)
//   PATH_TRACE_HIT_OBJECT_TO_WORLD: the hit instance's object-to-world transform (float3x4)
//   PATH_TRACE_HIT_WORLD_RAY_DIRECTION: the ray's world direction
//   PATH_TRACE_HIT_T: the hit's ray parameter
// PathTraceClosestHit defines them as the DXR intrinsics, so its compiled code is exactly what it was before the body
// moved here.
    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[instanceDataIndex];
    RayTracingInstanceData instance = instances[PATH_TRACE_HIT_INSTANCE_ID];
    instance.materialIndex = ResolveHitMaterial(instance, PATH_TRACE_HIT_PRIMITIVE_INDEX);

    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    uint3 indices = PathTraceTriangleIndices(instance, PATH_TRACE_HIT_PRIMITIVE_INDEX);
    float3 barycentrics = float3(1.0f - PATH_TRACE_HIT_BARYCENTRICS.x - PATH_TRACE_HIT_BARYCENTRICS.y,
        PATH_TRACE_HIT_BARYCENTRICS.x, PATH_TRACE_HIT_BARYCENTRICS.y);

    // Normals use the inverse transpose of object-to-world (row vector times world-to-object).
    float3x3 normalTransform = (float3x3)PATH_TRACE_HIT_WORLD_TO_OBJECT;
    float3 position0 = PathTraceLoadPosition(vertexBuffer, indices.x, instance.vertexStride);
    float3 position1 = PathTraceLoadPosition(vertexBuffer, indices.y, instance.vertexStride);
    float3 position2 = PathTraceLoadPosition(vertexBuffer, indices.z, instance.vertexStride);
    float3 objectGeometricNormal = cross(position1 - position0, position2 - position0);
    float3 geometricNormal = mul(objectGeometricNormal, normalTransform);
    float geometricLengthSquared = dot(geometricNormal, geometricNormal);

    float3 objectNormal = PathTraceLoadNormal(vertexBuffer, indices.x, instance) * barycentrics.x +
        PathTraceLoadNormal(vertexBuffer, indices.y, instance) * barycentrics.y +
        PathTraceLoadNormal(vertexBuffer, indices.z, instance) * barycentrics.z;
    float3 worldNormal = mul(objectNormal, normalTransform);
    float worldNormalLengthSquared = dot(worldNormal, worldNormal);

    float2 uv = PathTraceLoadUV(vertexBuffer, indices.x, instance) * barycentrics.x +
        PathTraceLoadUV(vertexBuffer, indices.y, instance) * barycentrics.y +
        PathTraceLoadUV(vertexBuffer, indices.z, instance) * barycentrics.z;

    if(!(geometricLengthSquared > 1.0e-30f) || !isfinite(geometricLengthSquared))
    {
        geometricNormal = -PATH_TRACE_HIT_WORLD_RAY_DIRECTION;
    }
    else
    {
        geometricNormal *= rsqrt(geometricLengthSquared);
    }

    if(!(worldNormalLengthSquared > 1.0e-16f) || !isfinite(worldNormalLengthSquared))
    {
        worldNormal = geometricNormal;
    }
    else
    {
        worldNormal *= rsqrt(worldNormalLengthSquared);
    }

    // Interpolated normals keep the triangle's orientation so two-sided flipping stays consistent.
    if(dot(worldNormal, geometricNormal) < 0.0f)
    {
        geometricNormal = -geometricNormal;
    }

    float3 shadingNormal = worldNormal;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);

    // OpenPBR layers: a coat's smooth normal is the interpolated normal, and anisotropy needs the surface tangent. Both
    // stay OPENPBR_PACKED_VECTOR_NONE for materials without those lobes.
    bool materialValid = instance.materialIndex < materialCount;
    uint lobeFlags = materialValid ? materials[instance.materialIndex].lobeFlags : 0u;
    bool hasTangents = instance.tangentOffset != 0xFFFFFFFFu;
    bool normalMapped = materialValid && hasTangents &&
        materials[instance.materialIndex].normalTextureIndex != 0xFFFFFFFFu;
    bool anisotropic = materialValid && hasTangents && (lobeFlags & GPU_MATERIAL_LOBE_ANISOTROPY) != 0u;

#if defined(PATH_TRACE_RESTIR_PT_LIBRARY) && defined(PATH_TRACE_SURFACE_DETAIL_RELIEF)
    // POM D2a (the relief kernel tiers): parallax occlusion mapping at the hit, so reflections and bounce light see the
    // same relief as the raster primary. The march follows this ray into the material's height field from the hit's UV,
    // with the triangle's own edges for the UV mapping and the ray cone (the camera pixel's angle times the distance the
    // ray travelled) for the height mip; every later texture read and the normal map use the UV it finds. The hit point
    // itself stays on the mesh. reliefClipped: with silhouette POM, the ray leaves the mesh before it meets the relief
    // (SurfaceDetailSilhouette.hlsli); an inline tracer then continues past the hit (RestirPtTraceInline).
    bool reliefClipped = false;

    if(materialValid && SurfaceDetailUsesParallax(materials[instance.materialIndex]))
    {
        GPUMaterial reliefMaterial = materials[instance.materialIndex];
        float3x3 reliefObjectToWorld = (float3x3)PATH_TRACE_HIT_OBJECT_TO_WORLD;
        float3 reliefEdge1 = mul(reliefObjectToWorld, position1 - position0);
        float3 reliefEdge2 = mul(reliefObjectToWorld, position2 - position0);
        float2 reliefUV0 = PathTraceLoadUV(vertexBuffer, indices.x, instance);
        float2 reliefUVEdge1 = PathTraceLoadUV(vertexBuffer, indices.y, instance) - reliefUV0;
        float2 reliefUVEdge2 = PathTraceLoadUV(vertexBuffer, indices.z, instance) - reliefUV0;
        float4 reliefTangent = float4(0.0f, 0.0f, 0.0f, 1.0f);

        if(hasTangents)
        {
            float4 objectReliefTangent = PathTraceLoadTangent(vertexBuffer, indices.x, instance) * barycentrics.x +
                PathTraceLoadTangent(vertexBuffer, indices.y, instance) * barycentrics.y +
                PathTraceLoadTangent(vertexBuffer, indices.z, instance) * barycentrics.z;
            reliefTangent = float4(mul(reliefObjectToWorld, objectReliefTangent.xyz), objectReliefTangent.w);
        }

        float2 uvPerHeight;
        float viewCosine;

        if(SurfaceDetailUVPerHeight(worldNormal, reliefTangent, -PATH_TRACE_HIT_WORLD_RAY_DIRECTION, reliefEdge1,
            reliefEdge2, reliefUVEdge1, reliefUVEdge2, uvPerHeight, viewCosine))
        {
            // The ray cone's width at the hit, in UV: its world width over the triangle's world length per UV.
            float coneWidth = PATH_TRACE_HIT_T * PathTraceCameraPixelAngle(PATH_TRACE_HIT_WORLD_RAY_DIRECTION);
            float uvArea = abs(reliefUVEdge1.x * reliefUVEdge2.y - reliefUVEdge1.y * reliefUVEdge2.x);
            float worldArea = length(cross(reliefEdge1, reliefEdge2));
            float uvFootprint = coneWidth * sqrt(uvArea / max(worldArea, 1.0e-20f));
            SurfaceDetailResult relief = SurfaceDetailMarch(reliefMaterial, materialSampler, uv,
                float2(uvFootprint, 0.0f), float2(0.0f, uvFootprint), uvPerHeight, viewCosine, 0.0f);

            if(relief.displaced && all(isfinite(relief.uv)))
            {
                reliefClipped = (reliefMaterial.lobeFlags & GPU_MATERIAL_PARALLAX_SILHOUETTE) != 0u &&
                    SurfaceDetailLeavesSilhouette(instance, PATH_TRACE_HIT_PRIMITIVE_INDEX, relief.uv);
                uv = relief.uv;
            }
        }
    }
#endif
    uint packedSmoothNormal = OPENPBR_PACKED_VECTOR_NONE;
    uint packedTangent = OPENPBR_PACKED_VECTOR_NONE;

    if((lobeFlags & GPU_MATERIAL_LOBE_COAT) != 0u)
    {
        packedSmoothNormal = OpenPBRPackUnitVector(worldNormal);
    }

    // Tangent-space normal mapping, identical to the RTGI/G-buffer convention, and the anisotropy tangent.
    if(normalMapped || anisotropic)
    {
        GPUMaterial material = materials[instance.materialIndex];
        float4 objectTangent = PathTraceLoadTangent(vertexBuffer, indices.x, instance) * barycentrics.x +
            PathTraceLoadTangent(vertexBuffer, indices.y, instance) * barycentrics.y +
            PathTraceLoadTangent(vertexBuffer, indices.z, instance) * barycentrics.z;
        float3 tangent = mul((float3x3)PATH_TRACE_HIT_OBJECT_TO_WORLD, objectTangent.xyz);
        tangent -= worldNormal * dot(worldNormal, tangent);
        float tangentLengthSquared = dot(tangent, tangent);
        bool tangentValid = all(isfinite(tangent)) && isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f;

        if(anisotropic && tangentValid)
        {
            packedTangent = OpenPBRPackUnitVector(tangent * rsqrt(tangentLengthSquared));
        }

        if(normalMapped)
        {
            float instanceDeterminant = determinant((float3x3)PATH_TRACE_HIT_OBJECT_TO_WORLD);
            float handedness = objectTangent.w * (instanceDeterminant < 0.0f ? -1.0f : 1.0f);

            Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.normalTextureIndex)];
            float2 normalXY = normalTexture.SampleLevel(materialSampler, uv, 0.0f).rg * 2.0f - 1.0f;
            float3 sampledNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
            float sampledLengthSquared = dot(sampledNormal, sampledNormal);

            bool frameValid = tangentValid && all(isfinite(sampledNormal)) && sampledLengthSquared > 1.0e-8f &&
                isfinite(handedness) && abs(handedness) > 1.0e-8f && isfinite(instanceDeterminant) &&
                instanceDeterminant != 0.0f;

            if(frameValid)
            {
                sampledNormal = normalize(sampledNormal);
                float3 tangentDirection = normalize(tangent);
                float3 bitangent = cross(worldNormal, tangentDirection) * handedness;
                float3 mappedNormal = tangentDirection * sampledNormal.x + bitangent * sampledNormal.y +
                    worldNormal * sampledNormal.z;
                float mappedLengthSquared = dot(mappedNormal, mappedNormal);

                if(all(isfinite(mappedNormal)) && isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
                {
                    shadingNormal = mappedNormal * rsqrt(mappedLengthSquared);
                }
            }
        }
    }

    payload.hitDistance = PATH_TRACE_HIT_T;
    payload.geometricNormal = geometricNormal;
    payload.shadingNormal = shadingNormal;
    payload.uv = uv;
    payload.materialIndex = instance.materialIndex;
    payload.packedSmoothNormal = packedSmoothNormal;
    payload.packedTangent = packedTangent;
