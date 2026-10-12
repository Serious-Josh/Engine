#pragma once

#include "GpuTimingMonitor.h"

// Keep this order aligned with GpuTimestampProfiler::Region.
inline constexpr const char* GpuTimingRegionNames[] = {"GPU frame",
                                                       "Geometry",
                                                       "RT shadows",
                                                       "Shadow denoise",
                                                       "RT reflections",
                                                       "Reflection limiter",
                                                       "NRD prepare",
                                                       "NRD REBLUR Specular",
                                                       "NRD reflection resolve",
                                                       "Deferred lighting",
                                                       "Tone map",
                                                       "TLAS update",
                                                       "Selection mask",
                                                       "RT AO",
                                                       "AO temporal",
                                                       "AO denoise",
                                                       "RT GI",
                                                       "Reflection temporal",
                                                       "Reflection denoise",
                                                       "NRD diffuse resolve",
                                                       "DebugUI rendering",
                                                       "GI spatial denoise",
                                                       "NRD REBLUR Diffuse"};
static_assert(sizeof(GpuTimingRegionNames) / sizeof(GpuTimingRegionNames[0]) == GpuTimingMonitor::RegionCount);
