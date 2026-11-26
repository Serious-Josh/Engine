// FG.2: where a frame's final image goes, and the switch to DLSS Frame Generation's present path (FramePresenter). With the
// path off, everything here does what Render and RenderUIOnly did before: the swap chain's current back buffer, waiting in
// PRESENT between frames, presented from the frame queue, and the frame slot re-read from the swap chain after each Present.

#include "Renderer.h"

#include "Core/Log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{
    // FG.2 diagnostics: ENGINE_FG_PRESENT_TEST=1 presents every Editor frame through the Frame Generation present path, with
    // the previous frame as the stand-in generated frame; "toggle" also switches it off and on every kToggleFrames frames, for
    // the switching lifecycle. Either logs the present spacing every kLogFrames frames.
    enum class FramePresentationTest
    {
        Off,
        On,
        Toggle
    };

    constexpr std::uint64_t kToggleFrames = 600;
    constexpr std::uint64_t kLogFrames = 300;

    // With the test on, ENGINE_FG_PRESENT_TEST_FRAME_MS=<ms> holds each frame to at least that period (a sleep before it
    // records), so the present thread's mid-interval timing runs at a real frame rate far below the display's.
    double GetTestFramePeriodMilliseconds()
    {
        static const double period = []
        {
            const char* value = std::getenv("ENGINE_FG_PRESENT_TEST_FRAME_MS");
            return value ? std::atof(value) : 0.0;
        }();

        return period;
    }

    FramePresentationTest GetFramePresentationTest()
    {
        static const FramePresentationTest test = []
        {
            const char* value = std::getenv("ENGINE_FG_PRESENT_TEST");
            const std::string text = value ? value : "";

            if(text == "1")
            {
                return FramePresentationTest::On;
            }

            if(text == "toggle")
            {
                return FramePresentationTest::Toggle;
            }

            return FramePresentationTest::Off;
        }();

        return test;
    }

    // FG.3 diagnostics: ENGINE_FRAME_GENERATION=1 requests DLSS Frame Generation whatever the project saved.
    bool IsFrameGenerationForcedForTesting()
    {
        static const bool forced = []
        {
            const char* value = std::getenv("ENGINE_FRAME_GENERATION");
            return value && std::string(value) == "1";
        }();

        return forced;
    }

    // NGX's camera matrices are row-major for row vectors (v * M); Engine's are column-vector matrices stored row-major, so
    // the transpose has the layout NGX reads (the same conversion NRD's matrices get).
    void StoreRowVectorMatrix(float destination[4][4], const Matrix4& matrix)
    {
        const Matrix4 transposed = matrix.Transpose();
        std::memcpy(destination, transposed.m, sizeof(transposed.m));
    }

    void StoreVector(float destination[3], const Vector3& vector)
    {
        destination[0] = vector.x;
        destination[1] = vector.y;
        destination[2] = vector.z;
    }
}

void Renderer::SetFrameGenerationRequested(bool requested)
{
    // Cooked-game port: a packaged game follows its saved settings only (no diagnostics override).
    if(m_packagedRuntime)
    {
        m_frameGenerationRequested = requested;
        return;
    }

    m_frameGenerationRequested = m_frameGenerationRequestForTesting.value_or(requested || IsFrameGenerationForcedForTesting());
}

