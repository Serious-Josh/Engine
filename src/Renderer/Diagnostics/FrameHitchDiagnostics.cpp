#include "Renderer/Diagnostics/FrameHitchDiagnostics.h"

#include "Core/Log.h"

#include <algorithm>
#include <chrono>
#include <string>

void RendererDiagnostics::HitchSampleWindow::Add(double milliseconds)
{
    samples[next] = milliseconds;
    next = (next + 1) % samples.size();
    count = (std::min)(count + 1, samples.size());
}

void RendererDiagnostics::FrameHitchDiagnostics::CommitFrame(
    CpuTiming::Clock::time_point now,
    CpuTiming::Clock::time_point frameStart,
    double frameMilliseconds,
    std::optional<FrameRenderResult> outcome,
    const CpuTiming::FrameSample& renderTiming,
    double hitchThresholdMilliseconds)
{
    auto& diagnostics = *this;
    const auto& snapshot = diagnostics.current;
    diagnostics.frames[diagnostics.nextFrame] = snapshot;
    diagnostics.nextFrame = (diagnostics.nextFrame + 1) % diagnostics.frames.size();
    diagnostics.frameCount = (std::min)(diagnostics.frameCount + 1, diagnostics.frames.size());

    // Missing/error outcomes interrupt a deferred streak but are not successful
    // render attempts. Minimized/message waits must not enter render statistics.

    if (outcome == FrameRenderResult::Deferred)
    {
        diagnostics.deferredDuration.Add(frameMilliseconds);

        if (diagnostics.consecutiveDeferred == 0)
        {
            diagnostics.deferredStart = frameStart;
        }

        ++diagnostics.consecutiveDeferred;

        diagnostics.maxConsecutiveDeferred =
            (std::max)(diagnostics.maxConsecutiveDeferred, diagnostics.consecutiveDeferred);
    }
    else
    {
        if (diagnostics.consecutiveDeferred != 0)
        {
            const double interval =
                std::chrono::duration<double, std::milli>(frameStart - diagnostics.deferredStart).count();
            diagnostics.maxDeferredIntervalMilliseconds =
                (std::max)(diagnostics.maxDeferredIntervalMilliseconds, interval);
            diagnostics.consecutiveDeferred = 0;
            diagnostics.deferredStart = {};
        }

        if (outcome == FrameRenderResult::Rendered)
        {
            diagnostics.renderedDuration.Add(frameMilliseconds);
            diagnostics.slotFenceWait.Add(renderTiming.fenceWait);

            if (diagnostics.previousRenderedStart != CpuTiming::Clock::time_point{})
            {
                diagnostics.renderedCadence.Add(
                    std::chrono::duration<double, std::milli>(frameStart - diagnostics.previousRenderedStart).count());
            }

            diagnostics.previousRenderedStart = frameStart;
        }
        else
        {
            diagnostics.previousRenderedStart = {}; // Exclude suspension/errors from cadence.
        }
    }

    if (now - diagnostics.lastReport >= std::chrono::seconds(2))
    {
        Report(now, hitchThresholdMilliseconds);
        diagnostics.lastReport = now;
    }
}

