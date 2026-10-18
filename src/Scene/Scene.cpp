#include "Scene.h"

#include "Scene/SceneHierarchyTransforms.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

RenderObjectID Scene::AddRenderObject(const RenderObject& object)
{
    // Structural mutation invalidates the entire temporary registration and any
    // installed hierarchy; the owner must install a new topology for the new slots.
    ClearHierarchy();

    if (m_physicsPresentationActive)
    {
        PreparePresentationCommit();
        CommitPresentation({}, false);
    }
    const RenderObjectID id = m_renderObjects.size();

    m_renderObjects.push_back(object);
    m_renderObjects.back().previousTransform = object.transform;
    m_renderObjects.back().presentationPose.reset();
    m_renderObjects.back().previousPresentedModel.reset();
    m_renderObjects.back().presentationDirty = false;
    m_renderObjects.back().hierarchyWorld.reset();
    m_renderObjects.back().previousHierarchyWorld.reset();

    m_transformDirty.push_back(false);

    ++m_structureRevision;

    return id;
}

void Scene::ClearRenderObjects()
{
    m_physicsPresentationActive = false;
    m_changedPresentationTransforms.clear();
    m_renderObjects.clear();
    m_changedTransforms.clear();
    m_transformDirty.clear();
    m_hierarchy = {};

    ++m_structureRevision;
}

void Scene::ReplaceRenderObjects(std::vector<RenderObject> objects)
{
    // A flat replacement cannot fail.
    ReplaceRenderObjects(std::move(objects), SceneHierarchyTopology{}, nullptr);
}

bool Scene::ReplaceRenderObjects(std::vector<RenderObject> objects, SceneHierarchyTopology topology,
    std::string* error)
{
    for(auto& object : objects)
    {
        object.previousTransform = object.transform;
        object.presentationPose.reset();
        object.previousPresentedModel.reset();
        object.presentationDirty = false;
        object.hierarchyWorld.reset();
        object.previousHierarchyWorld.reset();
    }

    // Validate and evaluate the candidate hierarchy against the incoming objects before any
    // committed state changes. An empty topology means the replacement is flat.
    const size_t count = objects.size();
    const bool flat = topology.parentSlots.empty() && topology.parentBeforeChild.empty();
    std::vector<bool> hasChildren;
    bool hasEdges = false;

    if(!flat && !ValidateHierarchyTopology(topology, count, hasChildren, hasEdges, error))
    {
        return false;
    }

    std::vector<Matrix4> worlds;
    std::vector<const Transform*> overrides;

    if(hasEdges)
    {
        worlds.resize(count);
        overrides.assign(count, nullptr);
        HierarchyInputs authored;
        authored.useCommittedPoses = false;

        if(!EvaluateHierarchy(objects, topology, hasChildren, authored, worlds, error))
        {
            return false;
        }
    }

    std::vector<bool> dirty(count, false);
    std::vector<RenderObjectID> changed;
    changed.reserve(hasEdges ? count : 0);

    // Commit point. New slots start with no carried-over world; a copied donor object never
    // keeps another slot's presentation. Only swaps and allocation-free copies follow.
    m_renderObjects.swap(objects);
    m_transformDirty.swap(dirty);
    m_changedTransforms.swap(changed);
    m_changedPresentationTransforms.clear();
    m_physicsPresentationActive = false;
    ++m_structureRevision;

    if(!hasEdges)
    {
        m_hierarchy = {};
        return true;
    }

    m_hierarchy = std::move(topology);
    m_hierarchyWorlds.swap(worlds);
    m_hierarchyLocalOverrides.swap(overrides);
    m_hierarchyHasChildren.swap(hasChildren);
    CommitHierarchyWorlds(true);
    return true;
}

