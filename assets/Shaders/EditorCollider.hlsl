// Nineteen root constants; CPU Matrix4 and HLSL both use row-major storage, column vectors.
// colliderShape selects the generated unit wireframe and matches ColliderOverlayShape on CPU.
// capsuleHalfCylinderHeight is the capsule's (h / 2) / r; Box and Sphere ignore it.
// colliderRole matches ColliderOverlayRole: Solid=0 (green), Trigger=1 (cyan).
cbuffer ColliderConstants : register(b0)
{
    row_major float4x4 clipFromCollider;
    uint colliderShape;
    float capsuleHalfCylinderHeight;
    uint colliderRole;
};

static const uint ColliderShapeBox = 0;
static const uint ColliderShapeSphere = 1;
static const uint ColliderShapeCapsule = 2;
static const uint ColliderShapeCone = 3;
static const uint ColliderShapeArrows = 4;

static const uint SphereSegmentsPerCircle = 64;
static const uint CapsuleRingSegments = 64;
static const uint CapsuleArcSegments = CapsuleRingSegments / 2;
static const uint CapsuleRingLineCount = 2 * CapsuleRingSegments;
static const uint CapsuleLongitudinalLineCount = 4;
static const uint ConeRingSegments = 64;
static const uint LinesPerArrow = 5;
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

float3 ConeEndpoint(uint vertexIndex)
{
    // Unit cone along +Z: ring segments at z = 1 (the last wraps to point zero), then four
    // radial lines from the apex. Matches ColliderConeVertex on CPU.
    uint lineIndex = vertexIndex / 2;
    uint endpoint = vertexIndex % 2;
    uint pointIndex = 0;

    if(lineIndex < ConeRingSegments)
    {
        pointIndex = (lineIndex + endpoint) % ConeRingSegments;
    }
    else
    {
        if(endpoint == 0)
        {
            return float3(0.0, 0.0, 0.0);
        }

        pointIndex = (lineIndex - ConeRingSegments) * (ConeRingSegments / 4);
    }

    float angle = float(pointIndex) * (TwoPi / ConeRingSegments);

    return float3(cos(angle), sin(angle), 1.0);
}

float3 ArrowsEndpoint(uint vertexIndex)
{
    // Five parallel arrows along +Z (axis, then +X, -X, +Y, -Y offsets): a shaft, then four
    // head lines from the tip. Matches ColliderArrowsVertex on CPU.
    const float arrowOffset = 0.3;
    const float headLength = 0.2;
    const float headHalfWidth = 0.08;
    const float2 offsets[5] = {float2(0.0, 0.0), float2(arrowOffset, 0.0), float2(-arrowOffset, 0.0),
        float2(0.0, arrowOffset), float2(0.0, -arrowOffset)};
    const float2 headDirections[4] = {float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0)};

    uint lineIndex = vertexIndex / 2;
    uint endpoint = vertexIndex % 2;
    uint arrowIndex = lineIndex / LinesPerArrow;
    uint lineInArrow = lineIndex % LinesPerArrow;
    float3 tip = float3(offsets[arrowIndex], 1.0);

    if(lineInArrow == 0)
    {
        return endpoint == 0 ? float3(tip.xy, 0.0) : tip;
    }

    if(endpoint == 0)
    {
        return tip;
    }

    return float3(tip.xy + headDirections[lineInArrow - 1] * headHalfWidth, 1.0 - headLength);
}

// Matches ColliderOverlayRole in EditorColliderOverlay.h.
float4 ColliderRoleColor(uint role)
{
    switch(role)
    {
    case 1:
        return float4(55.0 / 255.0, 197.0 / 255.0, 229.0 / 255.0, 1.0); // Trigger: cyan.
    case 2:
        return float4(1.0, 0.55, 0.0, 1.0);                             // Character Controller: amber.
    case 3:
        return float4(1.0, 0.15, 0.15, 1.0);                            // Controller alert: red.
    case 4:
        return float4(1.0, 0.95, 0.2, 1.0);                             // Ground support: yellow.
    case 5:
        return float4(0.95, 0.3, 0.85, 1.0);                            // Contacts: magenta.
    case 6:
        return float4(0.82, 0.84, 0.88, 1.0);                           // Camera frustum: light grey.
    case 7:
        return float4(1.0, 0.72, 0.0, 1.0);                             // Selected Camera: selection orange.
    case 8:
        return float4(0.38, 0.39, 0.42, 1.0);                           // Disabled Camera: dim grey.
    case 9:
        return float4(0.95, 0.88, 0.55, 1.0);                           // Light: pale yellow.
    case 10:
        return float4(1.0, 0.72, 0.0, 1.0);                             // Selected Light: selection orange.
    case 11:
        return float4(0.40, 0.39, 0.33, 1.0);                           // Disabled Light: dim olive grey.
    default:
        return float4(0.1, 1.0, 0.35, 1.0);                             // Solid: green.
    }
}

struct ColliderVertexOutput
{
    float4 position : SV_POSITION;
    nointerpolation float4 color : COLOR0;
};

ColliderVertexOutput VSMain(uint vertexIndex : SV_VertexID)
{
    float3 localPosition;

    if(colliderShape == ColliderShapeCapsule)
    {
        localPosition = CapsuleEndpoint(vertexIndex, capsuleHalfCylinderHeight);
    }
    else if(colliderShape == ColliderShapeCone)
    {
        localPosition = ConeEndpoint(vertexIndex);
    }
    else if(colliderShape == ColliderShapeArrows)
    {
        localPosition = ArrowsEndpoint(vertexIndex);
    }
    else if(colliderShape == ColliderShapeSphere)
    {
        localPosition = SphereCircleEndpoint(vertexIndex);
    }
    else
    {
        localPosition = BoxEdgeEndpoint(vertexIndex);
    }

    ColliderVertexOutput output;
    output.position = mul(clipFromCollider, float4(localPosition, 1));
    output.color = ColliderRoleColor(colliderRole);
    return output;
}

float4 PSMain(ColliderVertexOutput input) : SV_TARGET
{
    return input.color;
}
