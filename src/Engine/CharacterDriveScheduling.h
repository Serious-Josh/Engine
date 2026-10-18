#pragma once

#include "Math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <string>

class PhysicsSimulationController;

// Editor-only diagnostic movement intention for one controller (CM.9). Not a gameplay input
// contract: it exists so a developer can exercise the production command API by hand.
struct CharacterDriveIntent
{
    std::string objectID;
    std::uint64_t sessionGeneration = 0;
    // World-space horizontal velocity held this frame (y == 0); zero submits no movement.
    Vector3 horizontalVelocity;
    // One pending jump from a key press; submitted with the first step of a frame that runs one.
    bool jumpRequested = false;
};

struct CharacterDriveSubmission
{
    std::size_t movementCommands = 0;
    bool jumpSubmitted = false;
    std::string error;
};

// A long frame may run up to the controller's catch-up cap; the diagnostic drive commands
// at most this many of those steps so a stall cannot turn into a burst of held-key movement.
inline constexpr std::size_t MaximumDiagnosticDriveStepsPerFrame = 2;

// Submits commands for exactly the fixed steps the next Update(elapsedSeconds) will run,
// through the production session-generation-guarded API. Must be called immediately before
// that Update. A frame with no due step submits nothing and leaves the jump pending.
CharacterDriveSubmission SubmitCharacterDriveCommands(PhysicsSimulationController& simulation,
    const CharacterDriveIntent& intent, double elapsedSeconds);
