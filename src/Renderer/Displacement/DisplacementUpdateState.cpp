#include "Renderer/Displacement/DisplacementUpdateState.h"

bool RendererDisplacement::PendingDisplacementUpdate::ResetResolved()
{
    const bool canReset = status == DisplacementUpdateStatus::Empty || status == DisplacementUpdateStatus::Completed ||
                          status == DisplacementUpdateStatus::Discarded;

    const bool recoveryResolved =
        frameRecovery.state == DisplacementFrameExitState::Empty ||
        frameRecovery.state == DisplacementFrameExitState::Discarded ||
        (status == DisplacementUpdateStatus::Completed && frameRecovery.state == DisplacementFrameExitState::Submitted);

    if (!canReset || !recoveryResolved)
    {
        return false;
    }

    const bool ownsSubmission = submission.fenceValue != 0 || submission.lifetime != 0 || submission.retentionID != 0;

    const bool ownsReservation = reservation.lifetime != 0 || reservation.reservationID != 0 ||
                                 reservationOwnership != DisplacementReservationOwnership::None;

    if (ownsSubmission || ownsReservation)
    {
        return false;
    }

    scene = nullptr;
    sceneRevision = 0;

    sceneResourceLifetime = 0;
    requestRevision = 0;
    mesh = nullptr;
    targetCounts = {};
    patchLODs.clear();
    settings = {};
    layout = {};
    preparedPatchResource = nullptr;
    preparedPatchDescriptor = UINT_MAX;

    frameSlot = UINT_MAX;
    recordingGeneration = 0;
    recordingStage = DisplacementRecordingStage::None;
    historyPreparation = DisplacementHistoryPreparation::NotRequested;

    for (DisplacementTLASPreparation& preparation : tlasPreparation)
    {
        preparation = DisplacementTLASPreparation::NotRequested;
    }

    retirementBatch = {};
    replacementBLASAddress = 0;

    frameRecovery = {};

    for (DisplacementTLASUse& use : tlasUses)
    {
        use = {};
    }

    status = DisplacementUpdateStatus::Empty;
    return true;
}
