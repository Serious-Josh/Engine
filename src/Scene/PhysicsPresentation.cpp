#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Renderer/Renderer.h"

#include <exception>
#include <array>
#include <unordered_set>
#include <type_traits>
#include <utility>

bool FileSceneRuntime::OwnsPhysicsPresentation(const Renderer& renderer, const PhysicsPresentationLease& lease) const noexcept
{
    return lease.revision != 0 && lease.revision == m_physicsPresentationRevision &&
        lease.runtimeIdentity == m_instanceIdentity && lease.rendererLifetime == renderer.m_sceneResourceLifetime &&
        renderer.m_sceneResourceScene == &m_scene && m_scene.HasPhysicsPresentation() && m_activePresentationAssociation.has_value();
}

PhysicsPresentationReadiness FileSceneRuntime::GetPhysicsPresentationBoundaryReadiness(
    const Renderer& renderer, std::string& error, bool clearing) const
{
    if (std::this_thread::get_id() != m_ownerThread)
    {
        error = "Physics presentation must be accessed on its owning thread.";
        return PhysicsPresentationReadiness::Failed;
    }
    if (renderer.m_commandSubmissionAttempted || renderer.m_resizeFailed || renderer.m_geometryUpdateFailed ||
        renderer.m_sceneInitializationAttempt.failed)
    {
        error = "Renderer failure requires checked recovery before physics presentation publication.";
        return PhysicsPresentationReadiness::Failed;
    }
    return ValidatePhysicsPresentationBoundary(renderer, error, clearing)
        ? PhysicsPresentationReadiness::Ready : PhysicsPresentationReadiness::Retry;
}

PhysicsPresentationReadiness FileSceneRuntime::CheckPhysicsPresentation(Renderer& renderer,
    const PhysicsPresentationAssociation& association, const PhysicsPresentationLease& lease, std::string& error) const
{
    error.clear();
    try
    {
        if (std::this_thread::get_id() != m_ownerThread)
        {
            return GetPhysicsPresentationBoundaryReadiness(renderer, error, false);
        }
        if (!OwnsPhysicsPresentation(renderer, lease) || !ValidatePhysicsPresentationAssociation(renderer, association, error))
        {
            if (error.empty()) error = "Physics presentation was cleared, replaced or moved to another scene/renderer lifetime.";
            return PhysicsPresentationReadiness::Stale;
        }
        return GetPhysicsPresentationBoundaryReadiness(renderer, error, false);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return PhysicsPresentationReadiness::Failed;
    }
}

PhysicsPresentationReadiness FileSceneRuntime::ClearOwnedPhysicsPresentation(Renderer& renderer,
    const PhysicsPresentationLease& lease, std::string& error)
{
    error.clear();
    if (std::this_thread::get_id() != m_ownerThread)
    {
        return GetPhysicsPresentationBoundaryReadiness(renderer, error, true);
    }
    if (!OwnsPhysicsPresentation(renderer, lease))
    {
        error = "Owned physics presentation is no longer current; no presentation was cleared.";
        return PhysicsPresentationReadiness::Stale;
    }
    const auto readiness = GetPhysicsPresentationBoundaryReadiness(renderer, error, true);
    if (readiness != PhysicsPresentationReadiness::Ready)
    {
        return readiness;
    }
    return ClearPhysicsPresentation(renderer, error) ? PhysicsPresentationReadiness::Ready : PhysicsPresentationReadiness::Failed;
}

bool FileSceneRuntime::ValidatePhysicsPresentationBoundary(const Renderer& renderer, std::string& error, bool clearing) const
{
    if (std::this_thread::get_id() != m_ownerThread ||
        !renderer.CanEditPendingMaterialBindings(m_scene) ||
        (!clearing && (HasPreparedScene() || HasMaterialAssignment())))
    {
        error = "Physics presentation requires the owning main thread, active renderer scene and an idle publication boundary.";
        return false;
    }
    return true;
}

