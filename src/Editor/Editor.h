#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

class IDemoScene;
class SphereGridScene;
class StressTestScene;
class DemoSceneManager;

class Editor
{
    public:
        void Draw(IDemoScene& demoScene, DemoSceneManager& sceneManager, float viewportWidth, float viewportHeight);

        size_t GetSelectedObject() const
        {
            return m_selectedObject;
        }

        uint64_t GetSelectionStructureRevision() const
        {
            return m_selectionStructureRevision;
        }

    private:
        enum class GizmoMode
        {
            Translate,
            Rotate,
            Scale
        };

        GizmoMode m_gizmoMode = GizmoMode::Translate;

        bool m_cameraLooking = false;
        float m_cameraYaw = 0.0f;
        float m_cameraPitch = 0.0f;

        void DrawTransformGizmo(IDemoScene& demoScene, float viewportWidth, float viewportHeight);
        void HandleViewportSelection(IDemoScene& demoScene, float viewportWidth, float viewportHeight);
        void UpdateCameraLook(IDemoScene& demoScene, float viewportWidth, float viewportHeight);
        void DrawObjectControls(IDemoScene& demoScene, DemoSceneManager& sceneManager);
        void DrawStressTestControls(StressTestScene& demoScene);
        void DrawSphereGridControls(SphereGridScene& demoScene);
        void DrawDirectionalLight(IDemoScene& demoScene);
        void DrawCamera(IDemoScene& demoScene);

        static constexpr size_t InvalidObjectSelection = (std::numeric_limits<size_t>::max)();
        size_t m_selectedObject = InvalidObjectSelection;
        uint64_t m_selectionStructureRevision = 0;
        std::string m_selectionSceneName;
};