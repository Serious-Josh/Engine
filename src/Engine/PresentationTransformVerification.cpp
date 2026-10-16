#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Editor/ViewportPicking.h"
#include "Renderer/Material.h"
#include "Renderer/Renderer.h"
#include "Physics/ScenePhysicsSession.h"
#include "Engine/PhysicsSimulationController.h"
#include "Engine/Engine.h"
#include "Scene/SceneSerializer.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool SameMatrix(const Matrix4& left, const Matrix4& right)
{
    return std::memcmp(left.m, right.m, sizeof(left.m)) == 0;
}

void CheckPresentation(const RenderObject& object)
{
    const PresentationTransform presentation = object.GetPresentationTransform();
    Check(SameMatrix(presentation.currentModel, object.transform.GetModelMatrix()), "Current presentation differs from authored");
    Check(SameMatrix(presentation.previousModel, object.previousTransform.GetModelMatrix()), "Previous presentation differs from existing history");
}

class DiagnosticResolver final : public SceneAssetResolver
{
public:
    Mesh* ResolveMesh(std::string_view) override
    {
        return nullptr;
    }

    Material* ResolveMaterial(std::string_view) override
    {
        return &m_material;
    }

private:
    Material m_material;
};

void VerifyHistoryAndPicking()
{
    Scene scene;
    RenderObject object;
    object.transform.position = {0.0f, 0.0f, 5.0f};
    object.transform.rotation = {0.2f, 0.3f, 0.4f};
    object.transform.scale = {2.0f, -3.0f, 4.0f};
    scene.AddRenderObject(object);
    auto presentation = scene.GetRenderObjects().front().GetPresentationTransform();
    Check(SameMatrix(presentation.currentModel, presentation.previousModel), "First appearance has stale previous model");
    const Matrix4 firstModel = presentation.currentModel;
    Transform edited = object.transform;
    edited.position.x = 3.0f;
    scene.SetRenderObjectTransform(0, edited);
    edited.position.y = 4.0f;
    scene.SetRenderObjectTransform(0, edited);
    for (int read = 0; read < 3; ++read)
    {
        presentation = scene.GetRenderObjects().front().GetPresentationTransform();
        Check(SameMatrix(presentation.previousModel, firstModel), "Reads or repeated edits advanced previous history");
        Check(scene.GetChangedTransforms().size() == 1, "Presentation read changed transform dirtiness");
        CheckPresentation(scene.GetRenderObjects().front());
    }
    // No clear represents a deferred frame: previous remains the last completed value.
    scene.ClearChangedTransforms();
    presentation = scene.GetRenderObjects().front().GetPresentationTransform();
    Check(SameMatrix(presentation.currentModel, presentation.previousModel), "Successful-frame history did not advance");
    scene.ClearChangedTransforms();
    CheckPresentation(scene.GetRenderObjects().front());
    Check(scene.GetChangedTransforms().empty(), "Unchanged object generated false transform dirtiness");

    RenderObject replacement;
    replacement.transform.position = {0.0f, 0.0f, 10.0f};
    replacement.transform.scale = {-2.0f, 3.0f, 1.0f};
    scene.ReplaceRenderObjects({replacement});
    presentation = scene.GetRenderObjects().front().GetPresentationTransform();
    Check(SameMatrix(presentation.currentModel, presentation.previousModel), "Index reuse inherited old previous matrix");
    Check(!SameMatrix(presentation.currentModel, firstModel), "Index reuse retained a cached current matrix");

    ImportedMesh triangle;
    triangle.vertices = {{{-1, -1, 0}}, {{1, -1, 0}}, {{0, 1, 0}}};
    triangle.indices = {0, 1, 2};
    triangle.metadata = DefaultMeshMetadata(3);
    const MeshBounds localBounds{{-1, -1, 0}, {1, 1, 0}, true};
    const ViewportPicking::Ray ray{{0, 0, 0}, {0, 0, 1}, 100};
    double distance = 100;
    Check(ViewportPicking::IntersectMesh(ray, presentation.currentModel, localBounds, triangle, true,
        triangle.metadata.submeshes, distance) && std::abs(distance - 10.0) < 0.00001,
        "Picking does not intersect the presented mirrored/nonuniform geometry");
}
}

