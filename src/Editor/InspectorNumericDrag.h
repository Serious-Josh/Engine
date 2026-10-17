#pragma once

// Transform-style numeric Inspector control. It wraps ImGui's drag widget, the same
// mechanism as the Transform Position/Rotation/Scale controls:
//   - click and drag horizontally to adjust;
//   - double-click (or Ctrl+click) to type a precise value;
//   - Enter or focus loss completes the edit;
//   - Escape cancels and restores the value the field had when the edit began.
// The caller applies or records the value only when the result is Committed, so one
// drag or one typed entry produces exactly one change.
enum class InspectorDragResult
{
    Idle,
    Editing,
    Committed,
    Cancelled
};

struct InspectorDragSpec
{
    // Value change per pixel of horizontal mouse movement.
    float speed = 0.01f;

    // Drag limits only. Typed values are never clamped here unless clampTypedInput is
    // set, so callers keep their existing validation and error feedback.
    float minimum = 0.0f;
    float maximum = 0.0f;
    bool clampTypedInput = false;

    const char* format = "%.6g";
};

// Edits componentCount consecutive floats (1 to 4) in place.
InspectorDragResult InspectorDragFloats(const char* label, float* values, int componentCount,
    const InspectorDragSpec& spec);

// A single-value field as wide as one component of a Transform Position/Rotation/Scale
// control, with its label directly after the box at the same ItemInnerSpacing.x gap a
// Transform label uses. The width follows the current item width, so it tracks
// Inspector resizing. The widget ID is PushID(label) + "##value".
InspectorDragResult InspectorDragFloatTransformWidth(const char* label, float& value, const InspectorDragSpec& spec);
