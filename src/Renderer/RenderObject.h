#pragma once

#include "Scene/Transform.h"
#include "Scene/PresentationTransform.h"
#include "Physics/RuntimePoseSnapshot.h"
#include <optional>
#include <map>
#include <string>

class Mesh;
struct Material;

struct RenderObject
{
    // Resident GPU geometry only. A null pointer does not describe the authored
    // assignment, which file-backed scenes retain in SceneObjectDocument.
    Mesh* mesh = nullptr;

    Material* material = nullptr;
    // Packaged references retained in the document; no substitute GPU material.
    // Deferred objects must remain mesh-pending until packaged material admission.
    bool materialsDeferred = false;

    Transform transform;
    Transform previousTransform;

    // Copied read of the pose already accepted by Scene; never exposes a body/session.
    std::optional<Physics::Pose> GetPhysicsPresentationPose() const noexcept
    {
        return presentationPose;
    }
    // Committed rendered world. Parented objects read the Scene-evaluated hierarchy world;
    // roots keep the original local or accepted-pose composition unchanged.
    PresentationTransform GetPresentationTransform() const
    {
        if (hierarchyWorld)
        {
            return {*hierarchyWorld, previousHierarchyWorld.value_or(*hierarchyWorld)};
        }

        const Matrix4 current = presentationPose
            ? Physics::ComposeRuntimeModelMatrix(*presentationPose, transform.scale)
            : transform.GetModelMatrix();
        return {current, previousHierarchyWorld.value_or(
            previousPresentedModel.value_or(previousTransform.GetModelMatrix()))};
    }

    // Effective runtime borrows, not authored overrides. The document retains
    // overrides independently of geometry/metadata availability.
    // Pending objects may retain explicit overrides only, not a complete slot table.
    // Empty maps retain legacy fallback behavior.
    std::map<std::string, Material*> slotMaterials;

    Material* GetSlotMaterial(const std::string& slotID) const noexcept
    {
        const auto found = slotMaterials.find(slotID);
        return found == slotMaterials.end() ? material : found->second;
    }

private:
    friend class Scene;
    // Scene owns these copied values. Candidates may copy them, but never own bodies.
    std::optional<Physics::Pose> presentationPose;
    std::optional<Matrix4> previousPresentedModel;
    bool presentationDirty = false;

    // H.3 derived values, written only by Scene's hierarchy commit. Present only for
    // objects with a parent. Previous advances only in Scene::ClearChangedTransforms.
    // A previous value may outlive its current one for one render after detachment.
    std::optional<Matrix4> hierarchyWorld;
    std::optional<Matrix4> previousHierarchyWorld;
};
