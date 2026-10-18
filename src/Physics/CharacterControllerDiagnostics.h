#pragma once

#include "Physics/CharacterControllerMover.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Physics
{
// Contacts copied per controller into a diagnostics snapshot; the total is still reported.
inline constexpr std::size_t MaximumCharacterDiagnosticContacts = 8;

// Read-only, value-owned copy of one controller's runtime state at a fixed-step boundary.
// No native handles or references into the session; world-space positions throughout.
struct CharacterControllerDiagnostics
{
    std::string objectID;
    // Accepted runtime pose (object origin, fixed authored yaw) and capsule geometry.
    Pose pose;
    Vector3 capsuleCenter;
    float capsuleRadius = 0.0f;
    float capsuleCylinderHeight = 0.0f;
    float verticalVelocity = 0.0f;
    // Ground support; point is in world space. groundObjectID is empty when airborne or
    // when the support is another controller or unregistered geometry.
    CharacterGroundState ground;
    std::string groundObjectID;
    // Fixed steps whose commands were last consumed (nullopt if none yet).
    std::optional<std::uint64_t> lastMovementCommandStep;
    std::optional<std::uint64_t> lastJumpRequestStep;
    // Outcome of the most recent fixed step.
    CharacterMoveStatus lastStatus = CharacterMoveStatus::NoMovement;
    CharacterJumpOutcome lastJumpOutcome = CharacterJumpOutcome::None;
    bool stepped = false;
    bool snapped = false;
    CharacterRecovery recovery = CharacterRecovery::NotNeeded;
    Vector3 carriedDisplacement;
    Vector3 recoveryDisplacement;
    // Moving body that carried the controller in the most recent step (empty if none).
    std::string carrierObjectID;
    // Solid constraints at the movement-accepted position: total, and the first few with
    // world-space points.
    std::size_t contactCount = 0;
    std::vector<MoverPlane> contacts;
    bool observerRegistered = false;
    bool ownsTriggerCollider = false;
};

// Diagnostics for every controller of one session at one published fixed-step boundary.
// sessionGeneration identifies the PhysicsSimulationController session that produced it.
struct CharacterDiagnosticsFrame
{
    std::uint64_t sessionGeneration = 0;
    std::uint64_t completedSteps = 0;
    std::vector<CharacterControllerDiagnostics> controllers;

    const CharacterControllerDiagnostics* Find(std::string_view objectID) const noexcept
    {
        for(const auto& controller : controllers)
        {
            if(controller.objectID == objectID)
            {
                return &controller;
            }
        }

        return nullptr;
    }
};
}
