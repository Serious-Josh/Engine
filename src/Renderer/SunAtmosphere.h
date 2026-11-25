#pragma once

#include "Scene/PhysicalSkySettings.h"

// Physical sky S2: the atmosphere between the sun and the camera (FrameLighting.h's BuildFrameLighting, Renderer's frame lighting
// cache).
struct SunAtmosphere
{
    PhysicalSkySettings settings;
    // max(camera y - sea level, 0.5 m) (SkyAtmosphereModel::CameraAltitudeMeters).
    double cameraAltitudeMeters = 0.5;
};