class PresentationTransformVerification
{
public:
    static void VerifyEditorPhysicsIntegration()
    {
        using State = PhysicsSimulationController::State;
        Editor displayPreferences;
        Check(displayPreferences.ArePhysicsCollidersVisible(), "Collider display did not default to enabled");
        displayPreferences.m_showPhysicsColliders = false;
        displayPreferences.ResetSceneInteractions();
        displayPreferences.OnSceneActivated();
        displayPreferences.SetPhysicsStatus(Editor::PhysicsState::Playing, false, {});
        displayPreferences.SetPhysicsStatus(Editor::PhysicsState::Stopping, false, {});
        displayPreferences.SetPhysicsStatus(Editor::PhysicsState::Stopped, true, {});
        Check(!displayPreferences.ArePhysicsCollidersVisible(), "Scene/interaction/physics reset discarded display preference");
        DiagnosticResolver resolver;
        SceneDocument document;
        document.id = "editor-physics-integration";
        SceneObjectDocument box;
        box.id = "falling-box";
        box.transform.position = {0.0f, 5.0f, 0.0f};
        box.physics.emplace();
        box.physics->bodyType = Physics::BodyType::Dynamic;
        document.objects.push_back(box);
        auto plain = box;
        plain.id = "authored-only";
        plain.physics.reset();
        document.objects.push_back(plain);
        auto runtime = MakeRuntime(document, resolver);
        auto engine = std::make_unique<Engine>();
        engine->m_renderer.m_sceneResourceScene = &runtime->GetScene();
        engine->m_renderer.m_sceneResourceLifetime = 300;
        engine->m_state = EngineState::Running;
        auto& editor = engine->m_editor;
        editor.m_selectedObject = 0;
        editor.m_selectedObjects = {0};
        editor.m_historyEpoch = 17;
        auto historyBefore = box.transform;
        historyBefore.position.x = -1.0f;
        auto historyRedo = box.transform;
        historyRedo.position.x = 1.0f;
        editor.m_sceneHistory.push_back({Editor::TransformEdit{Editor::PersistentHistoryTarget{box.id}, historyBefore, box.transform}, 17, 100});
        editor.m_sceneHistory.push_back({Editor::TransformEdit{Editor::PersistentHistoryTarget{box.id}, box.transform, historyRedo}, 17, 100});
        editor.m_historyCursor = 1;
        editor.m_historyPayloadBytes = 200;
        const auto checkHistoryAndSelection = [&]()
        {
            Check(editor.m_selectedObject == 0 && editor.m_selectedObjects == std::vector<std::size_t>{0} &&
                editor.m_sceneHistory.size() == 2 && editor.m_historyCursor == 1 && editor.m_historyEpoch == 17 &&
                editor.m_historyPayloadBytes == 200 &&
                SameMatrix(std::get<Editor::TransformEdit>(editor.m_sceneHistory[0].command).before.GetModelMatrix(), historyBefore.GetModelMatrix()) &&
                SameMatrix(std::get<Editor::TransformEdit>(editor.m_sceneHistory[1].command).after.GetModelMatrix(), historyRedo.GetModelMatrix()),
                "Physics controls altered selection or undo history");
        };
        engine->RefreshEditorPhysicsStatus(true);
        Check(editor.m_canPlayPhysics && !editor.IsPhysicsEditingLocked(), "Compatible stopped scene cannot Play");
        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 2.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Playing &&
            engine->m_physicsSimulation.GetTotalSimulatedSteps() == 0 && runtime->GetScene().HasPhysicsPresentation(),
            "Play did not establish initial presentation before stepping");
        EditorColliderOverlay initialPresentedColliders;
        std::string colliderCaptureError;
        Check(runtime->CaptureEditorColliderOverlay(engine->m_renderer, initialPresentedColliders, colliderCaptureError),
            "Initial presented collider capture failed");
        Check(initialPresentedColliders.colliderWorldTransforms.size() == 1,
            "Collider capture included nonphysics objects or missed active body");
        checkHistoryAndSelection();
        Check(editor.HistoryReplayBlocked() && editor.SceneObjectDeletionBlocked(), "Playing did not lock undo/delete");
        std::string error;
        Check(!engine->RequestSceneObjectCreation("", Transform{}, error) && error.find("Stop physics") != std::string::npos,
            "Engine mutation entry point bypassed Play restrictions");
        Check(!engine->RequestMaterialAssignment(SceneObjectMetadata{}, "", error), "Material authoring bypassed Play restrictions");
        Check(!engine->BeginPrimitiveCreation({ScenePrimitive::Cube,0,0},error), "Primitive creation bypassed Playing restrictions");
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0 / 120.0, true);
        Check(engine->m_physicsSimulation.GetTotalSimulatedSteps() == 0, "Half-frame elapsed time stepped too early");
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0 / 120.0, true);
        Check(engine->m_physicsSimulation.GetTotalSimulatedSteps() == 1, "Engine did not deliver supplied elapsed time exactly once");
        EditorColliderOverlay beforeDeferredColliders;
        Check(runtime->CaptureEditorColliderOverlay(engine->m_renderer, beforeDeferredColliders, colliderCaptureError),
            "Active collider capture failed");
        Check(beforeDeferredColliders.colliderWorldTransforms.size() == 1, "Active collider count changed");
        const auto enabledFrameOverlay = engine->CaptureEditorColliderOverlayForFrame(runtime.get(), true);
        Check(enabledFrameOverlay && enabledFrameOverlay->colliderWorldTransforms.size() == 1, "Enabled frame capture failed");
        editor.m_showPhysicsColliders = false;
        Check(!engine->CaptureEditorColliderOverlayForFrame(runtime.get(), true), "Hidden overlay submitted frame data");
        Check(engine->m_physicsSimulation.GetState() == State::Active, "Display preference stopped simulation");
        checkHistoryAndSelection();
        editor.m_showPhysicsColliders = true;
        const auto resumedFrameOverlay = engine->CaptureEditorColliderOverlayForFrame(runtime.get(), true);
        Check(resumedFrameOverlay && SameMatrix(resumedFrameOverlay->colliderWorldTransforms[0], beforeDeferredColliders.colliderWorldTransforms[0]),
            "Re-enabled visualization reused stale frame data");
        Check(!engine->CaptureEditorColliderOverlayForFrame(runtime.get(), false), "Noneditable runtime received an editor overlay");
        const auto beforeDeferred = runtime->GetScene().GetRenderObjects()[0].GetPresentationTransform();
        // A closed but unresolved command generation blocks publication, while
        // no active recording exists. CPU stepping remains outside recording.
        editor.m_showPhysicsColliders = false;
        engine->m_renderer.m_commandRecordingGeneration = 1;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0 / 60.0, true);
        Check(engine->m_physicsSimulation.GetTotalSimulatedSteps() == 2 &&
            engine->m_physicsSimulation.HasPendingPresentation(), "Deferred publication lost simulation or latest poses");
        Check(SameMatrix(beforeDeferred.currentModel, runtime->GetScene().GetRenderObjects()[0].GetPresentationTransform().currentModel),
            "Deferred publication changed displayed transforms");
        EditorColliderOverlay deferredColliders;
        Check(runtime->CaptureEditorColliderOverlay(engine->m_renderer, deferredColliders, colliderCaptureError),
            "Deferred collider capture failed");
        Check(deferredColliders.colliderWorldTransforms.size() == 1, "Deferred collider count changed");
        Check(SameMatrix(beforeDeferredColliders.colliderWorldTransforms[0], deferredColliders.colliderWorldTransforms[0]),
            "Deferred collider capture used unaccepted simulation poses");
        engine->m_renderer.m_commandRecordingGeneration = 0;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        Check(engine->m_physicsSimulation.GetTotalSimulatedSteps() == 2 && !engine->m_physicsSimulation.HasPendingPresentation(),
            "Publication retry simulated extra elapsed time");
        const auto published = runtime->GetScene().GetRenderObjects()[0].GetPresentationTransform();
        Check(!engine->CaptureEditorColliderOverlayForFrame(runtime.get(), true), "Hidden moving body produced a frame overlay");
        editor.m_showPhysicsColliders = true;
        const auto currentFrameOverlay = engine->CaptureEditorColliderOverlayForFrame(runtime.get(), true);
        Check(currentFrameOverlay && currentFrameOverlay->colliderWorldTransforms.size() == 1,
            "Re-enabled moving body capture failed");
        Check(!SameMatrix(currentFrameOverlay->colliderWorldTransforms[0], enabledFrameOverlay->colliderWorldTransforms[0]),
            "Re-enable reused the old simulated collider pose");
        Check(SameMatrix(beforeDeferred.previousModel, published.previousModel), "Deferred frame advanced history");
        runtime->GetScene().ClearChangedTransforms();
        Check(SameMatrix(runtime->GetScene().GetRenderObjects()[0].GetPresentationTransform().previousModel, published.currentModel),
            "Successful-frame history did not advance");

        engine->m_renderer.m_activeRecordingGeneration = 1;
        editor.m_physicsRequest = Editor::PhysicsRequest::Stop;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 10.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopping && !editor.m_canPlayPhysics &&
            !engine->m_physicsSimulation.GetLatestSnapshot(), "Pending Stop retained bodies or showed Playing");
        Check(!engine->BeginPrimitiveCreation({ScenePrimitive::Sphere,0,0},error), "Primitive creation bypassed Stopping restrictions");
        engine->m_renderer.m_activeRecordingGeneration = 0;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 10.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopped && !editor.IsPhysicsEditingLocked() &&
            !runtime->GetScene().HasPhysicsPresentation() && engine->m_physicsSimulation.GetTotalSimulatedSteps() == 2,
            "Stop retry advanced simulation or failed to restore editing");
        EditorColliderOverlay afterStopColliders;
        Check(runtime->CaptureEditorColliderOverlay(engine->m_renderer, afterStopColliders, colliderCaptureError),
            "Stopped collider capture failed");
        Check(afterStopColliders.colliderWorldTransforms.size() == 1, "Stopped collider count changed");
        Check(SameMatrix(initialPresentedColliders.colliderWorldTransforms[0], afterStopColliders.colliderWorldTransforms[0]),
            "Stop did not restore authored collider placement");
        const auto cleanColliderDocument = runtime->m_fileScene;
        auto colliderSelection = runtime->GetObjectMetadata(std::string_view{"falling-box"});
        Check(colliderSelection.has_value(), "Collider removal target is unavailable");
        Check(runtime->SetObjectPhysics(*colliderSelection, box.physics, {}, &colliderCaptureError),
            "Collider component removal failed");
        Check(runtime->CaptureEditorColliderOverlay(engine->m_renderer, afterStopColliders, colliderCaptureError),
            "Collider capture after component removal failed");
        Check(afterStopColliders.colliderWorldTransforms.empty(), "Removed component retained collider visualization");
        Check(runtime->SetObjectPhysics(*colliderSelection, {}, box.physics, &colliderCaptureError), "Collider component restoration failed");
        // Isolate the authored mutation subcase from the existing Play/Stop dirty-state checks.
        runtime->m_fileScene = cleanColliderDocument;
        runtime->GetScene().ClearChangedTransforms();
        CheckPresentation(runtime->GetScene().GetRenderObjects()[0]);
        checkHistoryAndSelection();

        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        engine->m_renderer.m_activeRecordingGeneration = 1;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0, true);
        Check(engine->m_physicsSimulation.GetTotalSimulatedSteps() == 0 &&
            editor.m_physicsState == Editor::PhysicsState::Stopping && !engine->m_physicsSimulation.GetLatestSnapshot(),
            "Engine advanced physics during command recording");
        engine->m_renderer.m_activeRecordingGeneration = 0;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopped, "Recording guard cleanup did not finish");

        engine->m_renderer.m_activeRecordingGeneration = 1;
        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopped && !editor.m_physicsMessage.empty() &&
            !runtime->GetScene().HasPhysicsPresentation(), "Failed Start displayed Playing or altered presentation");
        engine->m_renderer.m_activeRecordingGeneration = 0;
        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        engine->m_pendingEditorSceneOperation = Editor::FileRequest{Editor::FileRequestType::New, {}};
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0, true);
        Check(engine->m_physicsSimulation.GetState() == State::Inactive && !runtime->GetScene().HasPhysicsPresentation(),
            "Scene replacement request did not stop the old simulation before advancing");
        engine->m_pendingEditorSceneOperation.reset();

        for (int cycle = 0; cycle < 8; ++cycle)
        {
            editor.m_physicsRequest = Editor::PhysicsRequest::Play;
            engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
            Check(engine->m_physicsSimulation.GetState() == State::Active, "Repeated editor Play failed");
            // Same checked cleanup operation used by scene release and shutdown.
            Check(engine->StopEditorPhysicsSimulation(*runtime), "Engine teardown cleanup failed");
            engine->RefreshEditorPhysicsStatus(true);
            Check(!runtime->GetScene().HasPhysicsPresentation(), "Engine teardown left active presentation");
        }
        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, true);
        ++runtime->m_objectMapping.bindingRevisions[0];
        EditorColliderOverlay staleColliders;
        Check(!runtime->CaptureEditorColliderOverlay(engine->m_renderer, staleColliders, colliderCaptureError),
            "Stale physics association was accepted by collider capture");
        Check(staleColliders.colliderWorldTransforms.empty(), "Stale physics association produced collider lines");
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 1.0, true);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopped && !editor.m_physicsMessage.empty() &&
            !engine->m_physicsSimulation.GetLatestSnapshot(), "Stale association remained Playing");
        SceneDocument captured;
        Check(CaptureSceneDocument(document, runtime->GetScene(), runtime->GetObjectMapping(), captured, &error) &&
            SceneDocumentsEqual(document, captured), "Editor physics changed authored scene data");
        Check(!runtime->GetFileScene().IsDirty(), "Play/Stop dirtied the authored document");
        Check(runtime->SaveAs("build/editor-physics-verification.scene", &error), "Saving after Play/Stop failed");
        SceneDocument saved;
        Check(LoadSceneDocument("build/editor-physics-verification.scene", saved, &error) && SceneDocumentsEqual(document, saved),
            "Editor Play/Stop serialized simulated poses");
        checkHistoryAndSelection();
        editor.m_physicsRequest = Editor::PhysicsRequest::Play;
        engine->ProcessEditorPhysicsSimulation(runtime.get(), 0.0, false);
        Check(editor.m_physicsState == Editor::PhysicsState::Stopped, "Incompatible scene accepted Play");
        engine->m_renderer.m_sceneResourceScene = nullptr;
        engine->m_renderer.m_sceneResourceLifetime = 0;
    }

    static void VerifySimulationController()
    {
        using Controller = PhysicsSimulationController;
        using Status = Controller::Status;
        DiagnosticResolver resolver;
        SceneDocument document;
        document.id = "simulation-controller-verification";
        for (int index = 0; index < 4; ++index)
        {
            SceneObjectDocument object;
            object.id = "simulation-object-" + std::to_string(index);
            object.transform.position = {static_cast<float>(index * 4), 5.0f, 0.0f};
            if (index < 3)
            {
                object.physics.emplace();
                object.physics->bodyType = index == 0 ? Physics::BodyType::Static :
                    index == 1 ? Physics::BodyType::Dynamic : Physics::BodyType::Kinematic;
            }
            if (index == 0)
            {
                object.transform.position = {0.0f, -0.5f, 0.0f};
                object.physics->boxDimensions = {100.0f, 1.0f, 100.0f};
            }
            document.objects.push_back(object);
        }
        auto runtime = MakeRuntime(document, resolver);
        auto secondRuntime = MakeRuntime(document, resolver);
        auto renderer = std::make_unique<Renderer>();
        auto secondRenderer = std::make_unique<Renderer>();
        // CPU-only identity fixtures: no GPU resources or renderer Initialize.
        renderer->m_sceneResourceScene = &runtime->GetScene();
        renderer->m_sceneResourceLifetime = 200;
        secondRenderer->m_sceneResourceScene = &secondRuntime->GetScene();
        secondRenderer->m_sceneResourceLifetime = 201;
        Controller controller;
        Controller secondController;
        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Controller start failed");
        Check(secondController.Start(*secondRuntime, *secondRenderer).status == Status::Started, "Second controller start failed");
        Check(controller.GetTotalSimulatedSteps() == 0 && controller.GetLatestSnapshot() &&
            runtime->GetScene().HasPhysicsPresentation(), "Initial presentation was not established before stepping");
        CheckPresentation(runtime->GetScene().GetRenderObjects()[3]);
        Check(controller.Update(*runtime, *renderer, 0.0).stepsExecuted == 0, "Zero time stepped physics");
        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), Controller::MaximumAccumulatedSeconds + 1.0})
        {
            Check(controller.Update(*runtime, *renderer, invalid).status == Status::InvalidInput &&
                controller.GetTotalSimulatedSteps() == 0 && controller.GetAccumulatedSeconds() == 0.0,
                "Invalid time changed controller state");
        }
        for (int update = 0; update < 120; ++update)
        {
            Check(controller.Update(*runtime, *renderer, 1.0 / 120.0).status == Status::Updated, "120 Hz update failed");
            Check(controller.PublishAtBoundary(*runtime, *renderer).status == Status::Published, "120 Hz publish failed");
            runtime->GetScene().ClearChangedTransforms();
        }
        for (int update = 0; update < 30; ++update)
        {
            Check(secondController.Update(*secondRuntime, *secondRenderer, 1.0 / 30.0).status == Status::Updated, "30 Hz update failed");
            Check(secondController.PublishAtBoundary(*secondRuntime, *secondRenderer).status == Status::Published, "30 Hz publish failed");
            secondRuntime->GetScene().ClearChangedTransforms();
        }
        Check(controller.GetTotalSimulatedSteps() == 60 && secondController.GetTotalSimulatedSteps() == 60 &&
            controller.GetAccumulatedSeconds() == 0.0 && secondController.GetAccumulatedSeconds() == 0.0,
            "Equivalent time chunking missed or duplicated fixed steps");
        for (int index = 0; index < 3; ++index)
        {
            Check(SameMatrix(runtime->GetScene().GetRenderObjects()[index].GetPresentationTransform().currentModel,
                secondRuntime->GetScene().GetRenderObjects()[index].GetPresentationTransform().currentModel),
                "Equivalent elapsed time produced different body poses");
        }
        Check(controller.GetLatestSnapshot()->FindPose("simulation-object-1")->position.y < 5.0f, "Dynamic body did not fall");
        Check(controller.GetLatestSnapshot()->FindPose("simulation-object-2")->position.y == 5.0f, "Kinematic body drifted");
        std::string error;
        SceneDocument captured;
        Check(CaptureSceneDocument(document, runtime->GetScene(), runtime->GetObjectMapping(), captured, &error) &&
            SceneDocumentsEqual(document, captured), "Controller changed authored scene/history source data");
        const auto savePath = std::filesystem::path("build/physics-simulation-verification.scene");
        Check(runtime->SaveAs(savePath, &error), "Saving while simulation is active failed");
        SceneDocument reloadedDocument;
        Check(LoadSceneDocument(savePath, reloadedDocument, &error) && SceneDocumentsEqual(document, reloadedDocument),
            "Scene save persisted simulated transforms");

        const auto beforeDeferred = runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform();
        renderer->m_activeRecordingGeneration = 1;
        Check(controller.Update(*runtime, *renderer, Controller::FixedStepSeconds).stepsExecuted == 1,
            "CPU simulation could not advance independently of publication");
        Check(controller.PublishAtBoundary(*runtime, *renderer).status == Status::Pending && controller.HasPendingPresentation(),
            "Recording did not defer presentation publication");
        Check(SameMatrix(beforeDeferred.currentModel, runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform().currentModel),
            "CPU update mutated presentation during recording");
        renderer->m_activeRecordingGeneration = 0;
        Check(controller.PublishAtBoundary(*runtime, *renderer).status == Status::Published, "Publication retry failed");
        const auto afterDeferred = runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform();
        Check(SameMatrix(beforeDeferred.previousModel, afterDeferred.previousModel), "Deferred publication advanced successful-frame history");
        runtime->GetScene().ClearChangedTransforms();
        Check(SameMatrix(runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform().previousModel, afterDeferred.currentModel),
            "Successful frame did not advance presentation history");
        renderer->m_activeRecordingGeneration = 1;
        Check(controller.Stop(*runtime, *renderer).status == Status::Pending && !controller.GetLatestSnapshot() &&
            controller.GetState() == Controller::State::Stopping, "Pending stop retained simulation or mutated presentation");
        Check(controller.Update(*runtime, *renderer, 1.0).status == Status::Failed, "Stopping controller continued advancing");
        renderer->m_activeRecordingGeneration = 0;
        Check(controller.Stop(*runtime, *renderer).status == Status::Stopped && !runtime->GetScene().HasPhysicsPresentation(),
            "Stop retry did not clean up presentation");
        Check(secondController.Stop(*secondRuntime, *secondRenderer).status == Status::Stopped, "Second controller cleanup failed");

        Controller capped(4);
        Check(capped.Start(*runtime, *renderer).status == Status::Started, "Capped controller start failed");
        Check(capped.Update(*runtime, *renderer, 10.0 / 60.0).stepsExecuted == 4 &&
            std::abs(capped.GetAccumulatedSeconds() - 6.0 / 60.0) < 1.0e-12, "Catch-up cap discarded accumulated debt");
        Check(capped.Update(*runtime, *renderer, 0.0).stepsExecuted == 0 && capped.GetTotalSimulatedSteps() == 4,
            "Zero time drained catch-up debt");
        Check(capped.Update(*runtime, *renderer, Controller::MaximumAccumulatedSeconds).status == Status::InvalidInput &&
            capped.GetTotalSimulatedSteps() == 4, "Overflowing backlog was accepted");
        Check(capped.Update(*runtime, *renderer, 1.0 / 60.0).stepsExecuted == 4 &&
            capped.Update(*runtime, *renderer, 1.0 / 60.0).stepsExecuted == 4 &&
            capped.GetTotalSimulatedSteps() == 12 && capped.GetAccumulatedSeconds() == 0.0,
            "Retained catch-up debt was not simulated on later positive updates");
        Check(capped.Stop(*runtime, *renderer).status == Status::Stopped, "Capped controller stop failed");

        for (int cycle = 0; cycle < 12; ++cycle)
        {
            Check(controller.Start(*runtime, *renderer).status == Status::Started, "Repeated start failed");
            Check(controller.Update(*runtime, *renderer, 1.0 / 60.0).stepsExecuted == 1, "Exact boundary did not step once");
            Check(controller.PublishAtBoundary(*runtime, *renderer).status == Status::Published, "Repeated publish failed");
            Check(controller.Stop(*runtime, *renderer).status == Status::Stopped, "Repeated stop failed");
        }
        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Stale binding fixture start failed");
        ++runtime->m_objectMapping.bindingRevisions[0];
        Check(controller.Update(*runtime, *renderer, 1.0).status == Status::Failed &&
            controller.GetTotalSimulatedSteps() == 0 && !controller.GetLatestSnapshot() && !runtime->GetScene().HasPhysicsPresentation(),
            "Stale binding advanced physics or failed owned cleanup");

        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Superseded fixture start failed");
        auto association = runtime->AssociatePhysicsPresentation(*renderer, document, error);
        auto externalSession = Physics::ScenePhysicsSession::Create(document, &error);
        externalSession->Step();
        auto externalSnapshot = externalSession->CapturePoses(&error);
        Check(association && externalSnapshot && runtime->ApplyPhysicsPresentation(*renderer, *association, *externalSnapshot, error),
            "External presentation replacement failed");
        const auto newer = runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform();
        Check(controller.Stop(*runtime, *renderer).status == Status::Superseded && runtime->GetScene().HasPhysicsPresentation() &&
            SameMatrix(newer.currentModel, runtime->GetScene().GetRenderObjects()[1].GetPresentationTransform().currentModel),
            "Old controller cleared an unrelated newer presentation");
        Check(controller.Start(*runtime, *renderer).status == Status::Failed && !controller.GetLatestSnapshot(),
            "Failed start overwrote existing presentation or retained a session");
        Check(runtime->ClearPhysicsPresentation(*renderer, error), "External presentation cleanup failed");

        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Replacement fixture start failed");
        renderer->m_sceneResourceScene = &secondRuntime->GetScene();
        Check(controller.Update(*secondRuntime, *renderer, 1.0).status == Status::Failed &&
            !secondRuntime->GetScene().HasPhysicsPresentation() && !controller.GetLatestSnapshot(),
            "Stale session affected a replacement runtime with identical IDs");
        renderer->m_sceneResourceScene = &runtime->GetScene();
        Check(runtime->ClearPhysicsPresentation(*renderer, error), "Old runtime cleanup failed");
        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Authored change fixture start failed");
        auto edited = runtime->GetScene().GetRenderObjects()[1].transform;
        edited.position.x += 1.0f;
        runtime->GetScene().SetRenderObjectTransform(1, edited);
        Check(controller.Update(*runtime, *renderer, 1.0).status == Status::Failed && !controller.GetLatestSnapshot(),
            "Incompatible authored transform did not terminate the session");
        runtime->GetScene().SetRenderObjectTransform(1, document.objects[1].transform);
        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Visual scale fixture start failed");
        auto scaled = document.objects[1].transform;
        scaled.scale = {-2.0f, 3.0f, 0.5f};
        runtime->GetScene().SetRenderObjectTransform(1, scaled);
        Check(controller.Update(*runtime, *renderer, Controller::FixedStepSeconds).status == Status::Updated &&
            controller.PublishAtBoundary(*runtime, *renderer).status == Status::Published,
            "Visual-only scale edit incorrectly invalidated physics");
        Check(controller.Stop(*runtime, *renderer).status == Status::Stopped, "Visual scale cleanup failed");
        runtime->GetScene().SetRenderObjectTransform(1, document.objects[1].transform);

        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Renderer lifetime fixture start failed");
        ++renderer->m_sceneResourceLifetime;
        Check(controller.Update(*runtime, *renderer, 1.0).status == Status::Failed &&
            controller.GetTotalSimulatedSteps() == 0 && !controller.GetLatestSnapshot(),
            "Renderer lifetime change did not terminate the old session");
        Check(runtime->ClearPhysicsPresentation(*renderer, error), "Renderer lifetime fixture cleanup failed");

        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Publication failure fixture start failed");
        Check(controller.Update(*runtime, *renderer, Controller::FixedStepSeconds).stepsExecuted == 1, "Pre-failure step failed");
        ++runtime->m_objectMapping.bindingRevisions[0];
        Check(controller.PublishAtBoundary(*runtime, *renderer).status == Status::Failed &&
            !controller.GetLatestSnapshot() && !runtime->GetScene().HasPhysicsPresentation(),
            "Rejected publication failed to release its session and owned presentation");

        auto unsupportedDocument = document;
        unsupportedDocument.objects[1].physics->boxDimensions.width = 0.00001f;
        auto unsupportedRuntime = MakeRuntime(unsupportedDocument, resolver);
        renderer->m_sceneResourceScene = &unsupportedRuntime->GetScene();
        Check(controller.Start(*unsupportedRuntime, *renderer).status == Status::Failed &&
            !unsupportedRuntime->GetScene().HasPhysicsPresentation() && !controller.GetLatestSnapshot(),
            "Failed body registration left simulation or presentation active");
        renderer->m_sceneResourceScene = &runtime->GetScene();
        renderer->m_activeRecordingGeneration = 1;
        Check(controller.Start(*runtime, *renderer).status == Status::Failed && !runtime->GetScene().HasPhysicsPresentation(),
            "Start published initial poses during recording");
        renderer->m_activeRecordingGeneration = 0;
        Check(controller.Start(*runtime, *renderer).status == Status::Started, "Structure fixture start failed");
        runtime->GetScene().AddRenderObject(RenderObject{});
        Check(controller.Update(*runtime, *renderer, 1.0).status == Status::Failed && !controller.GetLatestSnapshot(),
            "Changed membership did not terminate the session");
        renderer->m_sceneResourceScene = nullptr;
        renderer->m_sceneResourceLifetime = 0;
        secondRenderer->m_sceneResourceScene = nullptr;
        secondRenderer->m_sceneResourceLifetime = 0;
    }

    static void VerifyOverrides()
    {
        DiagnosticResolver resolver;
        SceneDocument document;
        document.id = "physics-presentation-verification";
        for (int index = 0; index < 4; ++index)
        {
            SceneObjectDocument object;
            object.id = "object-" + std::to_string(index);
            object.transform.position = {static_cast<float>(index * 10), 10.0f, 0.0f};
            object.transform.rotation.z = 0.4f;
            object.transform.scale = {-2.0f, 3.0f, 4.0f};
            if (index < 3)
            {
                object.physics.emplace();
                object.physics->bodyType = index == 0 ? Physics::BodyType::Dynamic :
                    index == 1 ? Physics::BodyType::Static : Physics::BodyType::Kinematic;
                object.physics->colliderLocalPositionOffset = {0.25f, 0.5f, 0.0f};
            }
            document.objects.push_back(object);
        }
        auto runtime = MakeRuntime(document, resolver);
        auto session = Physics::ScenePhysicsSession::Create(document);
        Check(session != nullptr, "Cannot register diagnostic physics session");
        auto renderer = std::make_unique<Renderer>();
        // CPU boundary fixture only: no device, command context, renderer Initialize,
        // TLAS or BLAS. Production acceptance uses these same identity/idle checks.
        renderer->m_sceneResourceScene = &runtime->GetScene();
        renderer->m_sceneResourceLifetime = 100;
        std::string error;
        auto association = runtime->AssociatePhysicsPresentation(*renderer, document, error);
        Check(association.has_value(), "Cannot associate current authored physics document");
        auto initial = session->CapturePoses();
        Check(initial && runtime->ApplyPhysicsPresentation(*renderer, *association, *initial, error),
            "Initial physics presentation failed");
        auto& scene = runtime->GetScene();
        Check(scene.HasPhysicsPresentation(), "Explicit presentation/gizmo state is not active");
        const auto firstPresentation = scene.GetRenderObjects()[0].GetPresentationTransform();
        Check(SameMatrix(firstPresentation.previousModel, document.objects[0].transform.GetModelMatrix()),
            "Authored-to-physics transition lost the previous successful model");
        SceneDocument captured;
        Check(CaptureSceneDocument(document, scene, runtime->GetObjectMapping(), captured, &error) &&
            SceneDocumentsEqual(document, captured), "Physics presentation changed authored capture/history data");
        for (int tick = 0; tick < 12; ++tick) session->Step();
        auto moved = session->CapturePoses();
        Check(moved && moved->FindPose("object-0")->position.y < 10.0f, "Dynamic fixture did not fall");
        Check(runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Replacement failed");
        const auto movedPresentation = scene.GetRenderObjects()[0].GetPresentationTransform();
        Check(SameMatrix(movedPresentation.previousModel, firstPresentation.previousModel),
            "Deferred frame or replacement advanced presentation history");
        Check(scene.GetChangedTransforms().empty() && !scene.GetChangedPresentationTransforms().empty(),
            "Physics-only movement did not produce independent TLAS dirtiness");
        for (int index = 0; index < 3; ++index)
        {
            const auto expected = Physics::ComposeRuntimeModelMatrix(*moved->FindPose(document.objects[index].id),
                document.objects[index].transform.scale);
            Check(SameMatrix(expected, scene.GetRenderObjects()[index].GetPresentationTransform().currentModel),
                "Static/dynamic/kinematic pose or mirrored/nonuniform scale differs from shared composition");
            Check(SameMatrix(scene.GetRenderObjects()[index].transform.GetModelMatrix(), document.objects[index].transform.GetModelMatrix()),
                "Simulation changed an authored transform");
        }
        CheckPresentation(scene.GetRenderObjects()[3]);
        // Candidate freshness uses exactly this effective current-model comparison.
        Check(!SameMatrix(firstPresentation.currentModel, movedPresentation.currentModel),
            "A candidate built before replacement would appear transform-current");
        scene.ClearChangedTransforms();
        Check(SameMatrix(scene.GetRenderObjects()[0].GetPresentationTransform().previousModel, movedPresentation.currentModel),
            "Successful render did not advance physics history");
        Check(scene.GetChangedPresentationTransforms().empty(), "Successful frame retained physics dirtiness");
        Check(runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error) &&
            scene.GetChangedPresentationTransforms().empty(), "Unchanged snapshot generated false movement");

        const auto unchanged = scene.GetRenderObjects()[0].GetPresentationTransform();
        auto malformed = *moved;
        malformed.m_entries.pop_back();
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, malformed, error), "Missing pose accepted");
        malformed = *moved;
        malformed.m_entries[1].objectID = malformed.m_entries[0].objectID;
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, malformed, error), "Duplicate ID accepted");
        malformed = *moved;
        malformed.m_entries[1].objectID = "unknown";
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, malformed, error), "Unknown ID accepted");
        malformed = *moved;
        malformed.m_entries.back().pose.orientation.w = 0.0f;
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, malformed, error), "Invalid quaternion accepted");
        malformed = *moved;
        malformed.m_entries.back().pose.position.x = std::numeric_limits<float>::quiet_NaN();
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, malformed, error), "Nonfinite position accepted");
        Check(SameMatrix(unchanged.currentModel, scene.GetRenderObjects()[0].GetPresentationTransform().currentModel),
            "Rejected snapshot partially changed active presentation");
        auto duplicateAssociation = *association;
        duplicateAssociation.objects[1] = duplicateAssociation.objects[0];
        Check(!runtime->ApplyPhysicsPresentation(*renderer, duplicateAssociation, *moved, error), "Duplicate association accepted");
        renderer->m_activeRecordingGeneration = 1;
        Check(!runtime->ClearPhysicsPresentation(*renderer, error), "Clear accepted during recording");
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Apply accepted during recording");
        renderer->m_activeRecordingGeneration = 0;
        ++renderer->m_sceneResourceLifetime;
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Stale renderer lifetime accepted");
        --renderer->m_sceneResourceLifetime;
        ++renderer->m_materialPublication.revision;
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Stale publication epoch accepted");
        --renderer->m_materialPublication.revision;
        ++runtime->m_objectMapping.bindingRevisions[0];
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Stale binding revision accepted");
        runtime->RevalidatePhysicsPresentationBindings(*renderer);
        Check(scene.HasPhysicsPresentation() && SameMatrix(unchanged.currentModel,
            scene.GetRenderObjects()[0].GetPresentationTransform().currentModel), "Binding-only revalidation lost presentation");

        Transform edited = scene.GetRenderObjects()[0].transform;
        edited.position.x = 99.0f;
        edited.scale = {4.0f, -2.0f, 0.5f};
        scene.SetRenderObjectTransform(0, edited);
        Check(SameMatrix(scene.GetRenderObjects()[0].GetPresentationTransform().currentModel,
            Physics::ComposeRuntimeModelMatrix(*moved->FindPose("object-0"), edited.scale)), "Live visual scale was ignored");
        const auto beforeClear = scene.GetRenderObjects()[0].GetPresentationTransform();
        Check(runtime->ClearPhysicsPresentation(*renderer, error) && !scene.HasPhysicsPresentation(),
            "Clear failed to restore the gizmo state");
        const auto cleared = scene.GetRenderObjects()[0].GetPresentationTransform();
        Check(SameMatrix(cleared.currentModel, edited.GetModelMatrix()), "Clear restored stale authored data");
        Check(SameMatrix(cleared.previousModel, beforeClear.previousModel), "Clear advanced deferred history");
        Check(!SameMatrix(cleared.currentModel, beforeClear.currentModel) && !scene.GetChangedPresentationTransforms().empty(),
            "Clear did not invalidate candidate transforms or schedule TLAS movement");
        scene.ClearChangedTransforms();
        CheckPresentation(scene.GetRenderObjects()[0]);

        SceneDocument live;
        Check(CaptureSceneDocument(document, scene, runtime->GetObjectMapping(), live, &error), "Cannot capture edited authored state");
        association = runtime->AssociatePhysicsPresentation(*renderer, live, error);
        Check(association.has_value(), "Cannot acquire fresh association after binding change");
        for (int cycle = 0; cycle < 16; ++cycle)
        {
            Check(runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Repeated apply failed");
            scene.ClearChangedTransforms();
            Check(runtime->ClearPhysicsPresentation(*renderer, error), "Repeated clear failed");
            scene.ClearChangedTransforms();
        }
        auto reloaded = MakeRuntime(live, resolver);
        renderer->m_sceneResourceScene = &reloaded->GetScene();
        Check(!reloaded->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Same IDs accepted after runtime reload");
        renderer->m_sceneResourceScene = &scene;
        Check(runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Cannot apply before membership fixture");
        auto removed = runtime->PrepareObjectDeletion("object-0", &error);
        Check(removed && !removed->GetScene().HasPhysicsPresentation(), "Membership replacement inherited active overrides");
        CheckPresentation(removed->GetScene().GetRenderObjects()[0]);
        renderer->m_sceneResourceScene = &removed->GetScene();
        Check(!removed->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Index reuse accepted stale association");
        renderer->m_sceneResourceScene = &scene;
        Check(runtime->ClearPhysicsPresentation(*renderer, error), "Final cleanup failed");
        Check(runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Cannot apply before direct insertion");
        scene.AddRenderObject(RenderObject{});
        Check(!scene.HasPhysicsPresentation(), "Direct structural mutation retained active overrides");
        Check(!runtime->ApplyPhysicsPresentation(*renderer, *association, *moved, error), "Stale structure revision accepted");
        renderer->m_sceneResourceScene = nullptr;
        renderer->m_sceneResourceLifetime = 0;
    }

    static void Run()
    {
        DiagnosticResolver resolver;
        SceneDocument document;
        document.id = "presentation-boundary-verification";
        SceneObjectDocument first;
        first.id = "stable-first";
        first.transform.position = {1.0f, 2.0f, 3.0f};
        first.transform.rotation = {0.2f, -0.3f, 0.4f};
        first.transform.scale = {2.0f, -3.0f, 4.0f};
        document.objects.push_back(first);
        auto second = first;
        second.id = "stable-second";
        second.transform.position.x = 20.0f;
        document.objects.push_back(second);
        auto runtime = MakeRuntime(document, resolver);
        std::string error;
        const auto identity = runtime->GetObjectMetadata("stable-first", &error);
        Check(identity.has_value(), "Cannot capture stable presentation identity");
        SceneDocument before;
        Check(CaptureSceneDocument(document, runtime->GetScene(), runtime->GetObjectMapping(), before, &error),
            "Cannot capture authored baseline");
        for (int read = 0; read < 8; ++read)
        {
            const auto presentation = runtime->GetObjectPresentation(*identity, &error);
            Check(presentation && SameMatrix(presentation->currentModel, first.transform.GetModelMatrix()),
                "Stable-ID read returned another object's matrix");
        }
        SceneDocument after;
        Check(CaptureSceneDocument(document, runtime->GetScene(), runtime->GetObjectMapping(), after, &error) &&
            SceneDocumentsEqual(before, after), "Presentation reads changed authored capture");
        Check(!runtime->GetFileScene().IsDirty(), "Presentation reads dirtied authored document");

        Transform edited = first.transform;
        edited.position.z = 8.0f;
        runtime->m_scene.SetRenderObjectTransform(identity->runtimeIndex, edited);
        const auto editedPresentation = runtime->GetObjectPresentation(*identity, &error);
        Check(editedPresentation && SameMatrix(editedPresentation->currentModel, edited.GetModelMatrix()),
            "Authored edit did not reach presentation under the same identity");
        Check(SameMatrix(editedPresentation->previousModel, first.transform.GetModelMatrix()),
            "Authored edit discarded previous presentation");

        // Existing binding revision is the authority, not an additional presentation epoch.
        ++runtime->m_objectMapping.bindingRevisions[identity->runtimeIndex];
        Check(!runtime->GetObjectPresentation(*identity, &error), "Stale binding identity was accepted");
        const auto rebound = runtime->GetObjectMetadata("stable-first", &error);
        Check(rebound && runtime->GetObjectPresentation(*rebound, &error), "Current binding identity was rejected");
        auto wrongIndex = *rebound;
        wrongIndex.runtimeIndex = 1;
        Check(!runtime->GetObjectPresentation(wrongIndex, &error), "Stable ID crossed into another runtime index");
        auto missing = *rebound;
        missing.objectID = "missing";
        Check(!runtime->GetObjectPresentation(missing, &error), "Unknown stable ID returned presentation");

        auto reloaded = MakeRuntime(document, resolver);
        Check(!reloaded->GetObjectPresentation(*rebound, &error), "Reload reused a stale runtime identity");
        auto removed = runtime->PrepareObjectDeletion("stable-first", &error);
        Check(removed != nullptr, "CPU membership replacement failed");
        Check(!removed->GetObjectPresentation(*rebound, &error), "Removed object remained presentation-addressable");
        const auto shifted = removed->GetObjectMetadata("stable-second", &error);
        Check(shifted && shifted->runtimeIndex == 0, "Fixture did not reuse renderer index zero");
        const auto shiftedPresentation = removed->GetObjectPresentation(*shifted, &error);
        Check(shiftedPresentation && SameMatrix(shiftedPresentation->currentModel, second.transform.GetModelMatrix()),
            "Reused index returned removed object's matrix");
        Check(SameMatrix(shiftedPresentation->currentModel, shiftedPresentation->previousModel),
            "Membership replacement inherited unrelated previous history");
    }

private:
    static std::unique_ptr<FileSceneRuntime> MakeRuntime(const SceneDocument& document, DiagnosticResolver& resolver)
    {
        auto runtime = std::make_unique<FileSceneRuntime>();
        std::string error;
        Check(runtime->m_fileScene.LoadDocument(document, &error), "Cannot load CPU fixture");
        PreparedScene prepared;
        Check(PrepareScene(document, resolver, prepared, &error), "Cannot prepare CPU fixture");
        Check(CommitPreparedEditorScene(prepared, document, runtime->m_scene, runtime->m_objectMapping, &error),
            "Cannot commit CPU fixture");
        return runtime;
    }
};

int VerifyPresentationTransforms()
{
    try
    {
        VerifyHistoryAndPicking();
        PresentationTransformVerification::Run();
        PresentationTransformVerification::VerifyOverrides();
        std::cout << "PASS: authored boundary; physics apply/replace/clear; independent successful/deferred history; mirrored scale; static/dynamic/kinematic; stale/malformed association rejection; reload/membership/index reuse; authored capture; repeated cycles. CPU fixture only, no GPU initialization.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: presentation boundary: " << exception.what() << '\n';
        return 1;
    }
}

int VerifyPhysicsSimulationController()
{
    try
    {
        PresentationTransformVerification::VerifySimulationController();
        std::cout << "PASS: real controller lifecycle, initial poses, 120/30 Hz equivalence, exact/zero/invalid time, retained catch-up debt, body types, save/authored separation, deferred history, owned stop/retry, supersession, stale binding/runtime/structure, repeated cleanup. CPU fixtures; no GPU initialization.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: physics simulation controller: " << exception.what() << '\n';
        return 1;
    }
}

int VerifyEditorPhysicsIntegration()
{
    try
    {
        PresentationTransformVerification::VerifyEditorPhysicsIntegration();
        std::cout << "PASS: Engine-owned editor Play/Stop, elapsed delivery, initial poses, deferred publish/history, pending Stop, editing guards, failed Start/stale feedback, selection/history/save preservation, replacement requests and teardown cleanup. CPU fixture; no GPU/editor initialization.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: editor physics integration: " << exception.what() << '\n';
        return 1;
    }
}
