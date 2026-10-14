#ifndef SHADING_NORMAL_HLSLI
#define SHADING_NORMAL_HLSLI

// Inputs are normalized world-space directions. Only camera-facing surfaces
// are corrected; the reference normal retains its original meaning at the call site.
float3 CorrectShadingNormalViewHorizon(
    float3 shadingNormal,
    float3 referenceNormal,
    float3 viewDirection,
    float horizonMargin = 0.02f)
{
    if(!all(isfinite(viewDirection)) || dot(viewDirection, viewDirection) < 1.0e-8f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }

    if(!all(isfinite(shadingNormal)) || dot(shadingNormal, shadingNormal) < 1.0e-8f)
    {
        return viewDirection;
    }

    float margin = isfinite(horizonMargin) ? clamp(horizonMargin, 1.0e-4f, 0.25f) : 0.02f;
    float shadingNormalDotView = dot(shadingNormal, viewDirection);

    if(shadingNormalDotView >= margin)
    {
        return shadingNormal;
    }

    if(!all(isfinite(referenceNormal)) || dot(referenceNormal, referenceNormal) < 1.0e-8f)
    {
        referenceNormal = viewDirection;
    }

    float referenceNormalDotView = dot(referenceNormal, viewDirection);

    if(referenceNormalDotView <= 0.0f)
    {
        return shadingNormal;
    }

    // A nearly tangent reference cannot reach the margin by blending alone.
    // Lift only the local blend target, retaining its transverse direction.
    float3 blendTarget = referenceNormal;

    if(referenceNormalDotView < 2.0f * margin)
    {
        blendTarget = normalize(referenceNormal + viewDirection * (2.0f * margin - referenceNormalDotView));
    }

    float targetDotView = dot(blendTarget, viewDirection);
    float blendWeight = saturate((margin - shadingNormalDotView) / (targetDotView - shadingNormalDotView));
    float3 correctedNormal = lerp(shadingNormal, blendTarget, blendWeight);

    // The unnormalized blend has a positive view component of at least margin,
    // so it cannot vanish. Its length is at most one; normalization preserves
    // or increases that component. Weight approaches zero at the threshold.
    return normalize(correctedNormal);
}

#endif
