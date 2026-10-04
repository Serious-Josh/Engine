#pragma once

#include "Math/Matrix4.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class Mesh;

enum class RayTracingAccelerationStructureBuildPreference
{
    FastTrace,
    FastBuild
};

struct RayTracingGeometryDescription
{
    const Mesh* mesh = nullptr;
    bool opaque = true;
};

struct RayTracingBottomLevelDescription
{
    std::vector<RayTracingGeometryDescription> geometries;

    RayTracingAccelerationStructureBuildPreference buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;
};

struct RayTracingInstanceDescription
{
    Matrix4 transform;

    std::uint32_t instanceID = 0;
    std::uint8_t mask = 0xFF;
    
    std::size_t bottomLevelIndex = 0;
};

struct RayTracingTopLevelDescription
{
    std::vector<RayTracingInstanceDescription> instances;

    RayTracingAccelerationStructureBuildPreference buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;
};