// AIR.4D D2.1: the Omni ellipsoid and Vortex ring emitters' geometry, in the motor's local frame (its emission axes, metres). Used by
// AirSolver.hlsl; the behaviour is specified in AirMotorComponent.h and Documentation/Air/AirArchitecture.md (AIR.4D D2.1).

#ifndef AIR_EMITTER_HLSLI
#define AIR_EMITTER_HLSLI

// The Omni lattice: 8 bands of colatitude by 16 longitudes over the ellipsoid's parametric sphere. The Vortex lattice: 64 points
// evenly spaced in the ring's parametric angle.
static const uint AirOmniBands = 8;
static const uint AirOmniLongitudes = 16;
static const uint AirOmniSamples = AirOmniBands * AirOmniLongitudes;
static const uint AirVortexSamples = 64;
// Each sample's state takes 2 bits (AirMotorEmissionStatus::Point), 16 per word: 8 words per motor slot.
static const uint AirSampleWordsPerMotor = 8;
// How far outward along the normal an Omni sample is checked for open fluid, in metres.
static const float AirSurfaceStep = 1.0 / 1024.0;
// Fallback candidates: the lattice point holding the nearest point, then its neighbours nearest first (Omni: two rings of the
// band-by-longitude grid; Vortex: 12 either side). At most 7 of them, those that emit, are traced.
static const uint AirFallbackCandidates = 25;
static const uint AirMaximumFallbackTraces = 7;
// Iteration bounds for the nearest-point solvers. The ellipsoid's Newton iteration converges monotonically (see below), at worst
// gaining a factor of 1.5 per step far from the surface, so 64 covers every authorable size and distance. The ellipse bisection
// halves its interval each step and stops when float can no longer split it.
static const uint AirEllipsoidIterations = 64;
static const uint AirEllipseIterations = 160;

static const float AirPi = 3.14159265;

// The Omni grid offsets, in fallback order: (band, longitude).
static const int2 AirOmniFallbackOffsets[AirFallbackCandidates] = {
    int2(0, 0),
    int2(0, 1), int2(0, -1), int2(1, 0), int2(-1, 0), int2(1, 1), int2(1, -1), int2(-1, 1), int2(-1, -1),
    int2(0, 2), int2(0, -2), int2(2, 0), int2(-2, 0), int2(1, 2), int2(1, -2), int2(-1, 2), int2(-1, -2),
    int2(2, 1), int2(2, -1), int2(-2, 1), int2(-2, -1), int2(2, 2), int2(2, -2), int2(-2, 2), int2(-2, -2)};

// The point of a shape nearest a local point, and the directions the motor needs there.
struct AirNearestPoint
{
    float3 position;
    // Omni: the outward unit normal. Vortex: the unit tangent, counter-clockwise looking down +Z.
    float3 direction;
    // Vortex: the in-plane inward unit normal.
    float3 inward;
    float distance;
    // Omni: whether the local point lies strictly outside the ellipsoid (only outside points have a region coordinate).
    bool outside;
};

// The nearest point of the ellipsoid x^2/a^2 + y^2/b^2 + z^2/c^2 = 1 (semi-axes `semiAxes`) to `local`.
//
// For a point outside, the nearest surface point is x_i = a_i^2 y_i / (t + a_i^2) for the unique t > 0 with
// F(t) = sum (a_i y_i / (t + a_i^2))^2 - 1 = 0. F is convex and decreasing for t > -min a_i^2, and F(0) > 0 outside, so Newton's
// method from t = 0 approaches the root from below without ever overshooting it. The point is unique, so no tie can arise; points
// on an axis (y_i = 0) need no special case.
AirNearestPoint AirEllipsoidNearest(float3 local, float3 semiAxes)
{
    AirNearestPoint nearest;
    nearest.inward = float3(0.0, 0.0, 0.0);
    const float3 scaled = local / semiAxes;
    nearest.outside = dot(scaled, scaled) > 1.0;

    if(!nearest.outside)
    {
        nearest.position = local;
        nearest.direction = float3(0.0, 0.0, 0.0);
        nearest.distance = 0.0;
        return nearest;
    }

    const float3 squared = semiAxes * semiAxes;
    float t = 0.0;

    for(uint iteration = 0; iteration < AirEllipsoidIterations; ++iteration)
    {
        const float3 denominator = t + squared;
        const float3 ratio = semiAxes * local / denominator;
        const float excess = dot(ratio, ratio) - 1.0;

        if(excess <= 0.0)
        {
            break;
        }

        const float slope = -2.0 * dot(ratio * ratio, 1.0 / denominator);
        const float step = -excess / slope;
        t += step;

        if(step <= t * 1e-7)
        {
            break;
        }
    }

    const float3 denominator = t + squared;
    nearest.position = squared * local / denominator;
    nearest.direction = normalize(local / denominator);
    nearest.distance = length(local - nearest.position);
    return nearest;
}