std::optional<PhysicsPresentationAssociation> FileSceneRuntime::AssociatePhysicsPresentation(
    Renderer& renderer, const SceneDocument& registeredDocument, std::string& error) const
{
    error.clear();
    try
    {
        if (!ValidatePhysicsPresentationBoundary(renderer, error))
        {
            return std::nullopt;
        }
        SceneDocument live;
        if (!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, live, &error))
        {
            return std::nullopt;
        }
        if (!SceneDocumentsEqual(live, registeredDocument))
        {
            error = "Register physics from the current captured authored document before associating presentation.";
            return std::nullopt;
        }
        PhysicsPresentationAssociation association;
        association.runtimeIdentity = m_instanceIdentity;
        association.structureRevision = m_scene.GetStructureRevision();
        association.rendererLifetime = renderer.m_sceneResourceLifetime;
        association.materialBindingEpoch = renderer.m_rayTracingScene.m_materialBindingEpoch;
        association.materialTableRevision = renderer.GetMaterialTableRevision();
        for (const auto& object : live.objects)
        {
            auto identity = GetObjectMetadata(object.id, &error);
            if (!identity)
            {
                return std::nullopt;
            }
            association.objects.push_back(std::move(*identity));
            association.components.push_back(object.physics);
            association.triggerComponents.push_back(object.triggerCollider);
            association.controllerComponents.push_back(object.characterController);
        }
        return association;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return std::nullopt;
    }
}

bool FileSceneRuntime::ValidatePhysicsPresentationAssociation(const Renderer& renderer,
    const PhysicsPresentationAssociation& association, std::string& error) const
{
    if (association.rendererLifetime != renderer.m_sceneResourceLifetime ||
        association.materialBindingEpoch != renderer.m_rayTracingScene.m_materialBindingEpoch ||
        association.materialTableRevision != renderer.GetMaterialTableRevision())
    {
        error = "Physics presentation renderer lifetime or DXR/material publication epoch is stale.";
        return false;
    }
    const auto& document = m_fileScene.GetDocument();
    if(association.runtimeIdentity != m_instanceIdentity ||
        association.structureRevision != m_scene.GetStructureRevision() ||
        association.objects.size() != document.objects.size() ||
        association.components.size() != document.objects.size() ||
        association.triggerComponents.size() != document.objects.size() ||
        association.controllerComponents.size() != document.objects.size())
    {
        error = "Physics presentation association belongs to an obsolete runtime or scene membership.";
        return false;
    }
    for (std::size_t index = 0; index < association.objects.size(); ++index)
    {
        const auto& identity = association.objects[index];
        if (!GetObjectPresentation(identity, &error))
        {
            return false;
        }
        const auto& object = document.objects[index];
        if(object.id != identity.objectID || object.physics != association.components[index] ||
            object.triggerCollider != association.triggerComponents[index] ||
            object.characterController != association.controllerComponents[index])
        {
            error = "Authored physics configuration changed; register a new session and association.";
            return false;
        }
    }
    return true;
}

bool FileSceneRuntime::PreparePhysicsPresentation(const Physics::RuntimePoseSnapshot& snapshot,
    std::vector<std::optional<Physics::Pose>>& poses,
    std::string& error) const
{
    poses.assign(m_scene.GetRenderObjects().size(), std::nullopt);
    std::unordered_set<std::string> expectedIDs;
    for (const auto& component : m_fileScene.GetDocument().objects)
    {
        // Each runtime pose has one owner: Physics and Character Controller are exclusive.
        if (component.physics || component.characterController) expectedIDs.insert(component.id);
    }
    if (snapshot.GetEntries().size() != expectedIDs.size())
    {
        error = "Physics presentation requires exactly one pose for every registered physics or controller object.";
        return false;
    }
    for (const auto& entry : snapshot.GetEntries())
    {
        if (expectedIDs.erase(entry.objectID) != 1)
        {
            error = "Unknown or duplicate physics presentation object ID: " + entry.objectID;
            return false;
        }
        if (!Physics::ValidateRuntimePose(entry.pose, &error))
        {
            return false;
        }
        const auto identity = GetObjectMetadata(entry.objectID, &error);
        if (!identity)
        {
            return false;
        }
        // Prove composition is valid before the allocation-free acceptance below.
        Physics::ComposeRuntimeModelMatrix(entry.pose, m_scene.GetRenderObjects()[identity->runtimeIndex].transform.scale);
        poses[identity->runtimeIndex] = entry.pose;
    }
    return true;
}