void Scene::SetRenderObjectTransform(
    RenderObjectID id,
    const Transform& transform)
{
    if (id >= m_renderObjects.size())
    {
        return;
    }

    // Validate composition before changing authored scale while a pose is active.
    // A failed edit preserves both authored and presented state.
    if (m_renderObjects[id].presentationPose)
    {
        try
        {
            Physics::ComposeRuntimeModelMatrix(*m_renderObjects[id].presentationPose, transform.scale);
        }
        catch (const std::exception&)
        {
            return;
        }
    }

    // Every descendant world is evaluated against the candidate local before it is accepted.
    if (HasHierarchy())
    {
        m_hierarchyLocalOverrides[id] = &transform;
        const bool valid = EvaluateHierarchyEdit(nullptr);
        m_hierarchyLocalOverrides[id] = nullptr;

        if (!valid)
        {
            return;
        }
    }

    m_renderObjects[id].transform = transform;
    MarkTransformDirty(id);

    if (HasHierarchy())
    {
        CommitHierarchyWorlds(false);
    }
}

bool Scene::SetRenderObjectTransforms(std::span<const RenderObjectTransformUpdate> updates)
{
    const auto finite = [](const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    for(size_t index = 0; index < updates.size(); ++index)
    {
        const auto& update = updates[index];
        if(update.id >= m_renderObjects.size() || !finite(update.transform.position) ||
            !finite(update.transform.rotation) || !finite(update.transform.scale))
        {
            return false;
        }
        if (m_renderObjects[update.id].presentationPose)
        {
            try
            {
                Physics::ComposeRuntimeModelMatrix(*m_renderObjects[update.id].presentationPose, update.transform.scale);
            }
            catch (const std::exception&)
            {
                return false;
            }
        }
        for(size_t previous = 0; previous < index; ++previous)
        {
            if(updates[previous].id == update.id) return false;
        }
    }
    m_changedTransforms.reserve(m_changedTransforms.size() + updates.size());

    if(!HasHierarchy())
    {
        for(const auto& update : updates)
        {
            SetRenderObjectTransform(update.id, update.transform);
        }

        return true;
    }

    // One evaluation covers the whole batch, so a group edit is accepted or rejected as a unit.
    for(const auto& update : updates)
    {
        m_hierarchyLocalOverrides[update.id] = &update.transform;
    }

    const bool valid = EvaluateHierarchyEdit(nullptr);

    for(const auto& update : updates)
    {
        m_hierarchyLocalOverrides[update.id] = nullptr;
    }

    if(!valid)
    {
        return false;
    }

    for(const auto& update : updates)
    {
        auto& local = m_renderObjects[update.id].transform;

        if(std::memcmp(&local, &update.transform, sizeof(Transform)) != 0)
        {
            local = update.transform;
            MarkTransformDirty(update.id);
        }
    }

    CommitHierarchyWorlds(false);
    return true;
}

const std::vector<RenderObjectID>& Scene::GetChangedTransforms() const
{
    return m_changedTransforms;
}

void Scene::ClearChangedTransforms()
{
    // Called only after a successful render. Deferred frames never advance this history.
    for (auto& object : m_renderObjects)
    {
        object.presentationDirty = false;

        // Hierarchy history advances only here, once per successful render.
        if (object.hierarchyWorld)
        {
            object.previousHierarchyWorld = object.hierarchyWorld;
        }
        else
        {
            object.previousHierarchyWorld.reset();
        }

        if (object.previousPresentedModel)
        {
            if (object.presentationPose)
            {
                object.previousPresentedModel = object.GetPresentationTransform().currentModel;
            }
            else
            {
                object.previousPresentedModel.reset();
            }
        }
    }
    m_changedPresentationTransforms.clear();
    for (RenderObjectID id : m_changedTransforms)
    {
        m_renderObjects[id].previousTransform = m_renderObjects[id].transform;

        m_transformDirty[id] = false;
    }

    m_changedTransforms.clear();
}

bool Scene::PreparePresentationCommit(std::span<const std::optional<Physics::Pose>> poses)
{
    m_changedPresentationTransforms.reserve(m_renderObjects.size());

    if (!HasHierarchy())
    {
        return true;
    }

    // Prove every descendant of a posed root is valid before the noexcept commit.
    HierarchyInputs inputs;
    inputs.poses = poses;
    inputs.useCommittedPoses = false;
    return EvaluateHierarchy(m_renderObjects, m_hierarchy, m_hierarchyHasChildren, inputs, m_hierarchyWorlds, nullptr);
}

void Scene::CommitPresentation(std::span<const std::optional<Physics::Pose>> poses, bool active) noexcept
{
    for (RenderObjectID index = 0; index < m_renderObjects.size(); ++index)
    {
        auto& object = m_renderObjects[index];
        const auto before = object.GetPresentationTransform();
        const auto pose = poses.empty() ? std::optional<Physics::Pose>{} : poses[index];
        if (pose || object.presentationPose)
        {
            if (!object.previousPresentedModel)
            {
                object.previousPresentedModel = before.previousModel;
            }
        }
        object.presentationPose = pose;
        const auto after = object.GetPresentationTransform();
        const bool modelChanged = std::memcmp(before.currentModel.m, after.currentModel.m, sizeof(before.currentModel.m)) != 0;
        if (modelChanged && !object.presentationDirty)
        {
            object.presentationDirty = true;
            m_changedPresentationTransforms.push_back(index);
        }
    }
    m_physicsPresentationActive = active;

    // Descendants follow the newly accepted root presentation. Preparation proved this
    // evaluation; authored worlds stay valid by construction when poses are cleared.
    const bool descendantsValid = HasHierarchy() && EvaluateHierarchy(m_renderObjects, m_hierarchy,
        m_hierarchyHasChildren, HierarchyInputs{}, m_hierarchyWorlds, nullptr);

    if (descendantsValid)
    {
        CommitHierarchyWorlds(false);
    }
}

uint64_t Scene::GetStructureRevision() const
{
    return m_structureRevision;
}

std::vector<RenderObject>& Scene::GetRenderObjects()
{
    return m_renderObjects;
}

const std::vector<RenderObject>& Scene::GetRenderObjects() const
{
    return m_renderObjects;
}

Camera& Scene::GetCamera()
{
    return m_camera;
}

const Camera& Scene::GetCamera() const
{
    return m_camera;
}

DirectionalLight& Scene::GetDirectionalLight()
{
    return m_directionalLight;
}

const DirectionalLight& Scene::GetDirectionalLight() const
{
    return m_directionalLight;
}

bool Scene::HasRenderableMeshes() const noexcept
{
    return std::any_of(m_renderObjects.begin(), m_renderObjects.end(), [](const RenderObject& object)
    {
        return object.mesh != nullptr;
    });
}

void Scene::MarkTransformDirty(RenderObjectID id) noexcept
{
    // Capacity for every slot is reserved before hierarchy commits, so this cannot allocate there.
    if (!m_transformDirty[id])
    {
        m_transformDirty[id] = true;
        m_changedTransforms.push_back(id);
    }
}

bool Scene::ValidateHierarchyTopology(const SceneHierarchyTopology& topology, size_t count,
    std::vector<bool>& hasChildren, bool& hasEdges, std::string* error)
{
    const auto fail = [&](const char* message)
    {
        if (error)
        {
            *error = message;
        }

        return false;
    };

    hasEdges = false;

    if (topology.parentSlots.size() != count || topology.parentBeforeChild.size() != count)
    {
        return fail("Hierarchy topology does not cover every runtime slot.");
    }

    // The order must be a permutation that visits every parent before its children.
    constexpr size_t kUnvisited = (std::numeric_limits<size_t>::max)();
    std::vector<size_t> position(count, kUnvisited);

    for (size_t order = 0; order < count; ++order)
    {
        const RenderObjectID slot = topology.parentBeforeChild[order];

        if (slot >= count || position[slot] != kUnvisited)
        {
            return fail("Hierarchy evaluation order is not a permutation of runtime slots.");
        }

        position[slot] = order;
    }

    std::vector<bool> candidate(count, false);

    for (RenderObjectID slot = 0; slot < count; ++slot)
    {
        const auto parent = topology.parentSlots[slot];

        if (!parent)
        {
            continue;
        }

        if (*parent >= count || *parent == slot || position[*parent] >= position[slot])
        {
            return fail("Hierarchy evaluation order does not place every parent before its child.");
        }

        candidate[*parent] = true;
        hasEdges = true;
    }

    hasChildren.swap(candidate);
    return true;
}

bool Scene::EvaluateHierarchy(std::span<const RenderObject> objects, const SceneHierarchyTopology& topology,
    const std::vector<bool>& hasChildren, const HierarchyInputs& inputs, std::vector<Matrix4>& worlds,
    std::string* error) noexcept
{
    const auto fail = [&](const char* message)
    {
        if (error)
        {
            try
            {
                *error = message;
            }
            catch (...)
            {
            }
        }

        return false;
    };

    const size_t count = objects.size();
    const bool overridesMatch = !inputs.localOverrides || inputs.localOverrides->size() == count;

    if (topology.parentSlots.size() != count || topology.parentBeforeChild.size() != count ||
        worlds.size() != count || hasChildren.size() != count || !overridesMatch)
    {
        return fail("Hierarchy topology does not match the scene's runtime slots.");
    }

    for (const RenderObjectID slot : topology.parentBeforeChild)
    {
        const RenderObject& object = objects[slot];
        const Transform* overrideLocal = inputs.localOverrides ? (*inputs.localOverrides)[slot] : nullptr;
        const Transform& local = overrideLocal ? *overrideLocal : object.transform;
        std::optional<Physics::Pose> pose;

        if (inputs.useCommittedPoses)
        {
            pose = object.presentationPose;
        }
        else if (!inputs.poses.empty())
        {
            pose = inputs.poses[slot];
        }

        Matrix4& world = worlds[slot];
        const auto parent = topology.parentSlots[slot];
        const char* failure = nullptr;

        if (!parent)
        {
            // Roots keep their existing composition exactly; a simulated root uses its accepted pose.
            if (pose)
            {
                try
                {
                    world = Physics::ComposeRuntimeModelMatrix(*pose, local.scale);
                }
                catch (...)
                {
                    return fail("A hierarchy root has an invalid accepted pose.");
                }
            }
            else
            {
                world = local.GetModelMatrix();
            }
        }
        else if (pose)
        {
            failure = "Simulation poses are root-only; a parented object received one.";
        }
        else
        {
            failure = SceneHierarchy::EvaluateChildWorld(worlds[*parent], local, world);
        }

        if (!failure && hasChildren[slot])
        {
            failure = SceneHierarchy::ValidateParentWorld(world);
        }

        if (failure)
        {
            return fail(failure);
        }
    }

    return true;
}

bool Scene::EvaluateHierarchyEdit(std::string* error) noexcept
{
    // Authored worlds must stay valid so that clearing poses (Stop) can never fail.
    HierarchyInputs authored;
    authored.localOverrides = &m_hierarchyLocalOverrides;
    authored.useCommittedPoses = false;

    if (!EvaluateHierarchy(m_renderObjects, m_hierarchy, m_hierarchyHasChildren, authored, m_hierarchyWorlds, error))
    {
        return false;
    }

    const bool posesPresented = std::any_of(m_renderObjects.begin(), m_renderObjects.end(),
        [](const RenderObject& object)
        {
            return object.presentationPose.has_value();
        });

    if (!posesPresented)
    {
        return true;
    }

    // Leaves the rendered candidate in m_hierarchyWorlds for the commit.
    HierarchyInputs rendered;
    rendered.localOverrides = &m_hierarchyLocalOverrides;
    return EvaluateHierarchy(m_renderObjects, m_hierarchy, m_hierarchyHasChildren, rendered, m_hierarchyWorlds, error);
}

void Scene::CommitHierarchyWorlds(bool initializeHistory) noexcept
{
    for (RenderObjectID slot = 0; slot < m_renderObjects.size(); ++slot)
    {
        RenderObject& object = m_renderObjects[slot];

        if (!m_hierarchy.parentSlots[slot])
        {
            // A detached object keeps its last presented world as previous until the next render.
            if (object.hierarchyWorld)
            {
                object.previousHierarchyWorld = object.GetPresentationTransform().previousModel;
                object.hierarchyWorld.reset();
                MarkTransformDirty(slot);
            }

            if (initializeHistory)
            {
                object.previousHierarchyWorld.reset();
            }

            continue;
        }

        const Matrix4& world = m_hierarchyWorlds[slot];

        if (!object.hierarchyWorld)
        {
            // Continue from the object's last presented history rather than its local matrix.
            object.previousHierarchyWorld = object.GetPresentationTransform().previousModel;
            object.hierarchyWorld = world;
            MarkTransformDirty(slot);
        }
        else if (std::memcmp(object.hierarchyWorld->m, world.m, sizeof(world.m)) != 0)
        {
            object.hierarchyWorld = world;
            MarkTransformDirty(slot);
        }

        if (initializeHistory)
        {
            object.previousHierarchyWorld = world;
        }
    }
}

bool Scene::InstallHierarchy(SceneHierarchyTopology topology, bool initializeHistory, std::string* error,
    std::span<const RenderObjectTransformUpdate> updates)
{
    std::vector<bool> hasChildren;
    bool hasEdges = false;

    if (!ValidateHierarchyTopology(topology, m_renderObjects.size(), hasChildren, hasEdges, error))
    {
        return false;
    }

    const size_t count = m_renderObjects.size();
    std::vector<Matrix4> worlds(count);
    std::vector<const Transform*> overrides(count, nullptr);
    m_changedTransforms.reserve(count);

    for(const auto& update : updates)
    {
        if(update.id >= count || overrides[update.id])
        {
            if(error)
            {
                *error = "Hierarchy local update contains an invalid or repeated runtime slot.";
            }

            return false;
        }

        overrides[update.id] = &update.transform;
    }

    HierarchyInputs authored;
    authored.useCommittedPoses = false;
    authored.localOverrides = &overrides;
    HierarchyInputs presented;
    presented.localOverrides = &overrides;

    if(!EvaluateHierarchy(m_renderObjects, topology, hasChildren, authored, worlds, error) ||
        !EvaluateHierarchy(m_renderObjects, topology, hasChildren, presented, worlds, error))
    {
        return false;
    }

    // Every allocation and evaluation precedes the combined local/topology publication.
    // Keep the old world as presentation history when detaching a child.
    if(!hasEdges)
    {
        ClearHierarchy();
    }

    for(const auto& update : updates)
    {
        auto& local = m_renderObjects[update.id].transform;

        if(std::memcmp(&local, &update.transform, sizeof(Transform)) != 0)
        {
            local = update.transform;
            MarkTransformDirty(update.id);
        }
    }

    std::fill(overrides.begin(), overrides.end(), nullptr);

    if(!hasEdges)
    {
        return true;
    }

    // Commit point: only swaps and allocation-free copies follow.
    m_hierarchy = std::move(topology);
    m_hierarchyWorlds.swap(worlds);
    m_hierarchyLocalOverrides.swap(overrides);
    m_hierarchyHasChildren.swap(hasChildren);
    CommitHierarchyWorlds(initializeHistory);
    return true;
}

void Scene::ClearHierarchy() noexcept
{
    if (!HasHierarchy())
    {
        return;
    }

    // Detaching every slot reuses the commit path, which preserves presented history.
    for (auto& parent : m_hierarchy.parentSlots)
    {
        parent.reset();
    }

    CommitHierarchyWorlds(false);
    m_hierarchy.parentSlots.clear();
    m_hierarchy.parentBeforeChild.clear();
    m_hierarchy.hierarchyRevision = 0;
}