// The root s of G(s) = (r0 z0 / (s + r0))^2 + (z1 / (s + 1))^2 - 1, by bisection (Eberly, "Distance from a Point to an Ellipse").
float AirEllipseRoot(float r0, float z0, float z1, float g)
{
    const float n0 = r0 * z0;
    float s0 = z1 - 1.0;
    float s1 = g < 0.0 ? 0.0 : length(float2(n0, z1)) - 1.0;
    float s = 0.0;

    for(uint iteration = 0; iteration < AirEllipseIterations; ++iteration)
    {
        s = (s0 + s1) * 0.5;

        if(s == s0 || s == s1)
        {
            break;
        }

        const float ratio0 = n0 / (s + r0);
        const float ratio1 = z1 / (s + 1.0);
        const float value = ratio0 * ratio0 + ratio1 * ratio1 - 1.0;

        if(value > 0.0)
        {
            s0 = s;
        }
        else if(value < 0.0)
        {
            s1 = s;
        }
        else
        {
            break;
        }
    }

    return s;
}

// The nearest point to `query` (first quadrant, y0, y1 >= 0) of the ellipse with semi-axes e0 >= e1 > 0, in the same quadrant.
float2 AirEllipseNearestSorted(float e0, float e1, float2 query)
{
    if(query.y > 0.0)
    {
        if(query.x > 0.0)
        {
            const float z0 = query.x / e0;
            const float z1 = query.y / e1;
            const float g = z0 * z0 + z1 * z1 - 1.0;

            if(g == 0.0)
            {
                return query;
            }

            const float r0 = (e0 / e1) * (e0 / e1);
            const float s = AirEllipseRoot(r0, z0, z1, g);
            return float2(r0 * query.x / (s + r0), query.y / (s + 1.0));
        }

        return float2(0.0, e1);
    }

    // On the major axis: inside the evolute's cusp the nearest query leaves the axis (the +y one of the two is taken); beyond it,
    // the axis end is nearest. A circle (e0 == e1) has no cusp.
    const float numerator = e0 * query.x;
    const float denominator = e0 * e0 - e1 * e1;

    if(numerator < denominator)
    {
        const float along = numerator / denominator;
        return float2(e0 * along, e1 * sqrt(1.0 - along * along));
    }

    return float2(e0, 0.0);
}

// The nearest point of the ring (the ellipse x^2/a^2 + y^2/b^2 = 1 in the local XY plane, semi-axes semiAxes.xy) to `local`.
//
// The nearest ring point lies over the nearest point of the in-plane ellipse to the projection, so the problem is 2D. Ties are
// broken deterministically: the quadrant is chosen with sign(0) taken as +, and a point on the major axis inside the cusp takes the
// +minor-axis solution; so the centre of a circle maps to (+a, 0) and the centre of an ellipse to the +minor-axis end.
AirNearestPoint AirRingNearest(float3 local, float2 semiAxes)
{
    const float2 flipped = float2(local.x < 0.0 ? -1.0 : 1.0, local.y < 0.0 ? -1.0 : 1.0);
    const float2 quadrant = abs(local.xy);
    const bool swapped = semiAxes.x < semiAxes.y;
    float2 onEllipse;

    if(swapped)
    {
        onEllipse = AirEllipseNearestSorted(semiAxes.y, semiAxes.x, quadrant.yx).yx;
    }
    else
    {
        onEllipse = AirEllipseNearestSorted(semiAxes.x, semiAxes.y, quadrant);
    }

    onEllipse *= flipped;

    AirNearestPoint nearest;
    nearest.position = float3(onEllipse, 0.0);
    nearest.distance = length(local - nearest.position);
    nearest.outside = true;
    // The implicit gradient (x / a^2, y / b^2) is the ellipse's outward normal; the counter-clockwise tangent is it turned by +90
    // degrees about +Z.
    const float2 outward = normalize(onEllipse / (semiAxes * semiAxes));
    nearest.direction = float3(-outward.y, outward.x, 0.0);
    nearest.inward = float3(-outward, 0.0);
    return nearest;
}

