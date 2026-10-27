#pragma once

#include "Debug/CpuFrameTiming.h"
#include "Debug/EditorPhaseTiming.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace RendererDiagnostics
{
    enum class FrameRenderResult
    {
        Rendered,
        Deferred,
        Error
    };

    enum class FrameHitchRegion : size_t
    {
        EngineInput,
        EngineUI,
        EngineCompletion,
        EngineEnvironment,
        EngineSwitch,
        EngineRebuild,
        EngineRequest,
        EngineRender,
        Admission,
        InitialValidation,
        PreBegin,
        PostBegin,
        Profiler,
        RecordingSetup,
        Displacement,
        OrdinaryPasses,
        DebugUI,
        PostSubmission,
        PostPresent,
        CameraPhaseLogging,
        AcceptedPhaseLogging,
        RecordedPhaseLogging,
        SubmittedPhaseLogging,
        CompletionPhaseLogging,
        AcknowledgmentPhaseLogging,
        Count
    };

    struct HitchSampleWindow
    {
        std::array<double, 256> samples{};

        size_t count = 0;
        size_t next = 0;

        void Add(double milliseconds);
    };

    struct FrameHitchSnapshot
    {
        std::uint64_t engineSerial = 0;

        std::optional<FrameRenderResult> outcome;

        double execution = 0.0;

        std::optional<double> cadence;

        unsigned int slot = UINT_MAX;

        std::uint64_t revision = 0;

        const char* phaseStart = "idle";
        const char* phaseEnd = "idle";

        std::uint64_t cameraLoggingRevision = 0;

        bool accepted = false;
        bool recorded = false;
        bool submitted = false;
        bool completed = false;

        bool finalized = false;
        bool requestCalled = false;
        bool completionCalled = false;

        double requestMilliseconds = 0.0;
        double completionMilliseconds = 0.0; // Sum of all polls during this frame.

        std::optional<double> compute;
        std::optional<double> blas;
        std::optional<double> tlas;

        std::optional<CpuTiming::FrameSample> render;

        std::array<std::optional<double>, static_cast<size_t>(FrameHitchRegion::Count)> regions{};

        CpuTiming::Clock::time_point contextStart{};
        CpuTiming::Clock::time_point contextEnd{};

        std::optional<double> executeValidation;
        std::optional<double> executeCommands;
        std::optional<double> executeSignal;
        std::optional<double> executeAssociation;

        bool uiTextureObserved = false;
        bool uiTextureUploadRequested = false;

        // Editor::Draw's phases (ENGINE_FRAME_HITCH_LOGGING=1 only); empty in frames without a Draw.
        EditorPhaseTiming::Frame editorPhases{};
    };

    struct FrameHitchDiagnostics
    {
        bool loggingEnabled = false;

        FrameHitchSnapshot current{};

        std::array<FrameHitchSnapshot, 256> frames{};

        size_t frameCount = 0;
        size_t nextFrame = 0;

        HitchSampleWindow renderedDuration;
        HitchSampleWindow deferredDuration;
        HitchSampleWindow renderedCadence;
        HitchSampleWindow slotFenceWait;

        HitchSampleWindow computeRecording;
        HitchSampleWindow blasRecording;
        HitchSampleWindow tlasRecording;
        HitchSampleWindow submissionToObservedCompletion;

        HitchSampleWindow observedCompletionToFinalization;

        // Frames with an Editor::Draw sample: EngineUI, EngineUI without Draw, and Draw without its top-level phases
        // all come from those frames. Each phase window holds only the frames in which that phase ran.
        HitchSampleWindow engineUI;
        HitchSampleWindow engineUIOutsideEditor;
        HitchSampleWindow editorRemainder;
        std::array<HitchSampleWindow, EditorPhaseTiming::PhaseCount> editorPhases{};
        // A nested parent phase (one with children) without its direct children, on the frames the parent ran.
        std::array<HitchSampleWindow, EditorPhaseTiming::PhaseCount> editorPhaseRemainders{};

        CpuTiming::Clock::time_point lastReport{};
        CpuTiming::Clock::time_point previousRenderedStart{};
        CpuTiming::Clock::time_point deferredStart{};

        std::uint64_t consecutiveDeferred = 0;
        std::uint64_t maxConsecutiveDeferred = 0;

        double maxDeferredIntervalMilliseconds = 0.0;

        // The caller finalizes current at its existing measurement boundaries.
        // Borrowed inputs are used only during commitment/reporting.
        void CommitFrame(
            CpuTiming::Clock::time_point now,
            CpuTiming::Clock::time_point frameStart,
            double frameMilliseconds,
            std::optional<FrameRenderResult> outcome,
            const CpuTiming::FrameSample& renderTiming,
            double hitchThresholdMilliseconds);

        void AddCompletedDisplacementSample(
            double computeMilliseconds,
            double blasMilliseconds,
            double tlasMilliseconds,
            double submissionToObservedMilliseconds,
            double observedToFinalizedMilliseconds);

        void Report(
            CpuTiming::Clock::time_point now,
            double hitchThresholdMilliseconds) const;
    };
}