bool Renderer::SetFrameGenerationPresentation(bool enabled)
{
    if(enabled == m_frameGenerationPresentation)
    {
        return true;
    }

    // Stage 1: nothing in flight on either queue while the swap chain moves between them. Stop presents what was submitted.
    if(m_frameGenerationPresentation)
    {
        m_framePresenter.Stop();
    }

    if(!m_commandContext.WaitForGPU())
    {
        m_resizeFailed = true;
        Log::Error("Frame Generation present path: GPU completion is unconfirmed; rendering stops.");
        return false;
    }

    std::string failure;

    if(enabled && !m_framePresenter.Initialize(m_graphicsDevice.GetDevice(), failure))
    {
        // Nothing changed yet: the frame queue's swap chain still presents.
        Log::Error("Frame Generation present path: " + failure + "; presentation stays as it was.");
        return false;
    }

    // Stage 2: a swap chain on the queue that presents from now on (three buffers for two presents per frame).
    ID3D12CommandQueue* const queue = enabled ? m_framePresenter.GetQueue() : m_commandContext.GetCommandQueue();
    const unsigned int bufferCount = enabled ? SwapChain::MaximumBufferCount : 2u;
    bool switched = RecreateSwapChain(queue, bufferCount);

    // Stage 3: the present thread.
    if(switched && enabled)
    {
        switched = m_framePresenter.Start(m_swapChain, m_commandContext.GetFence(), m_applicationWidth, m_applicationHeight,
            DXGI_FORMAT_R8G8B8A8_UNORM, failure);

        if(!switched)
        {
            Log::Error("Frame Generation present path: " + failure + ".");
        }
    }

    if(!switched)
    {
        // Back to ordinary presentation; without even that swap chain, rendering stops as after a failed resize.
        m_frameGenerationPresentation = false;

        if(!RecreateSwapChain(m_commandContext.GetCommandQueue(), 2u))
        {
            m_resizeFailed = true;
        }

        SyncFrameSlotToSwapChain();
        return false;
    }

    m_frameGenerationPresentation = enabled;
    m_framePresentationFailureLogged = false;

    if(!enabled)
    {
        SyncFrameSlotToSwapChain();
    }

    m_gpuTimestampProfiler.BreakFrameSequence();
    ResetCpuSubmissionCadence();
    Log::Info(std::string("Frame Generation present path ") + (enabled ? "on" : "off") + " (" +
        std::to_string(m_applicationWidth) + "x" + std::to_string(m_applicationHeight) + ", " + std::to_string(bufferCount) +
        " swap-chain buffers).");
    return true;
}

bool Renderer::RecreateSwapChain(ID3D12CommandQueue* queue, unsigned int bufferCount)
{
    // DXGI allows one swap chain per window: the old one and its buffers are released first (the GPU is idle).
    m_swapChain.Shutdown();

    if(!m_swapChain.Initialize(m_graphicsDevice.GetFactory(), m_graphicsDevice.GetDevice(), queue, m_windowHandle,
        m_applicationWidth, m_applicationHeight, bufferCount))
    {
        Log::Error("Frame Generation present path: the swap chain could not be created with " + std::to_string(bufferCount) +
            " buffers.");
        return false;
    }

    return true;
}

bool Renderer::HasPresentationTarget() const
{
    // While the present thread owns the swap chain, the render thread does not read it.
    if(m_frameGenerationPresentation)
    {
        return m_framePresenter.IsStarted() && !m_framePresenter.HasFailed();
    }

    return m_swapChain.GetCurrentBackBuffer() != nullptr;
}

bool Renderer::AcquirePresentationTarget(PresentationTarget& target, bool withGeneratedFrame)
{
    target = {};

    if(!m_frameGenerationPresentation)
    {
        target.resource = m_swapChain.GetCurrentBackBuffer();
        target.rtv = m_swapChain.GetCurrentRTV();
        target.idleState = D3D12_RESOURCE_STATE_PRESENT;
        return target.resource != nullptr;
    }

    const auto acquire = [&](FramePresenter::Composite& composite)
    {
        if(m_framePresenter.AcquireComposite(composite))
        {
            return true;
        }

        if(!m_framePresentationFailureLogged)
        {
            Log::Error("Frame Generation present path stopped: " + m_framePresenter.GetFailure() + ".");
            m_framePresentationFailureLogged = true;
        }

        return false;
    };

    FramePresenter::Composite composite;

    if(!acquire(composite))
    {
        return false;
    }

    target.resource = composite.resource;
    target.rtv = composite.rtv;
    target.idleState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    target.composite = composite.index;

    // FG.3: the generated frame's composite, used only if this recording renders a generated frame.
    if(withGeneratedFrame && IsFrameGenerationRunning())
    {
        FramePresenter::Composite generated;

        if(!acquire(generated))
        {
            return false;
        }

        target.generatedComposite = generated.index;
        target.generatedResource = generated.resource;
        target.generatedRtv = generated.rtv;
    }

    return true;
}

