// R.9I.3 pixel-centred caustics: the B3-T T3 double-precision elementary functions for the unchanged
// truncated-Cauchy CDF (src/Renderer/tests/CausticPointGPU/TCauchyMath.hlsli), copied without change.
// Argument reduction bounds atan's Taylor argument by tan(pi/32)<0.099.
// 12 terms then have truncation error < 8*0.099^25/25 < 3e-25.
double TPi()
{
    return asdouble(0x54442d18u, 0x400921fbu);
}

double TSqrt(double x)
{
    if(x == 0)
    {
        return 0;
    }

    double r = 1.0 / (double)sqrt((float)x);
    r = r * (1.5 - 0.5 * x * r * r);
    r = r * (1.5 - 0.5 * x * r * r);
    return x * r;
}

double TAtan(double x)
{
    double sign = x < 0 ? -1.0 : 1.0;
    double z = x < 0 ? -x : x;
    bool reciprocal = z > 1;

    if(reciprocal)
    {
        z = 1.0 / z;
    }

    for(uint k = 0; k < 3; ++k)
    {
        z = z / (1.0 + TSqrt(1.0 + z * z));
    }

    double power = z, sum = z, z2 = z * z;

    for(uint j = 1; j < 12; ++j)
    {
        power *= -z2;
        sum += power / (double)(2 * j + 1);
    }

    sum *= 8;

    if(reciprocal)
    {
        sum = TPi() * 0.5 - sum;
    }

    return sign * sum;
}

double TTan(double x)
{
    // |x|<=pi/2. 14 terms give sine/cosine truncation errors < 2e-24.
    double xx = x * x, s = x, c = 1, st = x, ct = 1;

    for(uint j = 1; j < 14; ++j)
    {
        st *= -xx / (double)((2 * j) * (2 * j + 1));
        ct *= -xx / (double)((2 * j - 1) * (2 * j));
        s += st;
        c += ct;
    }

    return s / c;
}

float2 TSample(float2 centre, float2 scale, float2 random)
{
    float2 result;

    for(uint axis = 0; axis < 2; ++axis)
    {
        double c = (double)centre[axis], a = (double)scale[axis];
        double lo = TAtan(-c / a), hi = TAtan((1.0 - c) / a);
        result[axis] = (float)(c + a * TTan(lo + (double)random[axis] * (hi - lo)));
    }

    return result;
}

float TDensity(float2 centre, float2 scale, float2 st)
{
    double product = 1;

    for(uint axis = 0; axis < 2; ++axis)
    {
        double c = (double)centre[axis], a = (double)scale[axis], z = ((double)st[axis] - c) / a;
        double delta = TAtan((1.0 - c) / a) - TAtan(-c / a);
        product /= a * delta * (1.0 + z * z);
    }

    return (float)product;
}
