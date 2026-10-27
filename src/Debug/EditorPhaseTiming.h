#pragma once

#include "Debug/CpuFrameTiming.h"

#include <array>
#include <cstddef>
#include <optional>

// Diagnostics only (ENGINE_FRAME_HITCH_LOGGING=1): CPU wall-clock durations of Editor::Draw's phases for one frame,
// reported by the frame-hitch diagnostics (RendererDiagnostics::FrameHitchDiagnostics::Report). Disabled, a scope is one
// null test. A phase that does not run in a frame keeps no value, so its samples count only the frames it ran.
namespace EditorPhaseTiming
{
    // Top-level phases follow Editor::Draw's order and do not overlap; together with the remainder they make up Draw.
    // A child phase is nested in the phase named by Parent and is already included in it.
    enum class Phase : size_t
    {
        Draw,
        Selection,
        Hierarchy,
        AssetBrowser,
        AssetBrowserProject,
        AssetBrowserSetup,
        AssetBrowserContents,
        AssetBrowserContentsLookup,
        AssetBrowserContentsFilesystem,
        AssetBrowserThumbnails,
        Inspection,
        InspectionActivation,
        InspectionObjectView,
        ObjectViewMetadata,
        ObjectViewAssetLabels,
        ObjectViewSlotLabels,
        Inspector,
        InspectorTransform,
        InspectorComponents,
        ComponentPhysics,
        ComponentTriggerColliders,
        ComponentCharacterController,
        ComponentCamera,
        ComponentLight,
        ComponentAdd,
        MaterialRevisions,
        ModalsAndViewport,
        SceneInspectorWindow,
        Tail,
        TailViewportTools,
        TailGizmo,
        Count
    };

    inline constexpr size_t PhaseCount = static_cast<size_t>(Phase::Count);

    inline constexpr std::array<const char*, PhaseCount> Names = {"Editor::Draw", "selection/history",
        "hierarchy", "asset browser", "thumbnail project/registry tables", "browser setup/tree",
        "browser contents", "contents registry/path lookups", "contents filesystem checks",
        "thumbnail prune/schedule", "inspection target", "object inspection activation", "object view resolve",
        "object metadata lookup", "mesh/material labels", "material slot labels", "inspector", "transform controls",
        "component controls", "physics controls", "trigger collider controls", "character controller controls",
        "camera controls", "light controls", "add component controls", "material revisions", "modals/viewport",
        "scene inspector window", "tail (camera/undo/tools)", "viewport tools", "transform gizmo"};

    // The enclosing phase of each phase; Count marks Draw itself and Draw's direct (top-level) phases use Draw.
    inline constexpr std::array<Phase, PhaseCount> Parent = {Phase::Count, Phase::Draw, Phase::Draw, Phase::Draw,
        Phase::AssetBrowser, Phase::AssetBrowser, Phase::AssetBrowser, Phase::AssetBrowserContents,
        Phase::AssetBrowserContents, Phase::AssetBrowser, Phase::Draw, Phase::Inspection, Phase::Inspection, Phase::InspectionObjectView, Phase::InspectionObjectView,
        Phase::InspectionObjectView, Phase::Draw, Phase::Inspector, Phase::Inspector, Phase::InspectorComponents,
        Phase::InspectorComponents, Phase::InspectorComponents, Phase::InspectorComponents,
        Phase::InspectorComponents, Phase::InspectorComponents, Phase::Draw, Phase::Draw, Phase::Draw, Phase::Draw,
        Phase::Tail, Phase::TailViewportTools};

    struct Frame
    {
        std::array<std::optional<double>, PhaseCount> durations{};
    };

    // Adds its duration to the phase when it ends (End or destruction); a null frame records nothing.
    class Scope
    {
        public:
        Scope(Frame* frame, Phase phase) : m_frame(frame), m_phase(phase)
        {
            if(m_frame)
            {
                m_start = CpuTiming::Clock::now();
            }
        }

        ~Scope()
        {
            End();
        }

        void End()
        {
            if(!m_frame)
            {
                return;
            }

            std::optional<double>& duration = m_frame->durations[static_cast<size_t>(m_phase)];
            duration = duration.value_or(0.0) + CpuTiming::MillisecondsSince(m_start);
            m_frame = nullptr;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        private:
        Frame* m_frame = nullptr;
        Phase m_phase = Phase::Draw;
        CpuTiming::Clock::time_point m_start{};
    };
}