bool Renderer::IsFrameGenerationRunning() const
{
    return m_frameGenerationRequested && m_frameGenerationPresentation &&
        m_dlssBackend.GetStatus().frameGeneration.availability == DlssFrameGenerationAvailability::Available;
}

bool Renderer::PresentTarget(const PresentationTarget& target)
{
    if(!target.composite)
    {
        const bool presented = m_swapChain.Present();
        SyncFrameSlotToSwapChain();
        return presented;
    }

    // FG.3: this frame's generated frame when the recording rendered one; the FG.2 diagnostics' stand-in (the previous real
    // frame) while the test runs without Frame Generation; otherwise only the real frame.
    std::optional<unsigned int> generated;

    if(target.generatedRecorded)
    {
        generated = target.generatedComposite;
    }
    else if(!m_frameGenerationRequested && !m_packagedRuntime && GetFramePresentationTest() != FramePresentationTest::Off)
    {
        generated = m_framePresenter.GetLastSubmittedComposite();
    }

    // The recording that rendered the composite was just submitted on this frame slot: the slot's fence value is the frame's
    // completion, which the present thread waits for before its copy (the only ordering between the two queues).
    const std::uint64_t frameFenceValue = m_commandContext.GetFrameFenceValue(m_frameSlot);

    if(frameFenceValue == 0)
    {
        Log::Error("Frame Generation present path: the frame's fence value is unknown; the frame is not presented.");
        return false;
    }

    m_framePresenter.Submit(*target.composite, frameFenceValue, generated);
    // The two frame slots alternate, as the two-buffer swap chain's back-buffer index did.
    m_frameSlot = (m_frameSlot + 1) % CommandContext::FrameCount;
    return !m_framePresenter.HasFailed();
}

bool Renderer::CompletePresentation(const PresentationTarget& target)
{
    // NVIDIA Reflex (no NVAPI call while it is off): submission ends here; the markers wrap the Present call, or the hand-over
    // to the Frame Generation present thread.
    m_reflexLatency.MarkPresentStart();
    const auto presentStart = CpuTiming::Clock::now();
    const bool presented = PresentTarget(target);
    m_cpuFrameTiming.present = CpuTiming::MillisecondsSince(presentStart);
    m_reflexLatency.MarkPresentEnd();
    return presented;
}