bool FileSceneRuntime::ApplyPhysicsPresentation(Renderer& renderer,
    const PhysicsPresentationAssociation& association, const Physics::RuntimePoseSnapshot& snapshot, std::string& error,
    PhysicsPresentationLease* acceptedLease)
{
    error.clear();
    try
    {
        if (!ValidatePhysicsPresentationBoundary(renderer, error))
        {
            return false;
        }
        if (!ValidatePhysicsPresentationAssociation(renderer, association, error))
        {
            return false;
        }
        if (m_physicsPresentationRevision == UINT64_MAX)
        {
            error = "Physics presentation ownership revisions are exhausted.";
            return false;
        }
        std::vector<std::optional<Physics::Pose>> poses;
        if (!PreparePhysicsPresentation(snapshot, poses, error))
        {
            return false;
        }
        // All fallible work precedes acceptance. Main-thread publication has no
        // callbacks or submission between validation and commit. Existing async
        // candidates compare these effective matrices before adoption and refresh.
        for (const auto& object : m_scene.GetRenderObjects())
        {
            object.GetPresentationTransform();
        }
        // Hierarchy descendants are evaluated against exactly these accepted poses.
        if (!ValidateHierarchyAssociation(&error))
        {
            return false;
        }

        std::optional<PhysicsPresentationAssociation> acceptedAssociation(association);

        if (!m_scene.PreparePresentationCommit(poses))
        {
            error = "Accepted physics poses produce an invalid hierarchy descendant world.";
            return false;
        }

        m_scene.CommitPresentation(poses, true);
        static_assert(std::is_nothrow_swappable_v<decltype(m_activePresentationAssociation)>);
        m_activePresentationAssociation.swap(acceptedAssociation);
        ++m_physicsPresentationRevision;
        if (acceptedLease)
        {
            acceptedLease->runtimeIdentity = m_instanceIdentity;
            acceptedLease->rendererLifetime = renderer.m_sceneResourceLifetime;
            acceptedLease->revision = m_physicsPresentationRevision;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool FileSceneRuntime::ClearPhysicsPresentation(Renderer& renderer, std::string& error)
{
    error.clear();
    try
    {
        // Clear changes no authored data or bindings. Prepared candidates remain
        // protected by their existing matrix freshness checks at acceptance.
        if (!ValidatePhysicsPresentationBoundary(renderer, error, true))
        {
            return false;
        }
        for (const auto& object : m_scene.GetRenderObjects())
        {
            object.GetPresentationTransform();
        }
        // Authored descendant worlds are validated on every edit, so restoring them cannot fail.
        if (!m_scene.PreparePresentationCommit())
        {
            error = "Authored hierarchy worlds are invalid; physics presentation was retained.";
            return false;
        }

        m_scene.CommitPresentation({}, false);
        m_activePresentationAssociation.reset();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void FileSceneRuntime::RevalidatePhysicsPresentationBindings(const Renderer& renderer) noexcept
{
    if (!m_activePresentationAssociation || !m_scene.HasPhysicsPresentation()) return;
    // Membership, runtime and index identity cannot change in binding-only commits.
    // Existing publication acceptance has already checked the complete source map
    // and candidate's effective matrices. No allocation or GPU work occurs here.
    auto& association = *m_activePresentationAssociation;
    for (auto& object : association.objects)
    {
        object.bindingRevision = m_objectMapping.bindingRevisions[object.runtimeIndex];
    }
    association.materialBindingEpoch = renderer.m_rayTracingScene.m_materialBindingEpoch;
    association.materialTableRevision = renderer.GetMaterialTableRevision();
}

bool FileSceneRuntime::CaptureEditorColliderOverlay(const Renderer& renderer, EditorColliderOverlay& output,
    std::string& error, std::span<const Physics::CharacterControllerDiagnostics> controllerDiagnostics) const
{
    // Failure must leave no old frame payload for a caller to accidentally reuse.
    output = {};
    error.clear();
    if(m_archiveScene)
    {
        return false;
    }
    if(std::this_thread::get_id() != m_ownerThread)
    {
        return false;
    }
    const bool ownsActiveRendererScene = renderer.m_sceneResourceScene == &m_scene &&
        renderer.m_sceneResourceLifetime != 0;
    if(!ownsActiveRendererScene)
    {
        return false;
    }

    const auto& authoredObjects = m_fileScene.GetDocument().objects;
    // Both optional components count independently, across all six shape/role batches.
    std::size_t colliderCount = 0;
    std::size_t characterCount = 0;
    std::array<std::array<std::size_t, 3>, 2> counts{};
    for(const auto& authoredObject : authoredObjects)
    {
        const auto countComponent = [&](const auto& component, std::size_t role)
        {
            if(!component)
            {
                return true;
            }

            const auto shape = static_cast<std::size_t>(component->colliderType);

            if(shape >= counts[role].size() || colliderCount == EditorColliderOverlay::MaxColliders)
            {
                return false;
            }

            ++colliderCount;
            ++counts[role][shape];
            return true;
        };

        if(!countComponent(authoredObject.physics, 0) || !countComponent(authoredObject.triggerCollider, 1))
        {
            error = "Editor collider overlay capacity exceeded or shape is invalid.";
            return false;
        }

        characterCount += authoredObject.characterController ? 1 : 0;
    }
    // Every controller may add a capsule plus a bounded set of diagnostic markers.
    const std::size_t remainingCapacity = EditorColliderOverlay::MaxColliders - colliderCount;

    if(characterCount > remainingCapacity / EditorColliderOverlay::MaxMarkersPerCharacter)
    {
        error = "Editor collider overlay capacity exceeded by Character Controller diagnostics.";
        return false;
    }
    if(colliderCount == 0 && characterCount == 0)
    {
        return true;
    }
    try
    {
        SceneDocument currentDocument;
        if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, currentDocument, &error))
        {
            return false;
        }

        const bool presentationActive = m_scene.HasPhysicsPresentation();
        if(presentationActive)
        {
            // Copied poses are usable only while their original stable-ID/binding
            // association is current. Deferred publication must not use newer solver poses.
            if(!m_activePresentationAssociation)
            {
                return false;
            }
            if(!ValidatePhysicsPresentationAssociation(renderer, *m_activePresentationAssociation, error))
            {
                return false;
            }
        }

        EditorColliderOverlay capturedOverlay;
        capturedOverlay.scene = &m_scene;
        capturedOverlay.structureRevision = m_scene.GetStructureRevision();
        capturedOverlay.rendererLifetime = renderer.m_sceneResourceLifetime;
        capturedOverlay.boxColliderWorldTransforms.reserve(counts[0][0]);
        capturedOverlay.sphereColliderWorldTransforms.reserve(counts[0][1]);
        capturedOverlay.capsuleColliders.reserve(counts[0][2]);
        capturedOverlay.triggerBoxWorldTransforms.reserve(counts[1][0]);
        capturedOverlay.triggerSphereWorldTransforms.reserve(counts[1][1]);
        capturedOverlay.triggerCapsules.reserve(counts[1][2]);
        capturedOverlay.characterMarkers.reserve(characterCount * EditorColliderOverlay::MaxMarkersPerCharacter);

        for(const auto& authoredObject : currentDocument.objects)
        {
            if(!authoredObject.physics && !authoredObject.triggerCollider && !authoredObject.characterController)
            {
                continue;
            }

            const auto objectMetadata = GetObjectMetadata(authoredObject.id, &error);

            if(!objectMetadata)
            {
                return false;
            }

            // Each builder accepts only its own collider type, so a shape can never be
            // drawn with another shape's geometry.
            const RenderObject& renderObject = m_scene.GetRenderObjects()[objectMetadata->runtimeIndex];
            const auto appendComponent = [&](const auto& component, bool trigger, bool usePresentation)
            {
                auto& boxes = trigger ? capturedOverlay.triggerBoxWorldTransforms :
                    capturedOverlay.boxColliderWorldTransforms;
                auto& spheres = trigger ? capturedOverlay.triggerSphereWorldTransforms :
                    capturedOverlay.sphereColliderWorldTransforms;
                auto& capsules = trigger ? capturedOverlay.triggerCapsules : capturedOverlay.capsuleColliders;
                bool placementValid = false;

                switch(component.colliderType)
                {
                case Physics::ColliderType::Sphere:
                {
                    Matrix4 colliderWorldTransform;
                    placementValid = BuildColliderSphereMatrix(component, renderObject, usePresentation,
                        colliderWorldTransform);

                    if(placementValid)
                    {
                        spheres.push_back(colliderWorldTransform);
                    }

                    break;
                }

                case Physics::ColliderType::Capsule:
                {
                    CapsuleColliderOverlay capsuleCollider;
                    placementValid = BuildColliderCapsule(component, renderObject, usePresentation, capsuleCollider);

                    if(placementValid)
                    {
                        capsules.push_back(capsuleCollider);
                    }

                    break;
                }

                default:
                {
                    Matrix4 colliderWorldTransform;
                    placementValid = BuildColliderBoxMatrix(component, renderObject, usePresentation,
                        colliderWorldTransform);

                    if(placementValid)
                    {
                        boxes.push_back(colliderWorldTransform);
                    }

                    break;
                }
                }

                return placementValid;
            };

            const bool solidValid = !authoredObject.physics || appendComponent(*authoredObject.physics, false, presentationActive);

            // Internal Static sensor hosts retain the locked authored pose, while triggers on a
            // presented body or a controller's Kinematic host use only the accepted
            // presentation, never a newer solver snapshot.
            const bool triggerFollowsPresentation = presentationActive &&
                (authoredObject.physics.has_value() || authoredObject.characterController.has_value());
            const bool triggerValid = !authoredObject.triggerCollider || appendComponent(*authoredObject.triggerCollider, true,
                triggerFollowsPresentation);
            bool characterValid = true;

            if(authoredObject.characterController)
            {
                CapsuleColliderOverlay capsule;
                characterValid = BuildCharacterControllerCapsule(*authoredObject.characterController, renderObject,
                    presentationActive, capsule);

                // Diagnostics are drawn only when they describe exactly the presented pose.
                const Physics::CharacterControllerDiagnostics* diagnostics = nullptr;

                const auto presentedPose = renderObject.GetPhysicsPresentationPose();

                if(characterValid && presentationActive && presentedPose)
                {
                    const auto& presented = *presentedPose;

                    for(const auto& candidate : controllerDiagnostics)
                    {
                        const auto& pose = candidate.pose;
                        const bool samePose = pose.position.x == presented.position.x &&
                            pose.position.y == presented.position.y && pose.position.z == presented.position.z &&
                            pose.orientation.x == presented.orientation.x && pose.orientation.y == presented.orientation.y &&
                            pose.orientation.z == presented.orientation.z && pose.orientation.w == presented.orientation.w;

                        if(candidate.objectID == authoredObject.id && samePose)
                        {
                            diagnostics = &candidate;
                            break;
                        }
                    }
                }

                const bool alert = diagnostics && (diagnostics->recovery == Physics::CharacterRecovery::Unresolved ||
                    diagnostics->lastStatus == Physics::CharacterMoveStatus::PenetrationRejected ||
                    diagnostics->lastStatus == Physics::CharacterMoveStatus::QueryCapacityExceeded);

                if(characterValid)
                {
                    capturedOverlay.characterMarkers.push_back({capsule.colliderWorldTransform, ColliderOverlayShape::Capsule,
                        alert ? ColliderOverlayRole::CharacterAlert : ColliderOverlayRole::CharacterController,
                        capsule.halfCylinderHeightInRadii});
                }

                if(diagnostics)
                {
                    AppendCharacterDiagnosticMarkers(*diagnostics, capturedOverlay.characterMarkers);
                }
            }

            if(!solidValid || !triggerValid || !characterValid)
            {
                error = "Collider placement is invalid or its accepted presentation pose is unavailable: " + authoredObject.id;
                return false;
            }
        }

        // Publish the owned matrix copies only after every collider passed validation.
        output = std::move(capturedOverlay);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
