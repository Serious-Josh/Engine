#include "Engine/CharacterDriveScheduling.h"
#include "Engine/PhysicsSimulationController.h"

#include <algorithm>

CharacterDriveSubmission SubmitCharacterDriveCommands(PhysicsSimulationController& simulation,
    const CharacterDriveIntent& intent, double elapsedSeconds)
{
    CharacterDriveSubmission submission;
    const std::size_t dueSteps = simulation.PredictStepsForUpdate(elapsedSeconds);

    if(dueSteps == 0)
    {
        return submission;
    }

    const std::uint64_t firstStep = simulation.GetNextFixedStep();
    const bool moving = intent.horizontalVelocity.x != 0.0f || intent.horizontalVelocity.z != 0.0f;

    // One movement intention per due step, capped so catch-up steps do not burst.
    if(moving)
    {
        const std::size_t commandedSteps = (std::min)(dueSteps, MaximumDiagnosticDriveStepsPerFrame);

        for(std::size_t offset = 0; offset < commandedSteps; ++offset)
        {
            if(!simulation.SubmitCharacterMovement(intent.sessionGeneration, intent.objectID, firstStep + offset,
                intent.horizontalVelocity, submission.error))
            {
                return submission;
            }

            ++submission.movementCommands;
        }
    }

    if(intent.jumpRequested)
    {
        submission.jumpSubmitted = simulation.SubmitCharacterJump(intent.sessionGeneration, intent.objectID, firstStep,
            submission.error);
    }

    return submission;
}