void Renderer::ApplyFramePresentationRequest()
{
    // Stage 1: FG.3, the saved toggle (since the cooked-game port, a packaged game's too). NGX initializes for it as for a
    // DLSS request (on its own thread, adopted here once done); the path follows the toggle while Frame Generation is
    // available.
    bool wanted = false;

    if(m_frameGenerationRequested)
    {
        BeginDlssBackend(true);
        PollDlssBackend();
        wanted = m_dlssBackend.GetStatus().frameGeneration.availability == DlssFrameGenerationAvailability::Available;
    }

    // Stage 2: the FG.2 diagnostics, which also run the path without Frame Generation (stand-in generated frames). Never in
    // a packaged game.
    const FramePresentationTest test = m_packagedRuntime ? FramePresentationTest::Off : GetFramePresentationTest();

    if(test != FramePresentationTest::Off)
    {
        ++m_framePresentationTestFrames;
        wanted = test == FramePresentationTest::On || (m_framePresentationTestFrames / kToggleFrames) % 2 == 0;
    }

    if(wanted != m_frameGenerationPresentation && m_applicationWidth != 0 && !m_resizeFailed)
    {
        SetFrameGenerationPresentation(wanted);
        // Whatever the switch did, the next generated frame starts a new history.
        m_frameGenerationResetPending = true;

        // Switching off stops presenting through Frame Generation but keeps its feature and images (about 470 MiB) for the
        // next switch on. Releasing that much GPU-used memory can hang the next first traversals of new acceleration
        // structures on the RTX 4080 with driver 621.01 (Documentation/Rendering/FgTiming_ManyLight_Switch_Hangs.md). They
        // go with an extent change, an NGX recovery or the DLSS teardown, as before.
    }

    if(!IsFrameGenerationRunning())
    {
        m_frameGenerationEvaluatedLastFrame = false;
        m_frameGenerationResetPending = true;
    }
    else if(m_outputWidth != 0 && FrameGenerationImagesNeeded())
    {
        // Stage 3: the generated viewport image and the HUD-less copy at the output extent, (re)created between frames once
        // nothing in flight uses the old ones (a viewport resize, or the first Frame Generation frame). A failure is not
        // retried for the same extent. A packaged game draws no UI over its scene image, so it has no HUD-less copy.
        m_frameGenerationImageAttemptWidth = m_outputWidth;
        m_frameGenerationImageAttemptHeight = m_outputHeight;
        ID3D12Device* const device = m_graphicsDevice.GetDevice();
        const bool drained = m_frameGenerationOutput.GetResource() == nullptr || m_commandContext.WaitForGPU();
        m_frameGenerationOutput.Shutdown();
        m_frameGenerationHudless.Shutdown();
        const bool created = drained &&
            m_frameGenerationOutput.Initialize(device, m_outputWidth, m_outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                m_rtvDescriptorHeap, m_resourceDescriptorHeap, {0.0f, 0.0f, 0.0f, 1.0f},
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            (m_packagedRuntime || m_frameGenerationHudless.Initialize(device, m_outputWidth, m_outputHeight,
                DXGI_FORMAT_R8G8B8A8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap));

        m_frameGenerationImagesFailed = !created;

        if(!created)
        {
            m_frameGenerationOutput.Shutdown();
            m_frameGenerationHudless.Shutdown();
            m_frameGenerationInactiveReason = "the generated-frame images could not be created";
            Log::Error("DLSS Frame Generation: the generated-frame images could not be created at " +
                std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight) + ".");
        }

        m_frameGenerationResetPending = true;
    }

    if(test == FramePresentationTest::Off)
    {
        return;
    }

    const double period = GetTestFramePeriodMilliseconds();

    if(period > 0.0)
    {
        static CpuTiming::Clock::time_point lastFrame = CpuTiming::Clock::now();
        const auto next = lastFrame + std::chrono::duration_cast<CpuTiming::Clock::duration>(
            std::chrono::duration<double, std::milli>(period));
        std::this_thread::sleep_until(next);
        lastFrame = CpuTiming::Clock::now();
    }

    if(m_frameGenerationPresentation && m_framePresentationTestFrames % kLogFrames == 0)
    {
        const FramePresenter::Statistics statistics = m_framePresenter.GetStatistics();
        Log::Info("FG present test: " + std::to_string(statistics.presents) + " presents (" +
            std::to_string(statistics.generatedPresents) + " generated); window " + std::to_string(statistics.intervals) +
            " intervals: generated->real " + std::to_string(statistics.meanGeneratedToReal) + " ms, real->generated " +
            std::to_string(statistics.meanRealToGenerated) + " ms, real->real " + std::to_string(statistics.realToReal) +
            ", mean " + std::to_string(statistics.meanInterval) + " ms, deviation " +
            std::to_string(statistics.standardDeviation) + " ms, maximum " + std::to_string(statistics.maximumInterval) +
            " ms; real frame " + std::to_string(statistics.realFrameInterval) + " ms.");
    }
}

void Renderer::RecordFrameGenerationHudless(ID3D12GraphicsCommandList* commandList, PresentationTarget& target)
{
    target.hudlessRecorded = false;

    if(!target.generatedComposite || !m_frameGenerationHudless.GetResource() ||
        m_frameGenerationHudless.GetWidth() != m_finalSceneColor.GetWidth() ||
        m_frameGenerationHudless.GetHeight() != m_finalSceneColor.GetHeight())
    {
        return;
    }

    // The final scene image before any Editor overlay (grid, colliders, AIR views, voxel cubes, unlit mist): Frame Generation
    // compares it with the finished image, treats the difference as UI and does not warp it along the surfaces beneath.
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_frameGenerationHudless.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(m_frameGenerationHudless.GetResource(), m_finalSceneColor.GetResource());
    m_frameGenerationHudless.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    target.hudlessRecorded = true;
}