void RendererDiagnostics::FrameHitchDiagnostics::Report(
    CpuTiming::Clock::time_point now,
    double hitchThresholdMilliseconds) const
{
    const auto describe = [hitchThresholdMilliseconds](const HitchSampleWindow& window)
    {
        if (window.count == 0)
        {
            return std::string("n=0");
        }

        auto sorted = window.samples;
        std::sort(sorted.begin(), sorted.begin() + window.count);

        double sum = 0.0;

        size_t hitches = 0;

        for (size_t index = 0; index < window.count; ++index)
        {
            sum += sorted[index];
            hitches += sorted[index] >= hitchThresholdMilliseconds ? 1 : 0;
        }

        // Nearest-rank percentiles over the latest bounded window.
        return "n=" + std::to_string(window.count) +

               ", avg=" + std::to_string(sum / static_cast<double>(window.count)) +
               ", p95=" + std::to_string(sorted[(window.count * 95 + 99) / 100 - 1]) +
               ", p99=" + std::to_string(sorted[(window.count * 99 + 99) / 100 - 1]) +
               ", max=" + std::to_string(sorted[window.count - 1]) + ", hitches=" + std::to_string(hitches);
    };
    const auto& diagnostics = *this;
    const double activeDeferredMilliseconds =
        diagnostics.consecutiveDeferred == 0
            ? 0.0
            : std::chrono::duration<double, std::milli>(now - diagnostics.deferredStart).count();

    Log::Info("Frame hitch diagnostics: CPU wall-clock ms; latest 256 samples per metric; hitch >= " +
              std::to_string(hitchThresholdMilliseconds) + "; rendered Frame() {" +
              describe(diagnostics.renderedDuration) + "}; deferred Frame() {" +
              describe(diagnostics.deferredDuration) + "}; rendered start cadence {" +
              describe(diagnostics.renderedCadence) + "}; slot fence blocking {" + describe(diagnostics.slotFenceWait) +
              "}; patch+compute recording {" + describe(diagnostics.computeRecording) + "}; BLAS recording {" +
              describe(diagnostics.blasRecording) + "}; merged TLAS recording {" + describe(diagnostics.tlasRecording) +
              "}; submit-return to completion-observed {" + describe(diagnostics.submissionToObservedCompletion) +
              "}; completion-observed to finalized {" + describe(diagnostics.observedCompletionToFinalization) +
              "}; deferred streak current=" + std::to_string(diagnostics.consecutiveDeferred) +
              ", elapsed=" + std::to_string(activeDeferredMilliseconds) +
              ", maxCount=" + std::to_string(diagnostics.maxConsecutiveDeferred) +
              ", maxInterval=" + std::to_string(diagnostics.maxDeferredIntervalMilliseconds) + ".");

    std::array<size_t, 256> worst{};

    size_t captured = 0;

    for (size_t index = 0; index < diagnostics.frameCount; ++index)
    {
        if (diagnostics.frames[index].outcome.has_value())
        {
            worst[captured++] = index;
        }
    }

    std::sort(worst.begin(), worst.begin() + captured, [&](size_t left, size_t right)
              { return diagnostics.frames[left].execution > diagnostics.frames[right].execution; });

    const auto milliseconds = [](std::optional<double> value)
    { return value ? std::to_string(*value) : std::string("n/a"); };

    for (size_t rank = 0; rank < (std::min)(captured, size_t{3}); ++rank)
    {
        const auto& frame = diagnostics.frames[worst[rank]];

        const char* outcome = *frame.outcome == FrameRenderResult::Rendered   ? "Rendered"
                              : *frame.outcome == FrameRenderResult::Deferred ? "Deferred"
                                                                              : "Error";
        const auto& render = frame.render;
        Log::Info(
            "CPU hitch snapshot: engineFrame=" + std::to_string(frame.engineSerial) + ", renderSerial=" +
            (render ? std::to_string(render->frameSerial) : std::string("n/a")) + ", outcome=" + outcome +
            ", slot=" + (frame.slot != UINT_MAX ? std::to_string(frame.slot) : std::string("n/a")) +
            ", revision=" + std::to_string(frame.revision) + ", phase=" + frame.phaseStart + "->" + frame.phaseEnd +
            ", events[accepted/recorded/submitted/completed/finalized]=" + std::to_string(frame.accepted) + "/" +
            std::to_string(frame.recorded) + "/" + std::to_string(frame.submitted) + "/" +
            std::to_string(frame.completed) + "/" + std::to_string(frame.finalized) + "; CPU wall-clock ms: Frame=" +
            std::to_string(frame.execution) + ", cadence=" + milliseconds(frame.cadence) +
            ", fenceWait=" + milliseconds(render ? std::optional<double>{render->fenceWait} : std::nullopt) +
            ", compute=" + milliseconds(frame.compute) + ", BLAS=" + milliseconds(frame.blas) +
            ", TLAS=" + milliseconds(frame.tlas) + ", requestAPI=" +
            milliseconds(frame.requestCalled ? std::optional<double>{frame.requestMilliseconds} : std::nullopt) +
            ", completionPolls=" +
            milliseconds(frame.completionCalled ? std::optional<double>{frame.completionMilliseconds} : std::nullopt) +
            ", recording=" + milliseconds(render ? std::optional<double>{render->recording} : std::nullopt) +
            ", Execute=" + milliseconds(render ? std::optional<double>{render->execute} : std::nullopt) +
            ", Present=" + milliseconds(render ? std::optional<double>{render->present} : std::nullopt) + ".");

        const auto region = [&](FrameHitchRegion name) { return frame.regions[static_cast<size_t>(name)]; };
        const auto value = [&](FrameHitchRegion name) { return region(name).value_or(0.0); };
        // Only disjoint Engine spans enter Frame residual. In particular,
        // EngineRender contains every Renderer/recording/Execute child below.
        double engineCovered = 0.0;

        for (size_t index = 0; index <= static_cast<size_t>(FrameHitchRegion::EngineRender); ++index)
        {
            engineCovered += frame.regions[index].value_or(0.0);
        }

        std::optional<double> rendererResidual;
        std::optional<double> recordingResidual;
        std::optional<double> displacementPreparation;
        std::optional<double> executeResidual;

        if (render && region(FrameHitchRegion::EngineRender))
        {
            rendererResidual = *region(FrameHitchRegion::EngineRender) - value(FrameHitchRegion::Admission) -

                               value(FrameHitchRegion::InitialValidation) - value(FrameHitchRegion::PreBegin) -
                               render->begin - value(FrameHitchRegion::PostBegin) - render->recording - render->end -
                               render->execute - value(FrameHitchRegion::PostSubmission) - render->present -
                               value(FrameHitchRegion::PostPresent);
            recordingResidual = render->recording - value(FrameHitchRegion::Profiler) -
                                value(FrameHitchRegion::RecordingSetup) - value(FrameHitchRegion::Displacement) -
                                value(FrameHitchRegion::OrdinaryPasses) - value(FrameHitchRegion::DebugUI);

            if (frame.executeValidation && frame.executeCommands && frame.executeSignal && frame.executeAssociation)
            {
                executeResidual = render->execute - *frame.executeValidation - *frame.executeCommands -
                                  *frame.executeSignal - *frame.executeAssociation;
            }
        }

        if (region(FrameHitchRegion::Displacement) && frame.compute && frame.blas && frame.tlas)
        {
            displacementPreparation =

                *region(FrameHitchRegion::Displacement) - *frame.compute - *frame.blas - *frame.tlas;
        }

        std::optional<double> phaseLoggingTotal;

        for (size_t index = static_cast<size_t>(FrameHitchRegion::CameraPhaseLogging);
             index < static_cast<size_t>(FrameHitchRegion::Count); ++index)
        {
            if (frame.regions[index])
            {
                phaseLoggingTotal = phaseLoggingTotal.value_or(0.0) + *frame.regions[index];
            }
        }

        Log::Info(

            "CPU hitch breakdown engineFrame=" + std::to_string(frame.engineSerial) +
            "; additive Engine ms: input/messages=" + milliseconds(region(FrameHitchRegion::EngineInput)) +

            ", UI/editor/update=" + milliseconds(region(FrameHitchRegion::EngineUI)) +
            ", completion/delivery/ack=" + milliseconds(region(FrameHitchRegion::EngineCompletion)) +
            ", environment=" + milliseconds(region(FrameHitchRegion::EngineEnvironment)) +
            ", sceneSwitch=" + milliseconds(region(FrameHitchRegion::EngineSwitch)) +
            ", sceneRebuild=" + milliseconds(region(FrameHitchRegion::EngineRebuild)) +
            ", cameraRequestBlock=" + milliseconds(region(FrameHitchRegion::EngineRequest)) +
            ", RenderCall=" + milliseconds(region(FrameHitchRegion::EngineRender)) +
            ", uncovered=" + std::to_string(frame.execution - engineCovered) +
            "; nested RenderCall ms: admission=" + milliseconds(region(FrameHitchRegion::Admission)) +
            ", initialValidation=" + milliseconds(region(FrameHitchRegion::InitialValidation)) +
            ", preBegin=" + milliseconds(region(FrameHitchRegion::PreBegin)) +
            ", Begin=" + milliseconds(render ? std::optional<double>{render->begin} : std::nullopt) +
            " [nested reset=" + milliseconds(render ? std::optional<double>{render->commandReset} : std::nullopt) +
            ", fenceWait=see snapshot], postBegin=" + milliseconds(region(FrameHitchRegion::PostBegin)) +
            ", End=" + milliseconds(render ? std::optional<double>{render->end} : std::nullopt) +
            ", postSubmit=" + milliseconds(region(FrameHitchRegion::PostSubmission)) + ", postPresent=" +
            milliseconds(region(FrameHitchRegion::PostPresent)) + ", uncovered=" + milliseconds(rendererResidual) +
            "; nested recording ms: profiler=" + milliseconds(region(FrameHitchRegion::Profiler)) +
            ", setup=" + milliseconds(region(FrameHitchRegion::RecordingSetup)) +
            ", displacementTotal=" + milliseconds(region(FrameHitchRegion::Displacement)) +
            " [nested preparation/bookkeeping=" + milliseconds(displacementPreparation) +
            ", compute/BLAS/TLAS=see snapshot], ordinaryPasses=" +
            milliseconds(region(FrameHitchRegion::OrdinaryPasses)) + ", DebugUI.EndFrame=" +
            milliseconds(region(FrameHitchRegion::DebugUI)) + ", uncovered=" + milliseconds(recordingResidual) +
            "; nested Execute ms: validation=" + milliseconds(frame.executeValidation) + ", ExecuteCommandLists=" +
            milliseconds(frame.executeCommands) + ", Signal=" + milliseconds(frame.executeSignal) +
            ", association/bookkeeping/logging=" + milliseconds(frame.executeAssociation) +
            ", uncovered=" + milliseconds(executeResidual) +
            "; nested phase logging ms (already included in parent scopes): total=" + milliseconds(phaseLoggingTotal) +
            ", camera [UI/update]=" + milliseconds(region(FrameHitchRegion::CameraPhaseLogging)) +
            " (requestedRevision=" + std::to_string(frame.cameraLoggingRevision) + ")" +
            ", accepted [cameraRequestBlock]=" + milliseconds(region(FrameHitchRegion::AcceptedPhaseLogging)) +
            ", recorded [displacementPreparation]=" + milliseconds(region(FrameHitchRegion::RecordedPhaseLogging)) +
            ", submitted [Execute association]=" + milliseconds(region(FrameHitchRegion::SubmittedPhaseLogging)) +
            ", completion+summary [completionPolls]=" + milliseconds(region(FrameHitchRegion::CompletionPhaseLogging)) +
            ", delivery/ack [EngineCompletion]=" + milliseconds(region(FrameHitchRegion::AcknowledgmentPhaseLogging)) +
            "; UI textureUploadRequested=" +
            (frame.uiTextureObserved ? std::to_string(frame.uiTextureUploadRequested) : std::string("n/a")) +
            ", UI bufferGrowth=n/a.");
    }
}

void RendererDiagnostics::FrameHitchDiagnostics::AddCompletedDisplacementSample(
    double computeMilliseconds,
    double blasMilliseconds,
    double tlasMilliseconds,
    double submissionToObservedMilliseconds,
    double observedToFinalizedMilliseconds)
{
    computeRecording.Add(computeMilliseconds);
    blasRecording.Add(blasMilliseconds);
    tlasRecording.Add(tlasMilliseconds);
    submissionToObservedCompletion.Add(submissionToObservedMilliseconds);
    observedCompletionToFinalization.Add(observedToFinalizedMilliseconds);
}
