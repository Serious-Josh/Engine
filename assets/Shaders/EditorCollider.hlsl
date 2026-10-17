// Eighteen root constants; CPU Matrix4 and HLSL both use row-major storage, column vectors.
// colliderShape selects the generated unit wireframe and matches ColliderOverlayShape on CPU.
// capsuleHalfCylinderHeight is the capsule's (h / 2) / r; Box and Sphere ignore it.
cbuffer ColliderConstants : register(b0)
{
    row_major float4x4 clipFromCollider;
    uint colliderShape;
    float capsuleHalfCylinderHeight;
};

static const uint ColliderShapeBox = 0;
static const uint ColliderShapeSphere = 1;
static const uint ColliderShapeCapsule = 2;

static const uint SphereSegmentsPerCircle = 64;
static const uint CapsuleRingSegments = 64;
static const uint CapsuleArcSegments = CapsuleRingSegments / 2;
static const uint CapsuleRingLineCount = 2 * CapsuleRingSegments;
static const uint CapsuleLongitudinalLineCount = 4;
static const float Pi = 3.14159265358979323846;
static const float TwoPi = 6.28318530717958647692;

float3 BoxEdgeEndpoint(uint vertexIndex)
{
    // Four parallel edges per axis, each with two endpoints. The remaining axes
    // choose the four fixed sides. This ordering matches ColliderBoxVertex on CPU.
    const uint endpointsPerEdge = 2;
    const uint edgesPerAxis = 4;
    const uint axisCount = 3;
    uint edgeIndex = vertexIndex / endpointsPerEdge;
    uint edgeAxis = edgeIndex / edgesPerAxis;
    uint sideIndex = edgeIndex % edgesPerAxis;
    uint firstSideAxis = (edgeAxis + 1) % axisCount;
    uint secondSideAxis = (edgeAxis + 2) % axisCount;

    float3 localPosition = 0.0;
    localPosition[edgeAxis] = float(vertexIndex % endpointsPerEdge) - 0.5;
    localPosition[firstSideAxis] = float(sideIndex % 2) - 0.5;
    localPosition[secondSideAxis] = float(sideIndex / 2) - 0.5;

    return localPosition;
}

float3 SphereCircleEndpoint(uint vertexIndex)
{
    // Three unit great circles of line segments. The last segment's second endpoint
    // wraps to point zero so each circle closes exactly. Matches ColliderSphereVertex.
    const uint endpointsPerSegment = 2;
    const uint verticesPerCircle = SphereSegmentsPerCircle * endpointsPerSegment;
    uint circleIndex = vertexIndex / verticesPerCircle;
    uint circleVertex = vertexIndex % verticesPerCircle;
    uint segmentIndex = circleVertex / endpointsPerSegment;
    uint pointIndex = (segmentIndex + circleVertex % endpointsPerSegment) % SphereSegmentsPerCircle;

    float angle = float(pointIndex) * (TwoPi / SphereSegmentsPerCircle);
    float cosine = cos(angle);
    float sine = sin(angle);

    // Circle 0 lies in the local XY plane, circle 1 in XZ and circle 2 in YZ.
    if(circleIndex == 0)
    {
        return float3(cosine, sine, 0.0);
    }
    else if(circleIndex == 1)
    {
        return float3(cosine, 0.0, sine);
    }
    else
    {
        return float3(0.0, cosine, sine);
    }
}

// Unit-radius point on the XZ-plane ring at the given point index and height.
float3 CapsuleRingPoint(uint pointIndex, float height)
{
    float angle = float(pointIndex % CapsuleRingSegments) * (TwoPi / CapsuleRingSegments);

    return float3(cos(angle), height, sin(angle));
}

float3 CapsuleEndpoint(uint vertexIndex, float halfHeight)
{
    // Radius-1 capsule along +Y with hemisphere centers at y = -/+ halfHeight. Lines:
    // two rings, four longitudinal lines, then two half-arcs per hemisphere.
    // Matches ColliderCapsuleVertex on CPU.
    uint lineIndex = vertexIndex / 2;
    uint endpoint = vertexIndex % 2;

    if(lineIndex < CapsuleRingLineCount)
    {
        uint ringIndex = lineIndex / CapsuleRingSegments;
        uint segmentIndex = lineIndex % CapsuleRingSegments;
        float ringHeight = ringIndex == 0 ? -halfHeight : halfHeight;

        return CapsuleRingPoint(segmentIndex + endpoint, ringHeight);
    }

    uint longitudinalIndex = lineIndex - CapsuleRingLineCount;

    if(longitudinalIndex < CapsuleLongitudinalLineCount)
    {
        float lineHeight = endpoint == 0 ? -halfHeight : halfHeight;

        return CapsuleRingPoint(longitudinalIndex * (CapsuleRingSegments / 4), lineHeight);
    }

    uint arcLineIndex = longitudinalIndex - CapsuleLongitudinalLineCount;
    uint hemisphereIndex = arcLineIndex / (2 * CapsuleArcSegments);
    uint arcInHemisphere = (arcLineIndex / CapsuleArcSegments) % 2;
    uint pointIndex = arcLineIndex % CapsuleArcSegments + endpoint;

    float theta = float(pointIndex) * (Pi / CapsuleArcSegments);
    float horizontal = cos(theta);
    float poleward = sin(theta);
    float height = hemisphereIndex == 0 ? -halfHeight - poleward : halfHeight + poleward;

    if(arcInHemisphere == 0)
    {
        return float3(horizontal, height, 0.0);
    }
    else
    {
        return float3(0.0, height, horizontal);
    }
}

float4 VSMain(uint vertexIndex : SV_VertexID) : SV_POSITION
{
    float3 localPosition;

    if(colliderShape == ColliderShapeCapsule)
    {
        localPosition = CapsuleEndpoint(vertexIndex, capsuleHalfCylinderHeight);
    }
    else if(colliderShape == ColliderShapeSphere)
    {
        localPosition = SphereCircleEndpoint(vertexIndex);
    }
    else
    {
        localPosition = BoxEdgeEndpoint(vertexIndex);
    }

    return mul(clipFromCollider, float4(localPosition, 1));
}

float4 PSMain() : SV_TARGET
{
    return float4(0.1, 1.0, 0.35, 1.0);
}