bool Renderer::RecordFrameGeneration(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex,
    const RenderView& frameView, const Matrix4& previousViewProjection, const PresentationTarget& target)
{
    m_frameGenerationInactiveReason.clear();

    // Stage 1: what the generated frame needs this frame.
    const UpscalerFrameInputs& inputs = m_upscalerInputs;
    const bool imagesReady = m_frameGenerationOutput.GetResource() && m_frameGenerationOutput.GetWidth() == m_outputWidth &&
        m_frameGenerationOutput.GetHeight() == m_outputHeight;

    if(!target.generatedComposite)
    {
        m_frameGenerationInactiveReason = "no composite for the generated frame";
        return false;
    }

    if(!inputs.prepared || !inputs.depth || !inputs.dlssMotionVectors)
    {
        m_frameGenerationInactiveReason = "the frame prepared no depth and motion vectors";
        return false;
    }

    if(!imagesReady)
    {
        m_frameGenerationInactiveReason = "the generated-frame images are not ready";
        return false;
    }

    // RR.1A: an NGX instance awaiting its restart is not used; the restart runs before the next recording.
    if(m_dlssBackend.RecoveryRequired())
    {
        m_frameGenerationInactiveReason = "NGX recovery pending after a discarded evaluation";
        return false;
    }

    // Stage 2: the feature for these extents, released and created again when they change (as Ray Reconstruction's is).
    const unsigned int outputWidth = m_outputWidth;
    const unsigned int outputHeight = m_outputHeight;
    const unsigned int renderWidth = inputs.renderWidth;
    const unsigned int renderHeight = inputs.renderHeight;
    constexpr std::uint32_t format = DXGI_FORMAT_R8G8B8A8_UNORM;

    if(m_dlssBackend.FrameGenerationCreationNeeded(outputWidth, outputHeight, renderWidth, renderHeight, format))
    {
        if(!ReleaseDlssFeature(DlssFeatureKind::FrameGeneration, true, false))
        {
            m_frameGenerationInactiveReason = "the previous Frame Generation feature could not be released";
            return false;
        }

        std::string failure;
        const bool created = CreateDlssFeatureOnLifecycleList(DlssFeatureKind::FrameGeneration,
            [&](ID3D12GraphicsCommandList* list)
        {
            return m_dlssBackend.EnsureFrameGenerationFeature(list, outputWidth, outputHeight, renderWidth, renderHeight,
                format);
        }, failure);

        if(!created)
        {
            m_frameGenerationInactiveReason = failure.empty() ? m_dlssBackend.GetFrameGenerationFeatureStatus().detail : failure;
            return false;
        }

        m_frameGenerationResetPending = true;
    }
    else if(!m_dlssBackend.HasFeature(DlssFeatureKind::FrameGeneration))
    {
        // A creation failure for these extents, which is not retried.
        m_frameGenerationInactiveReason = m_dlssBackend.GetFrameGenerationFeatureStatus().detail;
        return false;
    }

    // Stage 3: the states NGX requires (FG guide 6): inputs readable, the generated frame writable.
    constexpr D3D12_RESOURCE_STATES input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_finalSceneColor.Transition(commandList, input);
    inputs.depth->Transition(commandList, input);
    inputs.dlssMotionVectors->Transition(commandList, input);
    m_frameGenerationOutput.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Stage 4: the evaluation. The camera is the unjittered one the frame rendered (the motion vectors exclude jitter too).
    const Matrix4 viewProjection = frameView.projection * frameView.view;
    DlssFrameGenerationEvaluation evaluation;
    evaluation.backbuffer = m_finalSceneColor.GetResource();
    evaluation.depth = inputs.depth->GetResource();
    evaluation.motionVectors = inputs.dlssMotionVectors->GetResource();
    evaluation.hudless = target.hudlessRecorded ? m_frameGenerationHudless.GetResource() : nullptr;
    evaluation.output = m_frameGenerationOutput.GetResource();
    evaluation.renderWidth = renderWidth;
    evaluation.renderHeight = renderHeight;
    StoreRowVectorMatrix(evaluation.cameraViewToClip, frameView.projection);
    StoreRowVectorMatrix(evaluation.clipToCameraView, frameView.projection.Inverse());
    StoreRowVectorMatrix(evaluation.clipToPrevClip, previousViewProjection * viewProjection.Inverse());
    StoreRowVectorMatrix(evaluation.prevClipToClip, viewProjection * previousViewProjection.Inverse());
    StoreVector(evaluation.cameraPosition, frameView.position);
    StoreVector(evaluation.cameraUp, frameView.up);
    StoreVector(evaluation.cameraRight, frameView.right);
    StoreVector(evaluation.cameraForward, frameView.forward);
    evaluation.cameraNear = frameView.nearPlane;
    evaluation.cameraFar = frameView.farPlane;
    evaluation.cameraFieldOfView = frameView.verticalFov;
    evaluation.cameraAspectRatio = frameView.aspectRatio;
    // A history discontinuity the upscalers also see (scene activation, camera cut, resize...), or Frame Generation's own.
    evaluation.reset = m_frameGenerationResetPending || inputs.reset;

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::FrameGenerationEvaluate);
    const bool evaluated = m_dlssBackend.EvaluateFrameGeneration(commandList, evaluation);
    RestoreDlssGraphicsState(commandList);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::FrameGenerationEvaluate);

    // Stage 5: the generated image for the UI pass; the final scene image back where the rest of the frame expects it.
    m_frameGenerationOutput.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if(!evaluated)
    {
        m_frameGenerationInactiveReason = m_dlssBackend.GetFrameGenerationFeatureStatus().detail;
        return false;
    }

    m_frameGenerationResetPending = false;
    return true;
}

