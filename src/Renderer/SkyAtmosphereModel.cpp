#include "Renderer/SkyAtmosphereModel.h"

#include <algorithm>
#include <cmath>

namespace
{
    // The observer and its ray in the planet's frame, written so that no quantity is a small difference of two numbers near the
    // planet radius squared: rho2 = r^2 - R^2 is formed from the altitude directly (the same scheme as SkyAtmosphereCommon.hlsli).
    struct Ray
    {
        double groundRadius = 0.0;
        double topRadius = 0.0;
        double radius = 0.0;
        double rho2 = 0.0;
        double cosZenith = 1.0;
    };

    Ray MakeRay(const PhysicalSkySettings& settings, double altitudeMeters, double cosZenith)
    {
        const double altitude = (std::max)(altitudeMeters, 0.0) / 1000.0;
        Ray ray;
        ray.groundRadius = double(settings.planetRadiusKm);
        ray.topRadius = ray.groundRadius + double(settings.atmosphereHeightKm);
        ray.radius = ray.groundRadius + altitude;
        ray.rho2 = altitude * (2.0 * ray.groundRadius + altitude);
        ray.cosZenith = std::clamp(cosZenith, -1.0, 1.0);
        return ray;
    }

    // |p(t)|^2 - R^2 along the ray: rho2 + 2 t r mu + t^2.
    double RadiusSquaredAboveGround(const Ray& ray, double distance)
    {
        return ray.rho2 + 2.0 * distance * ray.radius * ray.cosZenith + distance * distance;
    }

    // The altitude at a distance along the ray, (|p|^2 - R^2) / (|p| + R).
    double AltitudeAt(const Ray& ray, double distance)
    {
        const double aboveGround = (std::max)(RadiusSquaredAboveGround(ray, distance), 0.0);
        return aboveGround / (std::sqrt(ray.groundRadius * ray.groundRadius + aboveGround) + ray.groundRadius);
    }

    // The interval of the ray inside the atmosphere's top sphere ([0, 0] when it never enters).
    void AtmosphereInterval(const Ray& ray, double& start, double& end)
    {
        const double b = ray.radius * ray.cosZenith;
        const double topAboveGround = (ray.topRadius - ray.groundRadius) * (ray.topRadius + ray.groundRadius);
        // c = r^2 - top^2, negative inside the atmosphere.
        const double c = ray.rho2 - topAboveGround;
        const double discriminant = b * b - c;
        start = 0.0;
        end = 0.0;

        if(discriminant < 0.0)
        {
            return;
        }

        // The two roots without cancellation: q = -(b + sign(b) sqrt(D)), roots q and c / q.
        const double root = std::sqrt(discriminant);
        const double q = b >= 0.0 ? -(b + root) : -b + root;
        double first = q;
        double second = q != 0.0 ? c / q : 0.0;

        if(first > second)
        {
            std::swap(first, second);
        }

        if(second <= 0.0)
        {
            return;
        }

        start = (std::max)(first, 0.0);
        end = second;
    }
}

double SkyAtmosphereModel::CameraAltitudeMeters(const PhysicalSkySettings& settings, double cameraY)
{
    return (std::max)(cameraY - double(settings.seaLevelMeters), MinimumCameraAltitudeMeters);
}

SkyAtmosphereModel::Rgb SkyAtmosphereModel::Extinction(const PhysicalSkySettings& settings, double altitudeKm)
{
    const double altitude = (std::max)(altitudeKm, 0.0);
    const double rayleighDensity = std::exp(-altitude / double(settings.rayleighScaleHeightKm));
    const double mieDensity = std::exp(-altitude / double(settings.mieScaleHeightKm));
    const double ozoneHalfWidth = 0.5 * double(settings.ozoneLayerWidthKm);
    const double ozoneCentre = double(settings.ozoneMinimumAltitudeKm) + ozoneHalfWidth;
    const double ozoneDensity = (std::max)(0.0, 1.0 - std::abs(altitude - ozoneCentre) / ozoneHalfWidth);

    Rgb extinction{};

    // Coefficients are stored per metre; the model works per kilometre.
    for(std::size_t channel = 0; channel < 3; ++channel)
    {
        const double rayleigh = double(settings.rayleighScattering[channel]) + double(settings.rayleighAbsorption[channel]);
        const double mie = double(settings.mieScattering[channel]) + double(settings.mieAbsorption[channel]);
        const double ozone = double(settings.ozoneAbsorption[channel]);
        extinction[channel] = 1000.0 * (rayleigh * rayleighDensity + mie * mieDensity + ozone * ozoneDensity);
    }

    return extinction;
}