// The Omni lattice's bands and longitudes are laid out in a frame tilted from the shape's axes, M = Rz(0.37) * Rx(0.61) (radians),
// so that their cell boundaries never coincide with the axis planes and diagonals where symmetric, grid-aligned geometry puts points
// exactly; there, rounding would decide which side of a boundary a point falls on. `AirLatticeFromShape` applies M to a parametric
// direction, `AirShapeFromLattice` its inverse (transpose).
static const float AirLatticeTiltX = 0.61;
static const float AirLatticeTiltZ = 0.37;

float3 AirLatticeFromShape(float3 direction)
{
    const float cx = cos(AirLatticeTiltX);
    const float sx = sin(AirLatticeTiltX);
    const float cz = cos(AirLatticeTiltZ);
    const float sz = sin(AirLatticeTiltZ);
    const float3 tilted = float3(direction.x, cx * direction.y - sx * direction.z, sx * direction.y + cx * direction.z);
    return float3(cz * tilted.x - sz * tilted.y, sz * tilted.x + cz * tilted.y, tilted.z);
}

float3 AirShapeFromLattice(float3 direction)
{
    const float cx = cos(AirLatticeTiltX);
    const float sx = sin(AirLatticeTiltX);
    const float cz = cos(AirLatticeTiltZ);
    const float sz = sin(AirLatticeTiltZ);
    const float3 untwisted = float3(cz * direction.x + sz * direction.y, -sz * direction.x + cz * direction.y, direction.z);
    return float3(untwisted.x, cx * untwisted.y + sx * untwisted.z, -sx * untwisted.y + cx * untwisted.z);
}

// Omni lattice sample `index`: its point on the parametric unit sphere (band = index / 16, longitude = index % 16, each at the
// centre of its cell of colatitude and longitude in the lattice frame).
float3 AirOmniSampleDirection(uint index)
{
    const float colatitude = (float(index / AirOmniLongitudes) + 0.5) * (AirPi / float(AirOmniBands));
    const float longitude = (float(index % AirOmniLongitudes) + 0.5) * (2.0 * AirPi / float(AirOmniLongitudes));
    return AirShapeFromLattice(float3(sin(colatitude) * cos(longitude), sin(colatitude) * sin(longitude), cos(colatitude)));
}

// The band and longitude of the lattice cell holding a surface point (local), through its parametric direction in the lattice frame.
int2 AirOmniLatticeCell(float3 surface, float3 semiAxes)
{
    const float3 direction = AirLatticeFromShape(normalize(surface / semiAxes));
    const int band = min(int(floor(acos(clamp(direction.z, -1.0, 1.0)) * (float(AirOmniBands) / AirPi))), int(AirOmniBands) - 1);
    float longitude = atan2(direction.y, direction.x);

    if(longitude < 0.0)
    {
        longitude += 2.0 * AirPi;
    }

    const int column = min(int(floor(longitude * (float(AirOmniLongitudes) / (2.0 * AirPi)))), int(AirOmniLongitudes) - 1);
    return int2(band, column);
}

// Vortex lattice sample `index`: its parametric angle.
float AirRingSampleAngle(uint index)
{
    return float(index) * (2.0 * AirPi / float(AirVortexSamples));
}

// The Vortex lattice point nearest a ring point (local), by parametric angle.
int AirRingLatticeIndex(float3 ring, float2 semiAxes)
{
    float angle = atan2(ring.y / semiAxes.y, ring.x / semiAxes.x);

    if(angle < 0.0)
    {
        angle += 2.0 * AirPi;
    }

    return int(round(angle * (float(AirVortexSamples) / (2.0 * AirPi)))) % int(AirVortexSamples);
}

// Fallback candidate `candidate` around a lattice cell: the sample index, or -1 when an Omni offset leaves the bands.
int AirOmniFallbackSample(int2 cell, uint candidate)
{
    const int2 offset = AirOmniFallbackOffsets[candidate];
    const int band = cell.x + offset.x;

    if(band < 0 || band >= int(AirOmniBands))
    {
        return -1;
    }

    const int column = (cell.y + offset.y + int(AirOmniLongitudes)) % int(AirOmniLongitudes);
    return band * int(AirOmniLongitudes) + column;
}

int AirRingFallbackSample(int index, uint candidate)
{
    // 0, +1, -1, +2, -2, ... +12, -12.
    const int magnitude = int((candidate + 1) / 2);
    const int offset = (candidate % 2 == 1) ? magnitude : -magnitude;
    return (index + offset + int(AirVortexSamples)) % int(AirVortexSamples);
}

#endif
