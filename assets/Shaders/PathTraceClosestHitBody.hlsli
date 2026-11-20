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

    // Tangent-space normal mapping, identical to the RTGI/G-buffer convention.
    if(instance.materialIndex < materialCount && instance.tangentOffset != 0xFFFFFFFFu &&
        materials[instance.materialIndex].normalTextureIndex != 0xFFFFFFFFu)
    {
        GPUMaterial material = materials[instance.materialIndex];
        float4 objectTangent = PathTraceLoadTangent(vertexBuffer, indices.x, instance) * barycentrics.x +
            PathTraceLoadTangent(vertexBuffer, indices.y, instance) * barycentrics.y +
            PathTraceLoadTangent(vertexBuffer, indices.z, instance) * barycentrics.z;
        float3 tangent = mul((float3x3)PATH_TRACE_HIT_OBJECT_TO_WORLD, objectTangent.xyz);
        tangent -= worldNormal * dot(worldNormal, tangent);
        float tangentLengthSquared = dot(tangent, tangent);
        float instanceDeterminant = determinant((float3x3)PATH_TRACE_HIT_OBJECT_TO_WORLD);
        float handedness = objectTangent.w * (instanceDeterminant < 0.0f ? -1.0f : 1.0f);

        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.normalTextureIndex)];
        float2 normalXY = normalTexture.SampleLevel(materialSampler, uv, 0.0f).rg * 2.0f - 1.0f;
        float3 sampledNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);

        bool frameValid = all(isfinite(tangent)) && isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
            all(isfinite(sampledNormal)) && sampledLengthSquared > 1.0e-8f && isfinite(handedness) &&
            abs(handedness) > 1.0e-8f && isfinite(instanceDeterminant) && instanceDeterminant != 0.0f;

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

    payload.hitDistance = PATH_TRACE_HIT_T;
    payload.geometricNormal = geometricNormal;
    payload.shadingNormal = shadingNormal;
    payload.uv = uv;
    payload.materialIndex = instance.materialIndex;