void Renderer::RecordPackagedPresentationCopy(ID3D12GraphicsCommandList* commandList, const PresentationTarget& target)
{
    // The real frame: the finished scene image into the target, back in RENDER_TARGET for the rest of the frame.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(1, &barrier);
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(target.resource, m_finalSceneColor.GetResource());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &barrier);
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The generated frame, when this recording rendered one: its image into its composite, which idles in COPY_SOURCE for
    // the present thread. The Editor draws its UI over the generated image instead (RecordGeneratedFrameUi); a packaged
    // game has no UI to draw.
    if(!target.generatedRecorded || !target.generatedResource)
    {
        return;
    }

    barrier.Transition.pResource = target.generatedResource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(1, &barrier);
    m_frameGenerationOutput.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(target.generatedResource, m_frameGenerationOutput.GetResource());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &barrier);
    m_frameGenerationOutput.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::RecordGeneratedFrameUi(ID3D12GraphicsCommandList* commandList, const PresentationTarget& target,
    const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, const float clearColor[4])
{
    if(!target.generatedRecorded || !target.generatedResource)
    {
        return;
    }

    // The generated frame's composite: cleared like the real one, then the same UI with the generated viewport image.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.generatedResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &barrier);

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(target.generatedRtv, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &target.generatedRtv, FALSE, nullptr);
    ID3D12DescriptorHeap* const heap = m_resourceDescriptorHeap.GetHeap();
    commandList->SetDescriptorHeaps(1, &heap);

    const std::uint64_t sceneTexture = m_resourceDescriptorHeap.GetGPUHandle(m_finalSceneColor.GetSRVIndex()).ptr;
    const std::uint64_t generatedTexture = m_resourceDescriptorHeap.GetGPUHandle(m_frameGenerationOutput.GetSRVIndex()).ptr;
    m_debugUI.RenderUiOverGeneratedImage(commandList, sceneTexture, generatedTexture);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &barrier);
}