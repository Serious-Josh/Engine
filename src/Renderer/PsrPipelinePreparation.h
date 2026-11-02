#pragma once

#include "Passes/PsrInPlaceReplacePass.h"
#include "Passes/PathTraceTemporalPass.h"
#include "Passes/PathTraceSpatialPass.h"
#include "Passes/NrdPathTracePreparePass.h"

// Private output of an auxiliary job on the existing pipeline worker. No textures, histories, descriptor
// heaps or command lists. Retained compute objects can be re-adopted after ordinary NRD resource eviction.
struct PsrPipelinePreparation
{
    PsrInPlaceReplacePass replacement;
    PathTraceTemporalPass temporal;
    PathTraceSpatialPass spatial;
    NrdPathTracePreparePass reblur;
    NrdPathTracePreparePass relax;
    NrdPathTracePreparePass hybrid;

    ~PsrPipelinePreparation()
    {
        replacement.Shutdown();
        temporal.Shutdown();
        spatial.Shutdown();
        reblur.Shutdown();
        relax.Shutdown();
        hybrid.Shutdown();
    }
};
