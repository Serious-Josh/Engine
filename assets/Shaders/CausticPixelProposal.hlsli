// R.9I.3 pixel-centred caustics: the B3-T receiver-specific proposal parameters, the GPU port of the validated
// C4Mesh::TargetFor (src/Renderer/tests/CausticPointGPU/TProposal.hlsli), copied without change.
double DDot(double3 a, double3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double3 DCross(double3 a, double3 b)
{
    return double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

double DRoot(double x)
{
    return TSqrt(x);
}

double3 DUnit(double3 a)
{
    return a / DRoot(DDot(a, a));
}

double Positive(double a)
{
    return a > 0 ? a : 0;
}

double3 Closest(double3 p, double3 a, double3 b, double3 c)
{
    double3 ab = b - a, ac = c - a, ap = p - a;
    double d1 = DDot(ab, ap), d2 = DDot(ac, ap);

    if(d1 <= 0 && d2 <= 0)
    {
        return a;
    }

    double3 bp = p - b;
    double d3 = DDot(ab, bp), d4 = DDot(ac, bp);

    if(d3 >= 0 && d4 <= d3)
    {
        return b;
    }

    double vc = d1 * d4 - d3 * d2;

    if(vc <= 0 && d1 >= 0 && d3 <= 0)
    {
        return a + ab * (d1 / (d1 - d3));
    }

    double3 cp = p - c;
    double d5 = DDot(ab, cp), d6 = DDot(ac, cp);

    if(d6 >= 0 && d5 <= d6)
    {
        return c;
    }

    double vb = d5 * d2 - d1 * d6;

    if(vb <= 0 && d2 >= 0 && d6 <= 0)
    {
        return a + ac * (d2 / (d2 - d6));
    }

    double va = d3 * d6 - d5 * d4;

    if(va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
    {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    double inv = 1.0 / (va + vb + vc);
    return a + ab * (vb * inv) + ac * (vc * inv);
}

double2 Slope(double2 st, double3 a, double3 e1, double3 e2, double3 light, double3 x, double3 n, double3 t1,
              double3 t2)
{
    double3 y = a + e1 * st.x + e2 * st.y;
    double3 h = DUnit(DUnit(light - y) + DUnit(x - y));
    return double2(DDot(h, t1), DDot(h, t2)) / DDot(h, n);
}

struct Target
{
    double2 centre;
    double2 scale;
    double weight;
};
Target TargetFor(MeshTriangle tri, float3 receiver)
{
    Target result;
    result.centre = double2(1.0 / 3.0, 1.0 / 3.0);
    result.scale = 1;
    result.weight = 0;
    double3 a = (double3)tri.a, b = (double3)tri.b, c = (double3)tri.c, light = (double3)pointLight.position,
            x = (double3)receiver;
    double3 e1 = b - a, e2 = c - a, n = DUnit(DCross(e1, e2));
    double dl = DDot(n, light - a), dx = DDot(n, x - a);

    if(!(dl > 1e-6 && dx > 1e-6))
    {
        return result;
    }

    double roughness = (double)Materials[tri.material].roughness;
    roughness = roughness > 0.045 ? roughness : 0.045;
    double alpha = roughness * roughness;
    double3 mirrored = light - n * (2 * dl), mirror = x + (mirrored - x) * (dx / (dx + dl));
    double g11 = DDot(e1, e1), g12 = DDot(e1, e2), g22 = DDot(e2, e2), det = g11 * g22 - g12 * g12;
    double r1 = DDot(e1, mirror - a), r2 = DDot(e2, mirror - a);
    result.centre = double2((g22 * r1 - g12 * r2) / det, (g11 * r2 - g12 * r1) / det);
    double3 t1 = DUnit(e1), t2 = DCross(n, t1);
    double step = 1e-5;

    for(uint axis = 0; axis < 2; ++axis)
    {
        double2 delta = axis == 0 ? double2(step, 0) : double2(0, step);
        double2 derivative = (Slope(result.centre + delta, a, e1, e2, light, x, n, t1, t2) -
                              Slope(result.centre - delta, a, e1, e2, light, x, n, t1, t2)) /
                             (2 * step);
        double norm = DRoot(derivative.x * derivative.x + derivative.y * derivative.y);
        double scale = alpha / (norm > 1e-8 ? norm : 1e-8);
        result.scale[axis] = scale < 1e-4 ? 1e-4 : (scale > 10 ? 10 : scale);
    }

    double3 q = Closest(mirror, a, b, c), toLight = light - q, toReceiver = x - q;
    double ld = DRoot(DDot(toLight, toLight)), rd = DRoot(DDot(toReceiver, toReceiver));
    double3 h = DUnit(DUnit(toLight) + DUnit(toReceiver));
    double cosine = Positive(DDot(n, h)), a2 = alpha * alpha, denom = 1 + (a2 - 1) * cosine * cosine;
    double distribution = a2 / (3.14159265358979323846 * denom * denom);
    result.weight = distribution * Positive(DDot(n, toLight) / ld) * Positive(DDot(n, toReceiver) / rd) /
                    (ld * ld * rd * rd);
    return result;
}