SkyAtmosphereModel::Rgb SkyAtmosphereModel::OpticalDepth(const PhysicalSkySettings& settings, double altitudeMeters,
    double cosZenith, int steps)
{
    const Ray ray = MakeRay(settings, altitudeMeters, cosZenith);
    double start = 0.0;
    double end = 0.0;
    AtmosphereInterval(ray, start, end);

    Rgb depth{0.0, 0.0, 0.0};
    const int stepCount = (std::max)(steps, 1);
    const double length = end - start;

    if(!(length > 0.0))
    {
        return depth;
    }

    // Midpoints of quadratically spaced segments, dense near the observer where the density is highest.
    for(int step = 0; step < stepCount; ++step)
    {
        const double first = double(step) / double(stepCount);
        const double second = double(step + 1) / double(stepCount);
        const double segmentStart = start + length * first * first;
        const double segmentEnd = start + length * second * second;
        const double segment = segmentEnd - segmentStart;
        const Rgb extinction = Extinction(settings, AltitudeAt(ray, 0.5 * (segmentStart + segmentEnd)));

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            depth[channel] += extinction[channel] * segment;
        }
    }

    return depth;
}

bool SkyAtmosphereModel::HitsGround(const PhysicalSkySettings& settings, double altitudeMeters, double cosZenith)
{
    const Ray ray = MakeRay(settings, altitudeMeters, cosZenith);

    // Below the geometric horizon of the observer, cos(zenith) < -sqrt(r^2 - R^2) / r.
    return ray.cosZenith < -std::sqrt(ray.rho2) / ray.radius;
}

SkyAtmosphereModel::Rgb SkyAtmosphereModel::Transmittance(const PhysicalSkySettings& settings, double altitudeMeters,
    double cosZenith, int steps)
{
    if(HitsGround(settings, altitudeMeters, cosZenith))
    {
        return {0.0, 0.0, 0.0};
    }

    const Rgb depth = OpticalDepth(settings, altitudeMeters, cosZenith, steps);
    return {std::exp(-depth[0]), std::exp(-depth[1]), std::exp(-depth[2])};
}

SkyAtmosphereModel::Rgb SkyAtmosphereModel::DiskTransmittance(const PhysicalSkySettings& settings, double altitudeMeters,
    const Vector3& direction, double angularRadius, int steps)
{
    const double length = std::sqrt(double(direction.x) * direction.x + double(direction.y) * direction.y +
        double(direction.z) * direction.z);

    if(!(length > 0.0) || !std::isfinite(length))
    {
        return {0.0, 0.0, 0.0};
    }

    const double cosZenith = (std::max)(-1.0, (std::min)(1.0, double(direction.y) / length));
    const Ray ray = MakeRay(settings, altitudeMeters, cosZenith);
    const double elevation = std::asin(cosZenith);
    const double horizon = std::asin(-std::sqrt(ray.rho2) / ray.radius);
    const double radius = std::isfinite(angularRadius) ? (std::max)(angularRadius, 0.0) : 0.0;

    if(!(radius > 0.0) || elevation - horizon >= 4.0 * radius)
    {
        return Transmittance(settings, altitudeMeters, cosZenith, steps);
    }

    constexpr int Strips = 8;
    Rgb sum{0.0, 0.0, 0.0};
    double weight = 0.0;

    // A strip the horizon crosses counts by its visible height, transmitted at that part's middle: continuous as it sets.
    const double halfHeight = radius / Strips;

    for(int strip = 0; strip < Strips; ++strip)
    {
        const double offset = -1.0 + (2.0 * strip + 1.0) / Strips;
        const double chord = std::sqrt((std::max)(0.0, 1.0 - offset * offset));
        const double centre = elevation + offset * radius;
        const double top = (std::min)(centre + halfHeight, 0.5 * 3.14159265358979323846);
        const double bottom = (std::max)(centre - halfHeight, horizon);
        weight += chord;

        if(top <= bottom)
        {
            continue;
        }

        const double visible = (top - bottom) / (2.0 * halfHeight);
        const Rgb transmittance = Transmittance(settings, altitudeMeters, std::sin(0.5 * (top + bottom)), steps);

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            sum[channel] += chord * visible * transmittance[channel];
        }
    }

    return {sum[0] / weight, sum[1] / weight, sum[2] / weight};
}

SkyAtmosphereModel::Rgb SkyAtmosphereModel::Transmittance(const PhysicalSkySettings& settings, double altitudeMeters,
    const Vector3& direction, int steps)
{
    const double length = std::sqrt(double(direction.x) * direction.x + double(direction.y) * direction.y +
        double(direction.z) * direction.z);

    if(!(length > 0.0) || !std::isfinite(length))
    {
        return {0.0, 0.0, 0.0};
    }

    return Transmittance(settings, altitudeMeters, double(direction.y) / length, steps);
}
