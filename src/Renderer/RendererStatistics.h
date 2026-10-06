#pragma once

#include "Renderer/GpuTimestampProfiler.h"

#include <cstdint>

struct RendererStatistics
{
    uint64_t frameSerial = 0;

    uint64_t geometryDrawCalls = 0;
    uint64_t submittedTriangles = 0;

    uint64_t renderObjectCount = 0;
    uint64_t tlasInstanceCount = 0;

    GpuTimestampProfiler::Results gpuTimings{};
};