#pragma once

#include "Math/Matrix4.h"

// Transient read value owned by its caller, never serialized. Current and previous
// describe presentation; authored Transform remains the editing source of truth.
struct PresentationTransform
{
    Matrix4 currentModel;
    Matrix4 previousModel;
};
