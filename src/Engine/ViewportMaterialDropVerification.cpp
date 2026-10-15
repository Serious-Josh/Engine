#include "Engine/Engine.h"

#ifndef NDEBUG
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"
#include "assets/MaterialAssetSerializer.h"
#include <wrl/client.h>
#include <array>
#include <stdexcept>
#include <thread>

namespace
{
    void RequireDrop(bool value, const std::string& message)
    {
        if(!value) throw std::runtime_error("Viewport material drop: " + message);
    }
}

class ViewportMaterialDropVerification
{
public:
    static void Run(Engine& engine, FileBackedDemoScene& owner, ProjectAssetRegistry& registry,
        const std::array<std::optional<AssetID>, 4>& materials, const AssetID& freshMaterial)
    {
        auto& editor = engine.m_editor;
        auto& renderer = engine.m_renderer;
        auto& runtime = *owner.m_runtime;
        const ProjectPaths paths(owner.GetProjectRoot());
        std::array<std::string, 4> refs;
        for(size_t i = 0; i < refs.size(); ++i) refs[i] = materials[i]->ToReference();
        const std::map<std::string, std::string> mixed{{"a", refs[0]}, {"b", refs[1]}, {"c", refs[2]}};
        const std::map<std::string, std::string> allD{{"a", refs[3]}, {"b", refs[3]}, {"c", refs[3]}};
        const auto resetBindings = [&](const char* object, const std::map<std::string, std::string>& overrides)
        {
            SceneMaterialBindingPublication publication;
            auto result = publication.Prepare(renderer, runtime, owner.m_assets, *runtime.GetObjectMetadata(object), overrides);
            RequireDrop(result.status == SceneMaterialBindingPublication::Status::Ready, result.message);
            result = publication.PublishAtBoundary(renderer, runtime, owner.m_assets);
            RequireDrop(result.status == SceneMaterialBindingPublication::Status::Ready, result.message);
        };
        const auto bindings = [&](const char* object) { return runtime.GetObjectMetadata(object)->materialSlotBindings; };
        const auto drop = [&](const char* object, const AssetID& material)
        {
            editor.QueueViewportMaterialDrop(owner, *runtime.GetObjectMetadata(object), material);
            engine.ProcessViewportMaterialAssignment(owner);
        };
        const auto replay = [&](bool redo)
        {
            editor.ReplaySceneHistory(owner, redo);
            engine.ProcessViewportMaterialAssignment(owner);
        };
        const auto session = [&]()
        {
            RequireDrop(editor.UpdateSceneHistorySession(owner, paths, "Fixture"), "History session failed.");
        };
        try
        {
            resetBindings("single", {});
            resetBindings("multi", mixed);
            editor.OnSceneActivated();
            session();
            editor.SelectSceneObject(owner, 0);
            editor.SelectSceneObject(owner, 1, true);
            const auto selected = editor.m_orderedSelection;
            const auto selectedIndices = editor.GetSelectedObjects();
            const auto active = editor.GetSelectedObject();

            RequireDrop(owner.IsAssignmentMaterialAdmitted(freshMaterial.ToReference(), renderer), "Published Inspector version was not recognized.");
            drop("multi", freshMaterial);
            RequireDrop(bindings("multi").at("b") == freshMaterial.ToReference() && editor.m_historyCursor == 1,
                "Already-published fresh version was rejected.");
            replay(false);
            RequireDrop(bindings("multi") == mixed && editor.m_historyCursor == 0, "Fresh version undo failed.");
            replay(true);
            RequireDrop(bindings("multi").at("b") == freshMaterial.ToReference(), "Fresh version redo failed.");
            replay(false);
            editor.ClearSceneHistory();
            session();

            drop("single", *materials[3]);
            RequireDrop(bindings("single") == std::map<std::string, std::string>{{"default", refs[3]}} &&
                editor.m_historyCursor == 1 && !editor.HasPendingSceneHistoryOperation(),
                "Single-slot drop/history failed: " + editor.m_materialDropStatus + "; " + editor.m_sceneHistoryStatus);
            replay(false);
            RequireDrop(bindings("single").empty() && editor.m_historyCursor == 0, "Undo did not restore omitted overrides.");
            replay(true);
            RequireDrop(bindings("single").at("default") == refs[3] && editor.m_historyCursor == 1, "Single-slot redo failed.");
            drop("multi", *materials[3]);
            RequireDrop(bindings("multi") == allD && editor.m_historyCursor == 2, "Multi-slot drop failed.");
            replay(false);
            RequireDrop(bindings("multi") == mixed && editor.m_historyCursor == 1, "Undo did not restore mixed slots.");
            replay(true);
            RequireDrop(bindings("multi") == allD && editor.m_historyCursor == 2, "Multi-slot redo failed.");
            const auto entries = editor.m_sceneHistory.size();
            drop("multi", *materials[3]);
            RequireDrop(editor.m_sceneHistory.size() == entries && editor.m_historyCursor == 2 &&
                !engine.m_pendingViewportMaterialBinding, "No-op created history or a pending request.");
            RequireDrop(editor.m_orderedSelection == selected && editor.GetSelectedObjects() == selectedIndices &&
                editor.GetSelectedObject() == active, "Drop or replay changed multi-selection.");

            std::string error;
            RequireDrop(runtime.SaveAs(paths.GetScenesDirectory() / "material-drop.scene", &error), error);
            SceneDocument restored;
            RequireDrop(LoadSceneDocument(paths.GetScenesDirectory() / "material-drop.scene", restored, &error) &&
                restored.objects[1].materialSlotBindings == allD, "Dropped material did not survive serialization.");
            replay(false);
            RequireDrop(bindings("multi") == mixed, "Undo after save failed.");
            replay(true);

            editor.m_assetBrowser.SetThumbnailProject(paths, registry);
            const auto payload = editor.m_assetBrowser.MakeMaterialDragPayload(*materials[3]);
            RequireDrop(payload && editor.m_assetBrowser.ValidateMaterialDrag(*payload, paths, registry, error), "Typed material payload rejected.");
            RequireDrop(std::string_view(MaterialDragPayloadType) != MeshDragPayloadType &&
                std::string_view(MaterialDragPayloadType) != "ENGINE_ASSET_MOVE", "Material payload aliases another drag type.");
            const auto oldPath = registry.Find(*materials[3])->relativePath;
            const std::filesystem::path moved = "Materials/moved.material";
            std::filesystem::rename(paths.GetRoot() / oldPath, paths.GetRoot() / moved);
            RequireDrop(registry.UpdatePath(*materials[3], moved) &&
                editor.m_assetBrowser.ValidateMaterialDrag(*payload, paths, registry, error)->id == *materials[3],
                "Material payload lost stable identity after move.");
            editor.m_assetBrowser.InvalidateMeshDragIdentity();
            RequireDrop(!editor.m_assetBrowser.ValidateMaterialDrag(*payload, paths, registry, error), "Stale drag session accepted.");

            const auto originalB = registry.Find(*materials[1])->relativePath;
            RequireDrop(registry.Unregister(*materials[1]), "Fixture material removal failed.");
            replay(false);
            RequireDrop(editor.m_historyCursor == 2 && bindings("multi") == allD && !editor.HasPendingSceneHistoryOperation(),
                "Failed undo advanced cursor or partially changed slots.");
            RequireDrop(registry.Register({*materials[1], AssetType::Material, originalB}), "Fixture material restore failed.");

            const auto unadmitted = AssetID::Generate();
            const auto unusedTexture = AssetID::Generate();
            RequireDrop(unadmitted.has_value(), "Unadmitted material identity failed.");
            RequireDrop(unusedTexture.has_value(), "Unused texture identity failed.");
            std::filesystem::copy_file(paths.GetRoot() / "Textures/streamed.tga", paths.GetRoot() / "Textures/unused.tga");
            RequireDrop(registry.Register({*unusedTexture, AssetType::Texture, "Textures/unused.tga"}), "Unused texture registration failed.");
            MaterialAssetData unusedData;
            unusedData.baseColorTexture = unusedTexture->ToReference();
            RequireDrop(MaterialAssetSerializer::SaveToFile(unusedData, paths.GetMaterialsDirectory() / "unadmitted.material", &error) &&
                registry.Register({*unadmitted, AssetType::Material, "Materials/unadmitted.material"}), error);
            RequireDrop(!owner.IsAssignmentMaterialAdmitted(unadmitted->ToReference(), renderer), "Fixture unexpectedly admitted.");
            drop("multi", *unadmitted);
            RequireDrop(engine.m_pendingViewportMaterialBinding && editor.m_historyCursor == 2 && bindings("multi") == allD,
                "Unused material changed bindings/history before preparation.");
            unsigned frame = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while(engine.m_pendingViewportMaterialBinding && std::chrono::steady_clock::now() < deadline)
            {
                const auto slot = frame++ % CommandContext::FrameCount;
                RequireDrop(renderer.BeginTrackedRecording(slot), "Admission frame recording failed.");
                const auto generation = renderer.m_activeRecordingGeneration;
                const auto revision = renderer.m_materialPublication.revision;
                auto* commands = renderer.m_commandContext.GetCommandList();
                renderer.RecordPreparedTextureUploads(commands);
                RequireDrop(renderer.RecordMaterialTable(slot, commands) && renderer.EndTrackedRecording() &&
                    renderer.ExecuteTrackedRecording(slot), "Admission frame submission failed.");
                const auto fence = renderer.m_commandContext.GetLastSubmissionFenceValue();
                for(auto& upload : renderer.m_preparedTextureUploads) upload.upload->ConfirmSubmission(generation, fence);
                RequireDrop(renderer.m_materialDependencies.Confirm(slot, fence), "Admission dependency confirmation failed.");
                renderer.m_materialPublication.ConfirmSubmission(slot, revision);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                renderer.PollPreparedTextureUploads();
                engine.ProcessViewportMaterialAssignment(owner);
            }
            const std::map<std::string, std::string> allUnused{{"a", unadmitted->ToReference()},
                {"b", unadmitted->ToReference()}, {"c", unadmitted->ToReference()}};
            RequireDrop(!engine.m_pendingViewportMaterialBinding && editor.m_historyCursor == 3 && bindings("multi") == allUnused &&
                owner.IsAssignmentMaterialAdmitted(unadmitted->ToReference(), renderer),
                "Unused material preparation/admission/drop failed: " + editor.m_materialDropStatus);
            const auto materialPin = CreateFileW((paths.GetMaterialsDirectory() / "unadmitted.material").c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            RequireDrop(materialPin != INVALID_HANDLE_VALUE, "Successful admission retained the CPU preparation ticket.");
            CloseHandle(materialPin);
            RequireDrop(runtime.SaveAs(paths.GetScenesDirectory() / "unused-material-drop.scene", &error) &&
                LoadSceneDocument(paths.GetScenesDirectory() / "unused-material-drop.scene", restored, &error) &&
                restored.objects[1].materialSlotBindings == allUnused, "Unused assignment did not persist.");
            drop("multi", *unadmitted);
            RequireDrop(editor.m_historyCursor == 3 && !engine.m_pendingViewportMaterialBinding, "Unused material no-op created history.");
            replay(false);
            RequireDrop(editor.m_historyCursor == 2 && bindings("multi") == allD, "Unused drop undo failed.");
            replay(true);
            RequireDrop(editor.m_historyCursor == 3 && bindings("multi") == allUnused, "Unused drop redo failed.");
            replay(false);

            // A newer no-op drop cancels an older, still-unadmitted request.
            // Pending table-slot retirement keeps the old request from accepting.
            const auto superseded = *AssetID::Generate();
            RequireDrop(MaterialAssetSerializer::SaveToFile({}, paths.GetMaterialsDirectory() / "superseded.material", &error) &&
                registry.Register({superseded, AssetType::Material, "Materials/superseded.material"}), error);
            drop("multi", superseded);
            RequireDrop(engine.m_pendingViewportMaterialBinding.has_value(), "Supersession fixture did not remain pending.");
            const auto canceledTicket = engine.m_pendingViewportMaterialBinding->preparationTicket;
            drop("multi", *materials[3]);
            RequireDrop(!engine.m_pendingViewportMaterialBinding && !editor.HasPendingSceneHistoryOperation() &&
                editor.m_historyCursor == 2 && bindings("multi") == allD && engine.m_materialPreparation && canceledTicket &&
                engine.m_materialPreparation->Poll(canceledTicket, paths, registry, owner.GetRuntimeIdentity()).status ==
                    ProjectMaterialPreparationService::Status::Stale,
                "Newer no-op drop did not cancel old preparation without history.");

            auto invalid = *runtime.GetObjectMetadata("multi");
            invalid.objectID = "missing";
            editor.QueueViewportMaterialDrop(owner, invalid, *materials[0]);
            RequireDrop(!editor.m_viewportMaterialRequest && !editor.HasPendingSceneHistoryOperation(), "Invalid object queued.");
            editor.QueueViewportMaterialDrop(owner, *runtime.GetObjectMetadata("multi"), *materials[0]);
            const auto layout = owner.GetScene().GetRenderObjects()[1].mesh->GetMetadata();
            auto changedLayout = layout;
            changedLayout.materialSlots[0].name = "Changed";
            RequireDrop(owner.GetScene().GetRenderObjects()[1].mesh->SetMetadata(changedLayout, error), error);
            engine.ProcessViewportMaterialAssignment(owner);
            RequireDrop(bindings("multi") == allD && editor.m_historyCursor == 2 && !editor.HasPendingSceneHistoryOperation(),
                "Changed layout was published.");
            RequireDrop(owner.GetScene().GetRenderObjects()[1].mesh->SetMetadata(layout, error), error);
            editor.QueueViewportMaterialDrop(owner, *runtime.GetObjectMetadata("multi"), *materials[0]);
            FileBackedDemoScene replacement(paths.GetScenesDirectory() / "replacement.scene");
            engine.ProcessViewportMaterialAssignment(replacement);
            RequireDrop(!engine.m_pendingViewportMaterialBinding && !editor.HasPendingSceneHistoryOperation() &&
                editor.m_historyCursor == 2 && bindings("multi") == allD, "Scene replacement left a pending edit.");

            editor.m_materialDropHover = runtime.GetObjectMetadata("multi");
            RequireDrop(editor.GetMaterialDropHighlight(owner) == 1 && editor.m_orderedSelection == selected, "Hover changed selection.");
            editor.DrawMaterialDropTarget(owner, paths, registry); // No drag active: clear transient hover.
            RequireDrop(!editor.GetMaterialDropHighlight(owner), "Ended drag retained hover.");

            // Hold one real submission. Four accepted changes retain old buffers;
            // the fifth must remain pending without changing the history cursor.
            editor.ClearSceneHistory();
            session();
            Microsoft::WRL::ComPtr<ID3D12Fence> gate;
            RequireDrop(SUCCEEDED(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "Gate failed.");
            struct GateCleanup { ID3D12Fence* fence; ~GateCleanup() { fence->Signal(1); } } releaseGate{gate.Get()};
            RequireDrop(renderer.BeginTrackedRecording(0) && renderer.EndTrackedRecording(), "Held recording failed.");
            RequireDrop(SUCCEEDED(renderer.m_commandContext.GetCommandQueue()->Wait(gate.Get(), 1)) &&
                renderer.ExecuteTrackedRecording(0), "Held submission failed.");
            for(unsigned i = 0; i < 4; ++i) drop("multi", *materials[i % 2 ? 3 : 0]);
            RequireDrop(editor.m_historyCursor == 4, "Retirement fixture did not publish four edits.");
            drop("multi", *materials[0]);
            RequireDrop(engine.m_pendingViewportMaterialBinding && engine.m_pendingViewportMaterialBinding->prepared &&
                editor.HasPendingSceneHistoryOperation() && editor.m_historyCursor == 4 && bindings("multi") == allD,
                "Retirement pressure changed the scene/history before acceptance.");
            engine.ProcessViewportMaterialAssignment(replacement);
            RequireDrop(!engine.m_pendingViewportMaterialBinding && !editor.HasPendingSceneHistoryOperation() &&
                editor.m_historyCursor == 4 && bindings("multi") == allD, "Scene replacement retained a prepared retry.");
            replay(false);
            RequireDrop(engine.m_pendingViewportMaterialBinding && engine.m_pendingViewportMaterialBinding->prepared &&
                editor.m_historyCursor == 4 && bindings("multi") == allD, "Retryable undo advanced its cursor early.");
            RequireDrop(SUCCEEDED(gate->Signal(1)) && renderer.WaitForGPU(), "Held submission completion failed.");
            engine.ProcessViewportMaterialAssignment(owner);
            RequireDrop(!engine.m_pendingViewportMaterialBinding && !editor.HasPendingSceneHistoryOperation() &&
                editor.m_historyCursor == 3 && bindings("multi").at("b") == refs[0], "Retry did not complete undo atomically.");
            replay(true);
            RequireDrop(editor.m_historyCursor == 4 && bindings("multi") == allD, "Redo after delayed undo failed.");
            drop("multi", *materials[0]);
            RequireDrop(editor.m_historyCursor == 5 && bindings("multi").at("b") == refs[0], "Forward retry cancellation left duplicate history.");
            replay(false);
            RequireDrop(editor.m_historyCursor == 4 && bindings("multi") == allD, "Undo of delayed publication failed.");
            replay(true);
            RequireDrop(editor.m_historyCursor == 5 && bindings("multi").at("b") == refs[0], "Redo of delayed publication failed.");

            editor.QueueViewportMaterialDrop(owner, *runtime.GetObjectMetadata("multi"), *materials[3]);
            auto expired = editor.TakeViewportMaterialRequest();
            RequireDrop(expired.has_value(), "Timeout fixture failed.");
            engine.m_pendingViewportMaterialBinding.emplace(Engine::PendingViewportMaterialBinding{
                std::move(*expired), std::chrono::steady_clock::now() - std::chrono::seconds(11), false});
            engine.ProcessViewportMaterialAssignment(owner);
            RequireDrop(!engine.m_pendingViewportMaterialBinding && !editor.HasPendingSceneHistoryOperation() &&
                editor.m_historyCursor == 5 && bindings("multi").at("b") == refs[0], "Timed-out request survived or changed bindings.");
            RequireDrop(editor.m_orderedSelection == selected && editor.GetSelectedObjects() == selectedIndices &&
                editor.GetSelectedObject() == active, "Failure/retry changed selection.");
            resetBindings("multi", allD); // Preserve the enclosing publication fixture's expected state.
            editor.OnSceneActivated();
            Log::Info("Viewport material drop integration passed: typed/stale/moved payloads, single/multiple slots, mixed undo/redo, no-op, save/load, selection/hover preservation, invalid target/layout/scene/material, retirement retry and timeout.");
        }
        catch(...)
        {
            engine.CancelViewportMaterialAssignment("Diagnostic canceled.");
            editor.OnSceneActivated();
            throw;
        }
    }
};

void VerifyViewportMaterialDropIntegration(Engine& engine, FileBackedDemoScene& scene,
    ProjectAssetRegistry& registry, const std::array<std::optional<AssetID>, 4>& materials, const AssetID& freshMaterial)
{
    ViewportMaterialDropVerification::Run(engine, scene, registry, materials, freshMaterial);
}
#endif
