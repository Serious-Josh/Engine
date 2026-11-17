#pragma once

#include "Scene/SceneDocument.h"

// The mist appearance widgets shared by the Local Level mist panel and the World environment editor, so both author the
// same fields with the same ranges and tooltips. The caller owns the settings, the enabled state and what an edit does.
struct MistFieldEdit
{
    // A value changed this frame.
    bool changed = false;
    // The edit is complete (a checkbox click, or a slider released).
    bool commit = false;
};

MistFieldEdit DrawMistAppearanceFields(MistRenderSettings& settings);
